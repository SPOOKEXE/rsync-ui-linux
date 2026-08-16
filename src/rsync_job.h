#pragma once

#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

// rsync flags shared by every job in a batch. Recursion is per-source, so it
// lives on Job rather than here.
struct JobOptions {
    bool archive = true;
    bool dryRun = false;
    bool deleteExtra = false;
    bool compress = false;
    bool checksum = false;
    // Keeps half-transferred files in a .rsync-partial directory so a re-run
    // continues them instead of starting over. This is what makes resume work.
    bool partial = true;
    std::string extraArgs;
};

enum class JobState { Pending, Running, Done, Failed, Cancelled };

const char* jobStateName(JobState s);

// One rsync invocation: a single source copied into a single destination directory.
// The live fields below are written by the queue worker and read by the UI thread,
// always under JobQueue's mutex.
struct Job {
    int id = 0;
    std::string source;
    std::string dest;
    bool recursive = true;
    JobOptions opts;

    JobState state = JobState::Pending;
    float percent = 0.0f;     // 0..1, parsed from rsync --info=progress2
    std::string transferred;  // bytes moved so far, verbatim from rsync
    std::string speed;
    std::string eta;
    int exitCode = -1;
    std::string error;  // last non-progress output line, shown when a job fails

    // Runtime-only bookkeeping owned by JobQueue, never persisted.
    // pid doubles as the process group id, see runRsync.
    pid_t pid = 0;
    bool cancelRequested = false;
};

// Builds the argv rsync is exec'd with. Exposed so the UI can show the exact command.
std::vector<std::string> buildRsyncArgs(const Job& job);

// Splits a raw extra-args string into tokens, honouring "double quoted" runs.
std::vector<std::string> splitArgs(const std::string& text);

// Joins argv into a shell-ish string for display only. Never fed back to a shell.
std::string joinArgs(const std::vector<std::string>& args);

struct RsyncCallbacks {
    // percent is 0..1; the string fields are copied verbatim from rsync's progress line.
    std::function<void(float percent, const std::string& transferred,
                       const std::string& speed, const std::string& eta)>
        onProgress;
    // Any output line that is not a progress update (file names, errors).
    std::function<void(const std::string& line)> onLine;
    // Hands the caller the child pid, which is also its process group id, so the
    // whole rsync tree can be stopped, continued or killed with kill(-pid, sig).
    std::function<void(pid_t)> onStarted;
};

// Runs rsync to completion in the calling thread. Returns the process exit code,
// 128+signal if it was killed, or -1 if the child could not be spawned.
int runRsync(const Job& job, const RsyncCallbacks& cb);
