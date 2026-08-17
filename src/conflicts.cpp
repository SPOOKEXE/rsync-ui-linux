#include "conflicts.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// rsync pads its change field to 11 characters and follows it with a space, for
// both ">f.st......" and "*deleting", so the path always starts at the same offset.
constexpr size_t kPathOffset = 12;
constexpr const char* kAllNew = "+++++++++";

}  // namespace

bool parseItemizeLine(const std::string& line, Conflict& out) {
    if (line.size() <= kPathOffset) return false;

    if (line.rfind("*deleting", 0) == 0) {
        out.kind = ConflictKind::Delete;
        out.path = line.substr(kPathOffset);
        out.allow = true;
        return !out.path.empty();
    }

    const char update = line[0];
    const char type = line[1];
    // '>' received, 'c' created locally. '.' means nothing changed, '<' is the
    // sending side, which a local copy never reports.
    if (update != '>' && update != 'c') return false;
    // Only regular files and symlinks. A directory whose timestamp shifts is not
    // what anyone means by a conflict.
    if (type != 'f' && type != 'L') return false;

    const std::string flags = line.substr(2, 9);
    if (flags == kAllNew) return false;  // brand new item, nothing to overwrite

    out.kind = ConflictKind::Overwrite;
    out.path = line.substr(kPathOffset);
    out.allow = true;
    return !out.path.empty();
}

std::string escapeFilterPattern(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '\\' || c == '*' || c == '?' || c == '[') out += '\\';
        out += c;
    }
    return out;
}

std::string writeSkipFile(const std::vector<std::string>& paths) {
    char tmpl[] = "/tmp/rsync-ui-skip-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd < 0) return {};
    close(fd);

    std::ofstream out(tmpl, std::ios::trunc);
    if (!out) {
        ::remove(tmpl);
        return {};
    }
    for (const auto& p : paths) {
        // Anchored at the transfer root so the rule matches this path and no other.
        out << '/' << escapeFilterPattern(p) << '\n';
    }
    if (!out) {
        ::remove(tmpl);
        return {};
    }
    return tmpl;
}

void fillConflictDetails(const std::string& sourcePath, const std::string& destRoot,
                         Conflict& c) {
    // rsync's paths start with the source's own basename, so the source side
    // resolves against the source's parent directory.
    const fs::path srcBase = fs::path(sourcePath).parent_path();

    struct stat st {};
    if (::stat((srcBase / c.path).c_str(), &st) == 0) {
        c.srcSize = static_cast<uint64_t>(st.st_size);
        c.srcMtime = st.st_mtime;
        c.srcKnown = true;
    }
    if (::stat((fs::path(destRoot) / c.path).c_str(), &st) == 0) {
        c.dstSize = static_cast<uint64_t>(st.st_size);
        c.dstMtime = st.st_mtime;
        c.dstKnown = true;
    }
}

std::string formatSize(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < sizeof(units) / sizeof(units[0])) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string formatTime(std::time_t t) {
    if (t == 0) return "-";
    std::tm tm {};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}
