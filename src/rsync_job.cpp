#include "rsync_job.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

const char* jobStateName(JobState s) {
    switch (s) {
        case JobState::Pending: return "pending";
        case JobState::Running: return "running";
        case JobState::Done: return "done";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "?";
}

std::vector<std::string> splitArgs(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuotes = false;
    bool started = false;
    for (char c : text) {
        if (c == '"') {
            inQuotes = !inQuotes;
            started = true;
        } else if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) {
            if (started) out.push_back(cur);
            cur.clear();
            started = false;
        } else {
            cur.push_back(c);
            started = true;
        }
    }
    if (started) out.push_back(cur);
    return out;
}

std::string joinArgs(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out += ' ';
        bool needQuote = args[i].find_first_of(" \t\"'") != std::string::npos;
        if (needQuote) out += '"';
        out += args[i];
        if (needQuote) out += '"';
    }
    return out;
}

std::vector<std::string> buildRsyncArgs(const Job& job) {
    std::vector<std::string> args{"rsync", "--info=progress2", "--no-inc-recursive"};

    if (job.opts.archive) args.push_back("-a");

    // --no-inc-recursive keeps the progress2 percentage honest: without it rsync
    // discovers files while transferring and the bar resets as the total grows.
    args.push_back("-r");
    if (!job.recursive) {
        // Non-recursive means "the source's own files, not its subdirectories".
        // Exclude patterns are anchored at the transfer root, which holds exactly
        // one entry (the source), so /*/*/ is precisely one level below it.
        // Plain -d would instead copy the directory and leave it empty.
        args.push_back("--exclude=/*/*/");
    }

    if (job.opts.dryRun) args.push_back("-n");
    if (job.opts.deleteExtra) args.push_back("--delete");
    if (job.opts.compress) args.push_back("-z");
    if (job.opts.checksum) args.push_back("-c");

    for (auto& extra : splitArgs(job.opts.extraArgs)) args.push_back(extra);

    // No trailing slash on the source, so "copy /a/photos into /b" lands at
    // /b/photos rather than dumping the contents into /b.
    args.push_back(job.source);

    std::string dest = job.dest;
    if (dest.empty() || dest.back() != '/') dest += '/';
    args.push_back(dest);

    return args;
}

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// Recognises an rsync --info=progress2 line, which looks like:
//   1,234,567  45%   12.34MB/s    0:00:12 (xfr#3, to-chk=10/20)
bool parseProgress(const std::string& line, float& pct, std::string& transferred,
                   std::string& speed, std::string& eta) {
    std::vector<std::string> tok;
    std::string cur;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) tok.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tok.push_back(cur);

    if (tok.size() < 4) return false;
    if (tok[1].size() < 2 || tok[1].back() != '%') return false;
    for (char c : tok[0]) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != ',' && c != '.') return false;
    }

    pct = static_cast<float>(std::atoi(tok[1].c_str())) / 100.0f;
    transferred = tok[0];
    speed = tok[2];
    eta = tok[3];
    return true;
}

}  // namespace

int runRsync(const Job& job, const RsyncCallbacks& cb) {
    std::vector<std::string> args = buildRsyncArgs(job);

    // execvp wants a NULL-terminated char* array. Going through argv directly
    // (no shell) means paths with spaces, quotes or $ need no escaping at all.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execvp("rsync", argv.data());
        _exit(127);  // rsync not on PATH
    }

    close(fds[1]);
    if (cb.onStarted) cb.onStarted(pid);

    // rsync separates progress updates with \r and everything else with \n,
    // so both count as line terminators here.
    std::string pending;
    char buf[4096];
    ssize_t n;
    auto flush = [&]() {
        std::string line = trim(pending);
        pending.clear();
        if (line.empty()) return;
        float pct = 0.0f;
        std::string transferred, speed, eta;
        if (parseProgress(line, pct, transferred, speed, eta)) {
            if (cb.onProgress) cb.onProgress(pct, transferred, speed, eta);
        } else if (cb.onLine) {
            cb.onLine(line);
        }
    };

    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\r' || c == '\n') {
                flush();
            } else {
                pending.push_back(c);
            }
        }
    }
    flush();
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}
