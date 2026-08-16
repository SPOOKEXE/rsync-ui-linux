#include "ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

const ImVec4 kDim(0.55f, 0.55f, 0.55f, 1.0f);
const ImVec4 kOk(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kBad(1.00f, 0.35f, 0.35f, 1.0f);

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\n\r");
    return s.substr(b, e - b + 1);
}

// Tidies a hand-typed path: trims blanks, expands a leading ~, and drops a
// trailing slash so rsync's "copy the directory itself" behaviour stays predictable.
std::string normalizePath(const std::string& raw) {
    std::string p = trim(raw);
    if (p.empty()) return p;
    if (p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        if (const char* home = std::getenv("HOME")) p = std::string(home) + p.substr(1);
    }
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

void clearBuf(char* buf, size_t cap) { std::memset(buf, 0, cap); }

ImVec4 stateColor(JobState s) {
    switch (s) {
        case JobState::Pending: return kDim;
        case JobState::Running: return ImVec4(1, 1, 1, 1);
        case JobState::Done: return kOk;
        case JobState::Failed: return kBad;
        case JobState::Cancelled: return kDim;
    }
    return kDim;
}

void drawHeader(AppState& s, const std::vector<Job>& jobs) {
    int running = 0, pending = 0, done = 0, failed = 0;
    for (const auto& j : jobs) {
        switch (j.state) {
            case JobState::Running: ++running; break;
            case JobState::Pending: ++pending; break;
            case JobState::Done: ++done; break;
            case JobState::Failed: ++failed; break;
            default: break;
        }
    }

    const bool canStart = !s.sources.empty() && !s.dests.empty();
    ImGui::BeginDisabled(!canStart);
    if (ImGui::Button("Queue all")) {
        s.queue.enqueue(buildJobs(s.sources, s.dests, s.opts));
    }
    ImGui::EndDisabled();
    if (canStart && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%zu source(s) x %zu destination(s) = %zu jobs", s.sources.size(),
                          s.dests.size(), s.sources.size() * s.dests.size());
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel current")) s.queue.cancelCurrent();
    ImGui::SameLine();
    if (ImGui::Button("Cancel all")) s.queue.cancelAll();
    ImGui::SameLine();
    if (ImGui::Button("Clear finished")) s.queue.clearFinished();
    ImGui::SameLine();
    ImGui::Checkbox("Log", &s.showLog);

    ImGui::SameLine();
    ImGui::TextColored(kDim, "  |  %d running   %d pending   %d done   %d failed", running,
                       pending, done, failed);
}

void drawSources(AppState& s) {
    ImGui::TextUnformatted("SOURCES");
    ImGui::BeginChild("sources", ImVec2(0, 170), true);

    int removeAt = -1;
    for (size_t i = 0; i < s.sources.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::SameLine();
        ImGui::Checkbox("rec", &s.sources[i].recursive);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("recurse into subdirectories\noff = copy this folder's own files only");
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(s.sources[i].path.c_str());
        ImGui::PopID();
    }
    if (s.sources.empty()) ImGui::TextColored(kDim, "none yet");

    ImGui::EndChild();

    ImGui::SetNextItemWidth(-160);
    bool submitted = ImGui::InputTextWithHint("##srcinput", "/path/to/source",
                                              s.sourceBuf, sizeof(s.sourceBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add##src", ImVec2(60, 0))) submitted = true;
    ImGui::SameLine();
    if (ImGui::Button("Browse##src", ImVec2(80, 0))) {
        s.browserTarget = BrowserTarget::Source;
        s.browser.open("Pick source", s.sources.empty() ? "" : s.sources.back().path, true);
    }
    if (submitted) {
        std::string p = normalizePath(s.sourceBuf);
        if (!p.empty()) s.sources.push_back({p, true});
        clearBuf(s.sourceBuf, sizeof(s.sourceBuf));
    }

    if (removeAt >= 0) s.sources.erase(s.sources.begin() + removeAt);
}

void drawDests(AppState& s) {
    ImGui::TextUnformatted("DESTINATIONS");
    ImGui::BeginChild("dests", ImVec2(0, 170), true);

    int removeAt = -1;
    for (size_t i = 0; i < s.dests.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 10000);
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::SameLine();
        ImGui::TextUnformatted(s.dests[i].c_str());
        ImGui::PopID();
    }
    if (s.dests.empty()) ImGui::TextColored(kDim, "none yet");

    ImGui::EndChild();

    ImGui::SetNextItemWidth(-160);
    bool submitted = ImGui::InputTextWithHint("##dstinput", "/path/to/destination",
                                              s.destBuf, sizeof(s.destBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add##dst", ImVec2(60, 0))) submitted = true;
    ImGui::SameLine();
    if (ImGui::Button("Browse##dst", ImVec2(80, 0))) {
        s.browserTarget = BrowserTarget::Dest;
        s.browser.open("Pick destination", s.dests.empty() ? "" : s.dests.back(), false);
    }
    if (submitted) {
        std::string p = normalizePath(s.destBuf);
        if (!p.empty()) s.dests.push_back(p);
        clearBuf(s.destBuf, sizeof(s.destBuf));
    }

    if (removeAt >= 0) s.dests.erase(s.dests.begin() + removeAt);
}

void drawOptions(AppState& s) {
    ImGui::TextUnformatted("OPTIONS");
    ImGui::Checkbox("archive -a", &s.opts.archive);
    ImGui::SameLine();
    ImGui::Checkbox("dry run -n", &s.opts.dryRun);
    ImGui::SameLine();

    // --delete removes files in the destination that are absent from the source,
    // so the toggle only takes effect after an explicit confirmation.
    bool wantDelete = s.opts.deleteExtra;
    if (ImGui::Checkbox("--delete", &wantDelete)) {
        if (wantDelete) {
            s.askDeleteConfirm = true;
        } else {
            s.opts.deleteExtra = false;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("deletes files in the destination that are not in the source");
    }
    ImGui::SameLine();
    ImGui::Checkbox("compress -z", &s.opts.compress);
    ImGui::SameLine();
    ImGui::Checkbox("checksum -c", &s.opts.checksum);

    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##extra", "extra rsync args, e.g. --exclude=\"*.tmp\" --bwlimit=10M",
                                 s.extraBuf, sizeof(s.extraBuf))) {
        s.opts.extraArgs = s.extraBuf;
    }
}

void drawDropFolder(AppState& s) {
    ImGui::TextUnformatted("DROP FOLDER");
    if (ImGui::Button("Open folder...", ImVec2(120, 0))) {
        s.browserTarget = BrowserTarget::DropFolder;
        s.browser.open("Open drop folder", s.dropFolder, false);
    }
    ImGui::SameLine();
    if (s.dropFolder.empty()) {
        ImGui::TextColored(kDim, "no folder open");
    } else {
        ImGui::TextUnformatted(s.dropFolder.c_str());
    }

    ImGui::BeginDisabled(s.dropFolder.empty());
    ImGui::Checkbox("drops copy straight here", &s.dropStraightToFolder);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("dropped items recursive", &s.dropRecursive);
    ImGui::SameLine();
    if (s.dropStraightToFolder && !s.dropFolder.empty()) {
        ImGui::TextColored(kDim, "drag files/folders onto this window to queue them");
    } else {
        ImGui::TextColored(kDim, "drag files/folders onto this window to add them as sources");
    }
}

void drawQueue(const std::vector<Job>& jobs, float height) {
    ImGui::TextUnformatted("QUEUE");
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("queue", 7, flags, ImVec2(0, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("progress", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("destination", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("speed", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("eta", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();

        for (const auto& j : jobs) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", j.id);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(stateColor(j.state), "%s", jobStateName(j.state));

            ImGui::TableSetColumnIndex(2);
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%d%%", static_cast<int>(j.percent * 100));
            ImGui::ProgressBar(j.percent, ImVec2(-1, 0), overlay);

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(j.source.c_str());
            if (ImGui::IsItemHovered()) {
                Job copy = j;
                std::string cmd = joinArgs(buildRsyncArgs(copy));
                if (j.state == JobState::Failed && !j.error.empty()) {
                    cmd += "\n\nexit " + std::to_string(j.exitCode) + ": " + j.error;
                }
                ImGui::SetTooltip("%s", cmd.c_str());
            }

            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(j.dest.c_str());

            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(j.speed.c_str());

            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(j.eta.c_str());
        }
        ImGui::EndTable();
    }
}

void drawLog(AppState& s) {
    if (!s.showLog) return;
    ImGui::TextUnformatted("LOG");
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) s.queue.clearLog();

    ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : s.queue.log()) ImGui::TextUnformatted(line.c_str());
    // Keep pinned to the newest line unless the user has scrolled up to read.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void drawDeleteConfirm(AppState& s) {
    if (s.askDeleteConfirm) {
        ImGui::OpenPopup("Enable --delete?");
        s.askDeleteConfirm = false;
    }
    // Passing a p_open flag gives the popup a close button and makes Escape work.
    // Dismissing it any way other than "Enable it" leaves --delete off.
    bool stayOpen = true;
    if (ImGui::BeginPopupModal("Enable --delete?", &stayOpen,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(kBad, "--delete removes files from the destination.");
        ImGui::TextUnformatted(
            "Anything in a destination that is not in its source will be deleted,\n"
            "for every job in the queue. Run once with dry run first if unsure.");
        ImGui::Separator();
        if (ImGui::Button("Enable it", ImVec2(120, 0))) {
            s.opts.deleteExtra = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it off", ImVec2(120, 0))) {
            s.opts.deleteExtra = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!stayOpen) s.opts.deleteExtra = false;
}

}  // namespace

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.ChildRounding = 0.0f;
    st.FrameRounding = 0.0f;
    st.PopupRounding = 0.0f;
    st.GrabRounding = 0.0f;
    st.ScrollbarRounding = 0.0f;
    st.TabRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.WindowPadding = ImVec2(10, 8);
    st.FramePadding = ImVec2(6, 3);
    st.ItemSpacing = ImVec2(6, 5);
    st.CellPadding = ImVec2(6, 3);

    ImVec4* c = st.Colors;
    const ImVec4 black(0, 0, 0, 1);
    const ImVec4 line(0.18f, 0.18f, 0.18f, 1);
    c[ImGuiCol_WindowBg] = black;
    c[ImGuiCol_ChildBg] = black;
    c[ImGuiCol_PopupBg] = black;
    c[ImGuiCol_MenuBarBg] = black;
    c[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(0.07f, 0.07f, 0.07f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.15f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1);
    c[ImGuiCol_Button] = ImVec4(0.10f, 0.10f, 0.10f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.30f, 0.30f, 1);
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.14f, 0.14f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.28f, 0.28f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.60f, 0.60f, 0.60f, 1);
    c[ImGuiCol_ScrollbarBg] = black;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.22f, 0.22f, 1);
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.06f, 0.06f, 0.06f, 1);
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.12f, 0.12f, 0.12f, 1);
    c[ImGuiCol_TableRowBg] = black;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.03f, 0.03f, 0.03f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.85f, 0.85f, 0.85f, 1);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.65f);
}

