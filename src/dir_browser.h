#pragma once

#include <string>
#include <vector>

// Minimal ImGui folder/file picker. There is no portable native dialog behind
// GLFW, and shelling out to zenity adds a runtime dependency, so this walks
// std::filesystem directly.
class DirBrowser {
public:
    // Opens the modal on the next draw(). startPath may be empty (falls back to $HOME).
    void open(const char* title, const std::string& startPath, bool allowFiles);

    // Draws the modal when open. Returns true on the single frame a path is picked,
    // filling `out` with it.
    bool draw(std::string& out);

    bool isOpen() const { return open_; }

private:
    void refresh();

    bool open_ = false;
    bool needsPopup_ = false;
    bool allowFiles_ = false;
    std::string title_;
    std::string cwd_;
    std::string selected_;
    std::string error_;
    std::vector<std::string> dirs_;
    std::vector<std::string> files_;
    char pathBuf_[1024] = {};
};
