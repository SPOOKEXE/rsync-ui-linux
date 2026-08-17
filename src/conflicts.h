#pragma once

#include <ctime>
#include <cstdint>
#include <string>
#include <vector>

// What a dry run says will happen to something that already exists in the
// destination. New files are not conflicts and never appear here.
enum class ConflictKind {
    Overwrite,  // destination file exists and rsync would replace it
    Delete,     // destination file has no source counterpart and --delete would remove it
};

struct Conflict {
    ConflictKind kind = ConflictKind::Overwrite;
    std::string path;     // relative to the transfer root, exactly as rsync printed it
    bool allow = true;    // UI choice: overwrite it, or allow the deletion

    // Filled in by fillConflictDetails so the user has something to decide with.
    uint64_t srcSize = 0;
    uint64_t dstSize = 0;
    std::time_t srcMtime = 0;
    std::time_t dstMtime = 0;
    bool srcKnown = false;
    bool dstKnown = false;
};

// Reads one --itemize-changes line. Returns false for anything that is not a
// conflict: new files, unchanged files, progress output, directory timestamps.
bool parseItemizeLine(const std::string& line, Conflict& out);

// Escapes the wildcards rsync honours in filter rules (* ? [ and backslash).
// Without this, skipping "weird[1].txt" silently skips "weird1.txt" instead.
std::string escapeFilterPattern(const std::string& path);

// Writes an --exclude-from list of anchored, escaped paths to a temp file and
// returns its path, or an empty string if it could not be written. The caller
// owns the file and should remove it once rsync has finished.
std::string writeSkipFile(const std::vector<std::string>& paths);

// Looks up size and mtime on both sides. sourcePath is the job's source and
// destRoot its destination, since rsync's paths are relative to the transfer
// root, which holds the source's own basename.
void fillConflictDetails(const std::string& sourcePath, const std::string& destRoot,
                         Conflict& c);

std::string formatSize(uint64_t bytes);
std::string formatTime(std::time_t t);
