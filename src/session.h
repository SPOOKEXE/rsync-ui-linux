#pragma once

#include <string>
#include <vector>

#include "job_queue.h"

// Everything worth surviving a restart. Jobs here are the unfinished ones only;
// they come back as Pending so rsync can resume them with --partial-dir.
struct SessionData {
    std::vector<SourceEntry> sources;
    std::vector<std::string> dests;
    JobOptions opts;
    std::string dropFolder;
    bool dropStraightToFolder = false;
    bool dropRecursive = true;
    int maxParallel = 1;
    std::vector<Job> jobs;
};

// $XDG_STATE_HOME/rsync-ui/session.tsv, falling back to ~/.local/state.
std::string sessionPath();

// Tab-separated, one record per line, with \\ \t and \n escaped. Deliberately
// not JSON: paths are the only tricky field and escaping three characters is
// less code than pulling in a parser.
std::string serializeSession(const SessionData& d);
SessionData parseSession(const std::string& text);

// Writes only when the text differs from what was last written, so an idle app
// does not churn the disk. lastHash is the caller's memory of the last write.
bool saveSessionIfChanged(const std::string& path, const std::string& text, size_t& lastHash);

std::string readFileOrEmpty(const std::string& path);
