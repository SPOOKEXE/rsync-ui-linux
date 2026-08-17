#include "job_queue.h"

#include <signal.h>

#include <algorithm>
#include <cstdio>

namespace {
constexpr size_t kMaxLogLines = 2000;

std::string tag(int id) { return "[" + std::to_string(id) + "] "; }

// True while a job owns a live rsync child, in either of its two phases.
bool isActive(JobState s) { return s == JobState::Scanning || s == JobState::Running; }
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
            if (isActive(j.state)) j.cancelRequested = true;
        }
        // A stopped process ignores SIGTERM until it runs again, so continue
        // first or a paused job would keep the worker blocked forever.
        signalActiveLocked(SIGCONT);
        signalActiveLocked(SIGTERM);
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
        if (j->state == JobState::Pending || j->state == JobState::Review) {
            j->state = JobState::Cancelled;
        } else if (isActive(j->state)) {
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
            if (j.state == JobState::Pending || j.state == JobState::Review) {
                j.state = JobState::Cancelled;
            }
            if (isActive(j.state)) j.cancelRequested = true;
        }
        signalActiveLocked(SIGCONT);
        signalActiveLocked(SIGTERM);
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
        signalActiveLocked(paused ? SIGSTOP : SIGCONT);
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
        if (j.state == JobState::Pending || isActive(j.state)) return true;
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

