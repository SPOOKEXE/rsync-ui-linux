#include "dir_browser.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

std::string homeDir() {
    if (const char* h = std::getenv("HOME")) return h;
    return "/";
}

void setBuf(char* buf, size_t cap, const std::string& s) {
    std::snprintf(buf, cap, "%s", s.c_str());
}

}  // namespace

void DirBrowser::open(const char* title, const std::string& startPath, bool allowFiles) {
    title_ = title;
    allowFiles_ = allowFiles;
    selected_.clear();
    error_.clear();

    std::error_code ec;
    fs::path start = startPath.empty() ? fs::path(homeDir()) : fs::path(startPath);
    if (!fs::is_directory(start, ec)) start = start.parent_path();
    if (start.empty() || !fs::is_directory(start, ec)) start = fs::path(homeDir());

    cwd_ = start.string();
    setBuf(pathBuf_, sizeof(pathBuf_), cwd_);
    refresh();
    open_ = true;
    needsPopup_ = true;
}

void DirBrowser::refresh() {
    dirs_.clear();
    files_.clear();
    error_.clear();

    std::error_code ec;
    fs::directory_iterator it(cwd_, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error_ = "cannot read " + cwd_ + ": " + ec.message();
        return;
    }
    for (const auto& entry : it) {
        std::error_code dirEc;
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;  // hide dotfiles, keeps the list readable
        if (entry.is_directory(dirEc)) {
            dirs_.push_back(name);
        } else if (allowFiles_) {
            files_.push_back(name);
        }
    }
    std::sort(dirs_.begin(), dirs_.end());
    std::sort(files_.begin(), files_.end());
}

bool DirBrowser::draw(std::string& out) {
    if (!open_) return false;

    if (needsPopup_) {
        ImGui::OpenPopup(title_.c_str());
        needsPopup_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_Appearing);
    bool picked = false;
    bool stayOpen = true;
    if (ImGui::BeginPopupModal(title_.c_str(), &stayOpen, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetNextItemWidth(-90);
        if (ImGui::InputText("##path", pathBuf_, sizeof(pathBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::error_code ec;
            if (fs::is_directory(pathBuf_, ec)) {
                cwd_ = pathBuf_;
                refresh();
            } else {
                error_ = "not a directory";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Go", ImVec2(80, 0))) {
            std::error_code ec;
            if (fs::is_directory(pathBuf_, ec)) {
                cwd_ = pathBuf_;
                refresh();
            } else {
                error_ = "not a directory";
            }
        }

        ImGui::BeginChild("list", ImVec2(0, -70), true);
        if (fs::path(cwd_).has_parent_path() && fs::path(cwd_).parent_path() != cwd_) {
            if (ImGui::Selectable("..", false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    cwd_ = fs::path(cwd_).parent_path().string();
                    setBuf(pathBuf_, sizeof(pathBuf_), cwd_);
                    selected_.clear();
                    refresh();
                }
            }
        }
        for (const auto& d : dirs_) {
            const std::string label = "[dir] " + d;
            bool isSel = (selected_ == d);
            if (ImGui::Selectable(label.c_str(), isSel, ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_ = d;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    cwd_ = (fs::path(cwd_) / d).string();
                    setBuf(pathBuf_, sizeof(pathBuf_), cwd_);
                    selected_.clear();
                    refresh();
                    break;
                }
            }
        }
        for (const auto& f : files_) {
            bool isSel = (selected_ == f);
            if (ImGui::Selectable(f.c_str(), isSel)) selected_ = f;
        }
        ImGui::EndChild();

        if (!error_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", error_.c_str());
        } else if (!selected_.empty()) {
            ImGui::TextDisabled("selected: %s", selected_.c_str());
        } else {
            ImGui::TextDisabled("double-click a folder to enter it");
        }

        const std::string chosen =
            selected_.empty() ? cwd_ : (fs::path(cwd_) / selected_).string();
        const std::string useLabel =
            selected_.empty() ? "Use this folder" : "Use selection";
        if (ImGui::Button(useLabel.c_str(), ImVec2(150, 0))) {
            out = chosen;
            picked = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
            open_ = false;
        }
        ImGui::EndPopup();
    }

    if (picked || !stayOpen) open_ = false;
    return picked;
}
