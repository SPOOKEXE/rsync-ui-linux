#pragma once

#include <string>
#include <vector>

#include "dir_browser.h"
#include "job_queue.h"
#include "session.h"

// Which list the folder picker is currently filling in.
enum class BrowserTarget { None, Source, Dest, DropFolder };

struct AppState {
    std::vector<SourceEntry> sources;
    std::vector<std::string> dests;
    JobOptions opts;

    // Requirement 3: with a drop folder set and this enabled, anything dropped on
    // the window is queued straight into that folder instead of joining the source list.
    std::string dropFolder;
    bool dropStraightToFolder = false;
    bool dropRecursive = true;

    JobQueue queue;

    // Paths handed over by the GLFW drop callback, drained once per frame.
    std::vector<std::string> droppedPaths;

    // UI-only scratch state.
    char sourceBuf[1024] = {};
    char destBuf[1024] = {};
    char extraBuf[512] = {};
    DirBrowser browser;
    BrowserTarget browserTarget = BrowserTarget::None;
    bool askDeleteConfirm = false;
    bool showLog = true;
    int restoredJobs = 0;  // banner count after a session restore, cleared by the user

    // Conflict resolution modal. The edited copy lives here rather than in the
    // queue so the worker is never looking at half-made decisions.
    int conflictJobId = 0;
    bool openConflictModal = false;
    std::vector<Conflict> conflictEdit;
    std::string conflictSummary;
};

void applyTheme();

// Session round-trip. Only unfinished jobs are carried over; done, failed and
// cancelled rows are transient and disappear with the process.
SessionData sessionFromState(AppState& s);
void applySession(AppState& s, const SessionData& d);

// Converts this frame's dropped paths into queued jobs or source rows.
void handleDrops(AppState& s);

// Draws the whole interface into one full-viewport window.
void drawUi(AppState& s);
