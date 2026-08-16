#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rsync_job.h"

// Runs queued rsync jobs one at a time on a single background thread.
// Every accessor is safe to call from the UI thread; all shared state lives
// behind one mutex, and the UI reads copies rather than references.
class JobQueue {
public:
    JobQueue();
    ~JobQueue();

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Appends jobs as Pending and assigns each an id. Starts the worker if idle.
    void enqueue(std::vector<Job> jobs);

    // Snapshots for rendering. Cheap enough at this scale to copy every frame.
    std::vector<Job> jobs();
    std::vector<std::string> log();

    void cancelCurrent();  // SIGTERM the running rsync, leave the rest of the queue alone
    void cancelAll();      // cancel the running job and mark everything pending as cancelled
    void clearFinished();  // drop done/failed/cancelled rows
    void clearLog();

    bool busy();  // true while anything is running or waiting to run

private:
    void worker();
    Job* findById(int id);                        // caller must hold mutex_
    void appendLogLocked(const std::string& line);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Job> jobs_;
    std::deque<std::string> log_;
    int nextId_ = 1;
    int runningId_ = 0;
    pid_t childPid_ = -1;
    bool cancelRequested_ = false;
    bool stop_ = false;
    std::thread thread_;
};

// Cross-products sources and destinations into the job list the queue runs.
struct SourceEntry {
    std::string path;
    bool recursive = true;
};
std::vector<Job> buildJobs(const std::vector<SourceEntry>& sources,
                           const std::vector<std::string>& dests, const JobOptions& opts);
