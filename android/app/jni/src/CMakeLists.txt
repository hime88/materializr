cmake_minimum_required(VERSION 3.22)
project(materializr_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (this file lives at android/app/jni/src/).
get_filename_component(MZ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(MZ_SRC "${MZ_ROOT}/src")

# Cross-built OpenCASCADE prefix (FreeType is statically baked into TKService).
if(NOT DEFINED MATERIALIZR_OCCT_PREFIX)
    message(FATAL_ERROR "MATERIALIZR_OCCT_PREFIX not set (path to cross-built OCCT for arm64-v8a)")
endif()
set(OCCT_INC "${MATERIALIZR_OCCT_PREFIX}/include/opencascade")
set(OCCT_LIB "${MATERIALIZR_OCCT_PREFIX}/lib")

# ── ImGui (docking) + GLM, same versions as the desktop build ────────────────
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG docking)
FetchContent_MakeAvailable(imgui)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_MakeAvailable(glm)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# imgui_impl_opengl3 auto-selects the GLES3 loader when it sees __ANDROID__.
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(imgui_lib PUBLIC SDL2)

# ── App sources ──────────────────────────────────────────────────────────────
# Glob the whole tree, then drop translation units that are desktop-only.
file(GLOB_RECURSE MZ_SOURCES CONFIGURE_DEPENDS "${MZ_SRC}/*.cpp")
list(FILTER MZ_SOURCES EXCLUDE REGEX "/main\\.cpp$")            # desktop CLI entry
list(FILTER MZ_SOURCES EXCLUDE REGEX "/UpdateChecker\\.cpp$")   # libcurl (desktop-only)
# FileDialogs.cpp IS built on Android: it compiles the in-app ImGui browser
# (pfd is #ifdef'd out). android_main.cpp (SDL_main) + android_shims.cpp
# (update-check stub) replace the remaining desktop-only entry points.

add_library(main SHARED ${MZ_SOURCES})

target_include_directories(main PRIVATE "${MZ_SRC}")
target_include_directories(main SYSTEM PRIVATE "${OCCT_INC}")

# SDL2 (built by the sibling add_subdirectory(SDL)) provides the headers/target.
target_link_libraries(main PRIVATE SDL2 SDL2main imgui_lib glm::glm)

# OpenCASCADE. The desktop build lists ~21 toolkits and relies on the system
# OCCT's DT_NEEDED to pull the rest transitively (TKG2d, etc.). Under Android's
# --no-undefined link, every transitively-needed toolkit must be on the line, so
# we link the whole cross-built set. --no-as-needed (in LINK_FLAGS, before the
# libs) keeps them all regardless of the cyclic dependency order.
file(GLOB OCCT_SO_PATHS "${OCCT_LIB}/libTK*.so")
list(LENGTH OCCT_SO_PATHS _occt_count)
message(STATUS "Linking ${_occt_count} OpenCASCADE toolkits from ${OCCT_LIB}")
target_link_options(main PRIVATE "-Wl,--no-as-needed")
target_link_libraries(main PRIVATE ${OCCT_SO_PATHS})

# Android system libs: GL ES 3, EGL, zlib (NDK-provided), Android logging.
find_library(EGL_LIB EGL)
find_library(GLES3_LIB GLESv3)
find_library(LOG_LIB log)
find_library(ANDROID_LIB android)
find_library(Z_LIB z)
target_link_libraries(main PRIVATE ${GLES3_LIB} ${EGL_LIB} ${LOG_LIB} ${ANDROID_LIB} ${Z_LIB})

# Android native build is SEPARATE from the root CMakeLists, so this version
# must be bumped by hand to match root project(VERSION ...) / build.gradle
# versionName on every release. (It silently drifted to 0.9.7.1 through 1.0.x.)
target_compile_definitions(main PRIVATE MATERIALIZR_VERSION="1.1.0")
