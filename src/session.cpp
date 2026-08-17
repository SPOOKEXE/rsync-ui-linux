#include "session.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (s[++i]) {
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case '\\': out += '\\'; break;
            default: out += s[i];
        }
    }
    return out;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

const char* b(bool v) { return v ? "1" : "0"; }
bool toBool(const std::string& s) { return s == "1"; }

// Options occupy a fixed run of six flags, the conflict policy, then the
// free-text extra args, shared by the "opt" line and every "job" line.
void writeOpts(std::ostringstream& os, const JobOptions& o) {
    os << '\t' << b(o.archive) << '\t' << b(o.dryRun) << '\t' << b(o.deleteExtra) << '\t'
       << b(o.compress) << '\t' << b(o.checksum) << '\t' << b(o.partial) << '\t'
       << static_cast<int>(o.onConflict) << '\t' << escape(o.extraArgs);
}

// Reads those eight fields starting at `at`. Returns false if the line is short.
bool readOpts(const std::vector<std::string>& f, size_t at, JobOptions& o) {
    if (f.size() < at + 8) return false;
    o.archive = toBool(f[at]);
    o.dryRun = toBool(f[at + 1]);
    o.deleteExtra = toBool(f[at + 2]);
    o.compress = toBool(f[at + 3]);
    o.checksum = toBool(f[at + 4]);
    o.partial = toBool(f[at + 5]);
    const int policy = std::atoi(f[at + 6].c_str());
    o.onConflict = policy >= 0 && policy <= 2 ? static_cast<ConflictPolicy>(policy)
                                              : ConflictPolicy::WorkAround;
    o.extraArgs = unescape(f[at + 7]);
    return true;
}

}  // namespace

std::string sessionPath() {
    fs::path base;
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) {
        base = state;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = fs::path(home) / ".local" / "state";
    } else {
        base = ".";
    }
    return (base / "rsync-ui" / "session.tsv").string();
}

std::string serializeSession(const SessionData& d) {
    std::ostringstream os;
    os << "v2\n";

    os << "opt";
    writeOpts(os, d.opts);
    os << '\n';

    os << "par\t" << d.maxParallel << '\n';
    os << "drop\t" << escape(d.dropFolder) << '\t' << b(d.dropStraightToFolder) << '\t'
       << b(d.dropRecursive) << '\n';

    for (const auto& s : d.sources) {
        os << "src\t" << escape(s.path) << '\t' << b(s.recursive) << '\n';
    }
    for (const auto& dst : d.dests) {
        os << "dst\t" << escape(dst) << '\n';
    }
    for (const auto& j : d.jobs) {
        os << "job\t" << escape(j.source) << '\t' << escape(j.dest) << '\t' << b(j.recursive);
        writeOpts(os, j.opts);
        os << '\n';
    }
    return os.str();
}

SessionData parseSession(const std::string& text) {
    SessionData d;
    std::istringstream is(text);
    std::string line;
    if (!std::getline(is, line) || line != "v2") return d;

    while (std::getline(is, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = splitTabs(line);
        const std::string& kind = f[0];

        if (kind == "opt") {
            readOpts(f, 1, d.opts);
        } else if (kind == "par" && f.size() >= 2) {
            d.maxParallel = std::atoi(f[1].c_str());
            if (d.maxParallel < 1 || d.maxParallel > JobQueue::kMaxParallel) d.maxParallel = 1;
        } else if (kind == "drop" && f.size() >= 4) {
            d.dropFolder = unescape(f[1]);
            d.dropStraightToFolder = toBool(f[2]);
            d.dropRecursive = toBool(f[3]);
        } else if (kind == "src" && f.size() >= 3) {
            d.sources.push_back({unescape(f[1]), toBool(f[2])});
        } else if (kind == "dst" && f.size() >= 2) {
            d.dests.push_back(unescape(f[1]));
        } else if (kind == "job" && f.size() >= 4) {
            Job j;
            j.source = unescape(f[1]);
            j.dest = unescape(f[2]);
            j.recursive = toBool(f[3]);
            if (!readOpts(f, 4, j.opts)) continue;
            d.jobs.push_back(std::move(j));
        }
    }
    return d;
}

bool saveSessionIfChanged(const std::string& path, const std::string& text, size_t& lastHash) {
    const size_t hash = std::hash<std::string>{}(text);
    if (hash == lastHash) return false;

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    // Write to a sibling then rename, so a crash mid-write cannot leave a
    // truncated session file behind.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) return false;

    lastHash = hash;
    return true;
}

std::string readFileOrEmpty(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}
