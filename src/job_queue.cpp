#include "job_queue.h"

#include <signal.h>

#include <algorithm>

namespace {
constexpr size_t kMaxLogLines = 2000;
}

JobQueue::JobQueue() { thread_ = std::thread(&JobQueue::worker, this); }

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
        cancelRequested_ = true;
        if (childPid_ > 0) kill(childPid_, SIGTERM);
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void JobQueue::enqueue(std::vector<Job> jobs) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& j : jobs) {
            j.id = nextId_++;
            j.state = JobState::Pending;
            jobs_.push_back(std::move(j));
        }
    }
    cv_.notify_one();
}

std::vector<Job> JobQueue::jobs() {
    std::lock_guard<std::mutex> lk(mutex_);
    return jobs_;
}

std::vector<std::string> JobQueue::log() {
    std::lock_guard<std::mutex> lk(mutex_);
    return {log_.begin(), log_.end()};
}

void JobQueue::cancelCurrent() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (childPid_ > 0) {
        cancelRequested_ = true;
        kill(childPid_, SIGTERM);
    }
}

void JobQueue::cancelAll() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& j : jobs_) {
            if (j.state == JobState::Pending) j.state = JobState::Cancelled;
        }
        if (childPid_ > 0) {
            cancelRequested_ = true;
            kill(childPid_, SIGTERM);
        }
    }
    cv_.notify_one();
}

void JobQueue::clearFinished() {
    std::lock_guard<std::mutex> lk(mutex_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [](const Job& j) {
                                   return j.state == JobState::Done ||
                                          j.state == JobState::Failed ||
                                          j.state == JobState::Cancelled;
                               }),
                jobs_.end());
}

void JobQueue::clearLog() {
    std::lock_guard<std::mutex> lk(mutex_);
    log_.clear();
}

bool JobQueue::busy() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& j : jobs_) {
        if (j.state == JobState::Pending || j.state == JobState::Running) return true;
    }
    return false;
}

// Jobs are addressed by id rather than index because clearFinished() can erase
// rows while a job is running, which would invalidate any cached index.
Job* JobQueue::findById(int id) {
    for (auto& j : jobs_) {
        if (j.id == id) return &j;
    }
    return nullptr;
}

void JobQueue::appendLogLocked(const std::string& line) {
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) log_.pop_front();
}

void JobQueue::worker() {
    for (;;) {
        Job current;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] {
                if (stop_) return true;
                for (auto& j : jobs_) {
                    if (j.state == JobState::Pending) return true;
                }
                return false;
            });
            if (stop_) return;

            Job* next = nullptr;
            for (auto& j : jobs_) {
                if (j.state == JobState::Pending) {
                    next = &j;
                    break;
                }
            }
            if (!next) continue;

            next->state = JobState::Running;
            next->percent = 0.0f;
            current = *next;
            runningId_ = next->id;
            cancelRequested_ = false;
            appendLogLocked("$ " + joinArgs(buildRsyncArgs(current)));
        }

        RsyncCallbacks cb;
        cb.onStarted = [this](pid_t pid) {
            std::lock_guard<std::mutex> lk(mutex_);
            childPid_ = pid;
            // A cancel that landed between marking the job Running and the fork
            // would otherwise be lost, so honour it as soon as we have a pid.
            if (cancelRequested_) kill(pid, SIGTERM);
        };
        cb.onProgress = [this](float pct, const std::string& transferred,
                               const std::string& speed, const std::string& eta) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (Job* j = findById(runningId_)) {
                j->percent = pct;
                j->transferred = transferred;
                j->speed = speed;
                j->eta = eta;
            }
        };
        cb.onLine = [this](const std::string& line) {
            std::lock_guard<std::mutex> lk(mutex_);
            appendLogLocked(line);
            if (Job* j = findById(runningId_)) j->error = line;
        };

        int code = runRsync(current, cb);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            childPid_ = -1;
            bool cancelled = cancelRequested_;
            cancelRequested_ = false;
            if (Job* j = findById(runningId_)) {
                j->exitCode = code;
                if (cancelled) {
                    j->state = JobState::Cancelled;
                    appendLogLocked("job " + std::to_string(j->id) + " cancelled");
                } else if (code == 0) {
                    j->state = JobState::Done;
                    j->percent = 1.0f;
                    j->error.clear();
                    appendLogLocked("job " + std::to_string(j->id) + " done");
                } else {
                    j->state = JobState::Failed;
                    if (code == 127) j->error = "rsync not found on PATH";
                    appendLogLocked("job " + std::to_string(j->id) +
                                    " failed, exit " + std::to_string(code));
                }
            }
            runningId_ = 0;
            if (stop_) return;
        }
    }
}

std::vector<Job> buildJobs(const std::vector<SourceEntry>& sources,
                           const std::vector<std::string>& dests, const JobOptions& opts) {
    std::vector<Job> out;
    for (const auto& d : dests) {
        for (const auto& s : sources) {
            Job j;
            j.source = s.path;
            j.dest = d;
            j.recursive = s.recursive;
            j.opts = opts;
            out.push_back(std::move(j));
        }
    }
    return out;
}