void handleDrops(AppState& s) {
    if (s.droppedPaths.empty()) return;

    if (s.dropStraightToFolder && !s.dropFolder.empty()) {
        std::vector<SourceEntry> entries;
        entries.reserve(s.droppedPaths.size());
        for (const auto& p : s.droppedPaths) entries.push_back({p, s.dropRecursive});
        s.queue.enqueue(buildJobs(entries, {s.dropFolder}, s.opts));
    } else {
        for (const auto& p : s.droppedPaths) {
            auto already = std::find_if(s.sources.begin(), s.sources.end(),
                                        [&](const SourceEntry& e) { return e.path == p; });
            if (already == s.sources.end()) s.sources.push_back({p, true});
        }
    }
    s.droppedPaths.clear();
}

void drawUi(AppState& s) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("rsync-ui", nullptr, flags);

    const std::vector<Job> jobs = s.queue.jobs();

    drawHeader(s, jobs);
    ImGui::Separator();

    // Sources and destinations sit side by side; each owns half the width.
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginGroup();
    ImGui::PushItemWidth(half);
    ImGui::BeginChild("srccol", ImVec2(half, 235));
    drawSources(s);
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::BeginChild("dstcol", ImVec2(half, 235));
    drawDests(s);
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::Separator();
    drawOptions(s);
    ImGui::Separator();
    drawDropFolder(s);
    ImGui::Separator();

    // Split the remaining height between the queue table and the log pane.
    const float logHeight = s.showLog ? 170.0f : 0.0f;
    float queueHeight = ImGui::GetContentRegionAvail().y - logHeight;
    if (queueHeight < 120.0f) queueHeight = 120.0f;
    drawQueue(jobs, queueHeight - ImGui::GetTextLineHeightWithSpacing());
    drawLog(s);

    std::string picked;
    if (s.browser.draw(picked)) {
        switch (s.browserTarget) {
            case BrowserTarget::Source: s.sources.push_back({picked, true}); break;
            case BrowserTarget::Dest: s.dests.push_back(picked); break;
            case BrowserTarget::DropFolder: s.dropFolder = picked; break;
            case BrowserTarget::None: break;
        }
        s.browserTarget = BrowserTarget::None;
    }
    drawDeleteConfirm(s);

    ImGui::End();
}
