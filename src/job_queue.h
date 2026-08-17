#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rsync_job.h"

// Runs queued rsync jobs on a pool of worker threads, at most maxParallel at a
// time. Every accessor is safe to call from the UI thread; all shared state
// lives behind one mutex and the UI reads copies rather than references.
class JobQueue {
public:
    static constexpr int kMaxParallel = 8;

    JobQueue();
    ~JobQueue();

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Appends jobs as Pending and assigns each a fresh id.
    void enqueue(std::vector<Job> jobs);

    // Snapshots for rendering and for saving the session.
    std::vector<Job> jobs();
    std::vector<std::string> log();

    void cancelJob(int id);  // SIGTERM one job's process group
    void cancelAll();        // cancel everything running and drop everything pending
    void clearFinished();    // drop done/failed/cancelled rows
    void clearLog();

    // Answers a job waiting in Review. Conflicts with allow=false become a skip
    // list; the job then re-runs live without scanning again.
    void resolveJob(int id, const std::vector<Conflict>& decisions);

    // Pausing SIGSTOPs every running rsync and stops workers picking up new jobs.
    void setPaused(bool paused);
    bool paused();

    void setMaxParallel(int n);  // clamped to [1, kMaxParallel]
    int maxParallel();

    bool busy();  // true while anything is running or waiting to run

    // Bumped on every mutation, so callers can cheaply notice the queue changed.
    uint64_t generation();

private:
    // How one rsync invocation ended. Cancellation is reported separately because
    // rsync exits non-zero precisely because it was killed.
    struct PhaseResult {
        bool ok = false;
        bool cancelled = false;
        int exitCode = -1;
    };

    void worker();
    RsyncCallbacks makeCallbacks(int id, std::vector<Conflict>* collect);
    PhaseResult runPhase(const Job& job, RunMode mode, int id, std::vector<Conflict>* collect);
    void finishLocked(int id, JobState state, const std::string& note);
    bool canStartLocked() const;
    Job* firstPendingLocked();
    Job* findByIdLocked(int id);
    void appendLogLocked(const std::string& line);
    void signalActiveLocked(int sig);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Job> jobs_;
    std::deque<std::string> log_;
    std::vector<std::thread> threads_;
    int nextId_ = 1;
    int runningCount_ = 0;
    int maxParallel_ = 1;
    bool paused_ = false;
    bool stop_ = false;
    uint64_t generation_ = 1;
};

// Cross-products sources and destinations into the job list the queue runs.
struct SourceEntry {
    std::string path;
    bool recursive = true;
};
std::vector<Job> buildJobs(const std::vector<SourceEntry>& sources,
                           const std::vector<std::string>& dests, const JobOptions& opts);
