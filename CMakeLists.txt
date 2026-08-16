cmake_minimum_required(VERSION 3.16)
project(rsync-ui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

# GLFW: window + input + the native file-drop callback we hang requirement 3 on.
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

# Dear ImGui ships no CMakeLists, so we fetch the sources and build our own target below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(glfw imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_executable(rsync-ui
    src/main.cpp
    src/ui.cpp
    src/rsync_job.cpp
    src/job_queue.cpp
    src/session.cpp
    src/dir_browser.cpp)
target_link_libraries(rsync-ui PRIVATE imgui Threads::Threads)
target_compile_options(rsync-ui PRIVATE -Wall -Wextra)

install(TARGETS rsync-ui RUNTIME DESTINATION bin)