void JobQueue::signalActiveLocked(int sig) {
    for (auto& j : jobs_) {
        if (isActive(j.state) && j.pid > 0) kill(-j.pid, sig);
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
        if (isActive(j.state) && j.opts.deleteExtra) deleteRunning = true;
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

// Builds the callback set shared by the dry scan and the live run. Everything it
// touches is addressed by job id, so a concurrent clearFinished() cannot dangle.
RsyncCallbacks JobQueue::makeCallbacks(int id, std::vector<Conflict>* collect) {
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
    cb.onProgress = [this, id](const RsyncProgress& p) {
        std::lock_guard<std::mutex> lk(mutex_);
        Job* j = findByIdLocked(id);
        if (!j) return;
        if (j->state == JobState::Scanning) {
            // A dry run's byte percentage counts transfers that never happen, so
            // the file counter is the only honest measure of scan progress.
            if (p.checkTotal > 0) {
                const int done = p.checkTotal - p.checkRemaining;
                j->percent = static_cast<float>(done) / static_cast<float>(p.checkTotal);
            }
        } else {
            j->percent = p.percent;
            j->transferred = p.transferred;
            j->speed = p.speed;
            j->eta = p.eta;
        }
        ++generation_;
    };
    cb.onLine = [this, id, collect](const std::string& line) {
        if (collect) {
            Conflict c;
            if (parseItemizeLine(line, c)) {
                collect->push_back(std::move(c));
                return;  // itemize output is data, not log noise
            }
            // A dry scan also prints lines for new files, which are not conflicts
            // and would drown the log, so only real messages get through.
            if (line.size() > 12 && (line[0] == '>' || line[0] == 'c' || line[0] == '.')) return;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        appendLogLocked(tag(id) + line);
        if (Job* j = findByIdLocked(id)) j->error = line;
    };
    return cb;
}

// Runs one phase and reports how it ended. Cancel wins over a non-zero exit,
// since rsync exits non-zero precisely because it was killed.
JobQueue::PhaseResult JobQueue::runPhase(const Job& job, RunMode mode, int id,
                                         std::vector<Conflict>* collect) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        appendLogLocked(tag(id) + "$ " + joinArgs(buildRsyncArgs(job, mode)));
    }
    const int code = runRsync(job, mode, makeCallbacks(id, collect));

    std::lock_guard<std::mutex> lk(mutex_);
    PhaseResult r;
    r.exitCode = code;
    if (Job* j = findByIdLocked(id)) {
        j->pid = 0;
        j->exitCode = code;
        r.cancelled = j->cancelRequested;
    }
    r.ok = !r.cancelled && code == 0;
    return r;
}

void JobQueue::finishLocked(int id, JobState state, const std::string& note) {
    if (Job* j = findByIdLocked(id)) {
        j->state = state;
        if (state == JobState::Done) {
            j->percent = 1.0f;
            j->error.clear();
        }
        j->cancelRequested = false;
        if (!j->excludeFrom.empty()) {
            ::remove(j->excludeFrom.c_str());
            j->excludeFrom.clear();
        }
    }
    appendLogLocked(tag(id) + note);
    ++generation_;
}

void JobQueue::worker() {
    for (;;) {
        Job current;
        int id = 0;
        bool needsScan = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || canStartLocked(); });
            if (stop_) return;

            Job* next = firstPendingLocked();
            if (!next) continue;

            // A job the user already answered goes straight to the live run; its
            // skip list is the answer and re-scanning would only ask again.
            needsScan = !next->resolved;
            next->state = needsScan ? JobState::Scanning : JobState::Running;
            next->percent = 0.0f;
            next->cancelRequested = false;
            next->pid = 0;
            current = *next;
            id = next->id;
            ++runningCount_;
            ++generation_;
        }

        std::vector<Conflict> conflicts;
        bool goLive = true;

        if (needsScan) {
            const PhaseResult scan = runPhase(current, RunMode::DryScan, id, &conflicts);
            if (!scan.ok) {
                std::lock_guard<std::mutex> lk(mutex_);
                --runningCount_;
                finishLocked(id, scan.cancelled ? JobState::Cancelled : JobState::Failed,
                             scan.cancelled ? "cancelled during dry scan"
                                            : "dry scan failed, exit " +
                                                  std::to_string(scan.exitCode));
                if (stop_) return;
                cv_.notify_all();
                continue;
            }

            for (auto& c : conflicts) fillConflictDetails(current.source, current.dest, c);

            if (!conflicts.empty()) {
                std::vector<std::string> skip;
                skip.reserve(conflicts.size());
                for (const auto& c : conflicts) skip.push_back(c.path);

                std::lock_guard<std::mutex> lk(mutex_);
                Job* j = findByIdLocked(id);
                appendLogLocked(tag(id) + std::to_string(conflicts.size()) +
                                " conflict(s), policy: " +
                                conflictPolicyName(current.opts.onConflict));
                if (j) j->conflicts = conflicts;

                if (current.opts.onConflict == ConflictPolicy::Pause) {
                    --runningCount_;
                    finishLocked(id, JobState::Review, "waiting for a decision");
                    goLive = false;
                } else {
                    // Both other policies copy what is safe now and differ only in
                    // whether the conflicts come back to the user afterwards.
                    const std::string file = writeSkipFile(skip);
                    if (j) {
                        j->excludeFrom = file;
                        j->state = JobState::Running;
                        j->percent = 0.0f;
                        current = *j;
                    }
                    ++generation_;
                }
            } else {
                std::lock_guard<std::mutex> lk(mutex_);
                if (Job* j = findByIdLocked(id)) {
                    j->state = JobState::Running;
                    j->percent = 0.0f;
                    current = *j;
                }
                ++generation_;
            }
        }

        if (!goLive) {
            cv_.notify_all();
            continue;
        }

        const PhaseResult live = runPhase(current, RunMode::Live, id, nullptr);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            --runningCount_;
            if (live.cancelled) {
                finishLocked(id, JobState::Cancelled, "cancelled");
            } else if (!live.ok) {
                if (Job* j = findByIdLocked(id); j && live.exitCode == 127) {
                    j->error = "rsync not found on PATH";
                }
                finishLocked(id, JobState::Failed,
                             "failed, exit " + std::to_string(live.exitCode));
            } else if (!conflicts.empty() &&
                       current.opts.onConflict == ConflictPolicy::WorkAround) {
                finishLocked(id, JobState::Review,
                             "safe files copied, " + std::to_string(conflicts.size()) +
                                 " conflict(s) waiting");
            } else if (!conflicts.empty() &&
                       current.opts.onConflict == ConflictPolicy::Skip) {
                finishLocked(id, JobState::Done,
                             "done, skipped " + std::to_string(conflicts.size()) +
                                 " conflict(s)");
            } else {
                finishLocked(id, JobState::Done, "done");
            }
            if (stop_) return;
        }
        cv_.notify_all();
    }
}

void JobQueue::resolveJob(int id, const std::vector<Conflict>& decisions) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        Job* j = findByIdLocked(id);
        if (!j || j->state != JobState::Review) return;

        std::vector<std::string> skip;
        for (const auto& c : decisions) {
            if (!c.allow) skip.push_back(c.path);
        }
        if (!j->excludeFrom.empty()) {
            ::remove(j->excludeFrom.c_str());
            j->excludeFrom.clear();
        }
        if (!skip.empty()) j->excludeFrom = writeSkipFile(skip);

        j->conflicts = decisions;
        j->resolved = true;
        j->state = JobState::Pending;
        j->percent = 0.0f;
        appendLogLocked(tag(id) + "resolved: " +
                        std::to_string(decisions.size() - skip.size()) + " applied, " +
                        std::to_string(skip.size()) + " kept as-is");
        ++generation_;
    }
    cv_.notify_all();
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
