#include "job_queue.h"

#include <signal.h>

#include <algorithm>

namespace {
constexpr size_t kMaxLogLines = 2000;

std::string tag(int id) { return "[" + std::to_string(id) + "] "; }
}  // namespace

JobQueue::JobQueue() {
    threads_.reserve(kMaxParallel);
    for (int i = 0; i < kMaxParallel; ++i) threads_.emplace_back(&JobQueue::worker, this);
}

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
        for (auto& j : jobs_) {
            if (j.state == JobState::Running) j.cancelRequested = true;
        }
        // A stopped process ignores SIGTERM until it runs again, so continue
        // first or a paused job would keep the worker blocked forever.
        signalRunningLocked(SIGCONT);
        signalRunningLocked(SIGTERM);
    }
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

void JobQueue::enqueue(std::vector<Job> jobs) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& j : jobs) {
            j.id = nextId_++;
            j.state = JobState::Pending;
            j.pid = 0;
            j.cancelRequested = false;
            jobs_.push_back(std::move(j));
        }
        ++generation_;
    }
    cv_.notify_all();
}

std::vector<Job> JobQueue::jobs() {
    std::lock_guard<std::mutex> lk(mutex_);
    return jobs_;
}

std::vector<std::string> JobQueue::log() {
    std::lock_guard<std::mutex> lk(mutex_);
    return {log_.begin(), log_.end()};
}

void JobQueue::cancelJob(int id) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        Job* j = findByIdLocked(id);
        if (!j) return;
        if (j->state == JobState::Pending) {
            j->state = JobState::Cancelled;
        } else if (j->state == JobState::Running) {
            j->cancelRequested = true;
            if (j->pid > 0) {
                kill(-j->pid, SIGCONT);
                kill(-j->pid, SIGTERM);
            }
        }
        ++generation_;
    }
    cv_.notify_all();
}

void JobQueue::cancelAll() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& j : jobs_) {
            if (j.state == JobState::Pending) j.state = JobState::Cancelled;
            if (j.state == JobState::Running) j.cancelRequested = true;
        }
        signalRunningLocked(SIGCONT);
        signalRunningLocked(SIGTERM);
        ++generation_;
    }
    cv_.notify_all();
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
    ++generation_;
}

void JobQueue::clearLog() {
    std::lock_guard<std::mutex> lk(mutex_);
    log_.clear();
}

void JobQueue::setPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (paused_ == paused) return;
        paused_ = paused;
        signalRunningLocked(paused ? SIGSTOP : SIGCONT);
        appendLogLocked(paused ? "queue paused" : "queue resumed");
        ++generation_;
    }
    cv_.notify_all();
}

bool JobQueue::paused() {
    std::lock_guard<std::mutex> lk(mutex_);
    return paused_;
}

void JobQueue::setMaxParallel(int n) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        n = std::clamp(n, 1, kMaxParallel);
        if (n == maxParallel_) return;
        maxParallel_ = n;
        ++generation_;
    }
    cv_.notify_all();
}

int JobQueue::maxParallel() {
    std::lock_guard<std::mutex> lk(mutex_);
    return maxParallel_;
}

bool JobQueue::busy() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& j : jobs_) {
        if (j.state == JobState::Pending || j.state == JobState::Running) return true;
    }
    return false;
}

uint64_t JobQueue::generation() {
    std::lock_guard<std::mutex> lk(mutex_);
    return generation_;
}

// Jobs are addressed by id rather than index because clearFinished() can erase
// rows while jobs are running, which would invalidate any cached index.
Job* JobQueue::findByIdLocked(int id) {
    for (auto& j : jobs_) {
        if (j.id == id) return &j;
    }
    return nullptr;
}

Job* JobQueue::firstPendingLocked() {
    for (auto& j : jobs_) {
        if (j.state == JobState::Pending) return &j;
    }
    return nullptr;
}

void JobQueue::signalRunningLocked(int sig) {
    for (auto& j : jobs_) {
        if (j.state == JobState::Running && j.pid > 0) kill(-j.pid, sig);
    }
}

// Decides whether a worker may claim the next pending job. Jobs are always taken
// in order, so serialising --delete here never reorders the queue, it only makes
// a deleting job wait for an empty field and hold it until it finishes.
bool JobQueue::canStartLocked() const {
    if (paused_ || runningCount_ >= maxParallel_) return false;

    const Job* next = nullptr;
    bool deleteRunning = false;
    for (const auto& j : jobs_) {
        if (!next && j.state == JobState::Pending) next = &j;
        if (j.state == JobState::Running && j.opts.deleteExtra) deleteRunning = true;
    }
    if (!next) return false;

    // Two rsyncs deleting inside the same destination can race and remove each
    // other's freshly written files, so a --delete job always runs alone.
    if (deleteRunning) return false;
    if (next->opts.deleteExtra && runningCount_ > 0) return false;
    return true;
}

void JobQueue::appendLogLocked(const std::string& line) {
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) log_.pop_front();
}

void JobQueue::worker() {
    for (;;) {
        Job current;
        int id = 0;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || canStartLocked(); });
            if (stop_) return;

            Job* next = firstPendingLocked();
            if (!next) continue;

            next->state = JobState::Running;
            next->percent = 0.0f;
            next->cancelRequested = false;
            next->pid = 0;
            current = *next;
            id = next->id;
            ++runningCount_;
            ++generation_;
            appendLogLocked(tag(id) + "$ " + joinArgs(buildRsyncArgs(current)));
        }

        RsyncCallbacks cb;
        cb.onStarted = [this, id](pid_t pid) {
            std::lock_guard<std::mutex> lk(mutex_);
            Job* j = findByIdLocked(id);
            if (!j) return;
            j->pid = pid;
            // A pause or cancel that landed between claiming the job and the fork
            // would otherwise be lost, so apply it as soon as we have a pid.
            if (j->cancelRequested) {
                kill(-pid, SIGTERM);
            } else if (paused_) {
                kill(-pid, SIGSTOP);
            }
        };
        cb.onProgress = [this, id](float pct, const std::string& transferred,
                                   const std::string& speed, const std::string& eta) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (Job* j = findByIdLocked(id)) {
                j->percent = pct;
                j->transferred = transferred;
                j->speed = speed;
                j->eta = eta;
                ++generation_;
            }
        };
        cb.onLine = [this, id](const std::string& line) {
            std::lock_guard<std::mutex> lk(mutex_);
            appendLogLocked(tag(id) + line);
            if (Job* j = findByIdLocked(id)) j->error = line;
        };

        int code = runRsync(current, cb);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            --runningCount_;
            if (Job* j = findByIdLocked(id)) {
                j->pid = 0;
                j->exitCode = code;
                if (j->cancelRequested) {
                    j->state = JobState::Cancelled;
                    appendLogLocked(tag(id) + "cancelled");
                } else if (code == 0) {
                    j->state = JobState::Done;
                    j->percent = 1.0f;
                    j->error.clear();
                    appendLogLocked(tag(id) + "done");
                } else {
                    j->state = JobState::Failed;
                    if (code == 127) j->error = "rsync not found on PATH";
                    appendLogLocked(tag(id) + "failed, exit " + std::to_string(code));
                }
                j->cancelRequested = false;
            }
            ++generation_;
            if (stop_) return;
        }
        cv_.notify_all();
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
