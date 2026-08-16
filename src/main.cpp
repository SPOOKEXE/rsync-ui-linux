#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>

#include "session.h"
#include "ui.h"

namespace {

void glfwErrorCallback(int code, const char* desc) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

// GLFW delivers native drag-and-drop here. It fires on the main thread inside
// glfwPollEvents/glfwWaitEvents, so the paths can be stashed without a lock.
void dropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!state) return;
    for (int i = 0; i < count; ++i) state->droppedPaths.push_back(paths[i]);
}

}  // namespace

int main() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to initialise glfw\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1200, 800, "rsync-ui", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini clutter; the layout is fixed anyway
    applyTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    AppState state;
    glfwSetWindowUserPointer(window, &state);
    glfwSetDropCallback(window, dropCallback);

    const std::string sessionFile = sessionPath();
    applySession(state, parseSession(readFileOrEmpty(sessionFile)));

    // Saving is driven by hashing the serialized session rather than by dirty
    // flags scattered through the UI: one place to get right, nothing to forget.
    size_t savedHash = 0;
    double nextSaveCheck = 0.0;

    while (!glfwWindowShouldClose(window)) {
        // Wait for input rather than spinning at vsync: the timeout only exists so
        // progress bars refresh while rsync runs. Idle costs almost no GPU.
        glfwWaitEventsTimeout(state.queue.busy() ? 0.1 : 0.5);

        handleDrops(state);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawUi(state);
        ImGui::Render();

        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (glfwGetTime() >= nextSaveCheck) {
            nextSaveCheck = glfwGetTime() + 2.0;
            saveSessionIfChanged(sessionFile, serializeSession(sessionFromState(state)), savedHash);
        }
    }

    saveSessionIfChanged(sessionFile, serializeSession(sessionFromState(state)), savedHash);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
