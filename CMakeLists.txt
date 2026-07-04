cmake_minimum_required(VERSION 3.20)
project(Materializr VERSION 1.3.0 LANGUAGES CXX)

# Pre-release suffix appended to the reported version (e.g. "-beta.1"). Empty on
# stable releases. Kept here next to project() so a beta build self-identifies
# as e.g. "1.3.0-beta.1" — that way the in-app update check (which is semver
# pre-release aware) doesn't nag a beta tester to "update" to the build they're
# already running, and hands off cleanly to the final 1.3.0.
set(MATERIALIZR_VERSION_SUFFIX "")

# macOS: deliberately do NOT pin CMAKE_OSX_DEPLOYMENT_TARGET. The bundled Homebrew
# OpenCASCADE/SDL2 dylibs are built for the build host's macOS, so they — not our
# compile flag — set the real floor of a packaged .dmg. Letting the binary inherit
# the host target keeps it consistent with those dylibs (no "built for newer
# version" linker warnings), and packaging/macos/build-dmg.sh derives the true
# LSMinimumSystemVersion from the actual binaries so the plist can't lie. (Override
# with -DCMAKE_OSX_DEPLOYMENT_TARGET=… only alongside dylibs built for that target.)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── FetchContent dependencies ───────────────────────────────────────────────

include(FetchContent)

# SDL2 — the windowing/input backend on every platform (the Android port
# unified desktop + touch on SDL2; src/app/Window.cpp is SDL-based, replacing
# the old GLFW backend). Prefer the system/vcpkg package; fall back to building
# 2.30.9 from source — the same version android/scripts/setup-deps.sh pins —
# which on Linux needs the X11 dev headers already required for the GL stack.
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "System SDL2 not found - fetching and building 2.30.9 from source")
    FetchContent_Declare(
        sdl2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.30.9
    )
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl2)
endif()
# Normalise the target name across find_package configs and source builds.
if(TARGET SDL2::SDL2)
    set(MZ_SDL2_TARGET SDL2::SDL2)
elseif(TARGET SDL2)
    set(MZ_SDL2_TARGET SDL2)
else()
    message(FATAL_ERROR "SDL2 target not found after find_package/FetchContent")
endif()

# GLM
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# ImGui (docking branch)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

# ─── ImGui static library ────────────────────────────────────────────────────

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC ${MZ_SDL2_TARGET})

# ─── Find system packages ────────────────────────────────────────────────────

find_package(OpenGL REQUIRED)

# OpenCASCADE
if(WIN32 OR APPLE)
    # Windows (vcpkg) and macOS (Homebrew: `brew install opencascade`) both ship a
    # clean OpenCASCADE CMake config that exports the TK* targets and sets
    # OpenCASCADE_INCLUDE_DIR directly — no path patching needed (unlike the Debian
    # packages handled in the else branch). The Homebrew config lives under the brew
    # prefix; pass -DCMAKE_PREFIX_PATH="$(brew --prefix)" if CMake doesn't find it.
    find_package(OpenCASCADE CONFIG REQUIRED)
else()
# Linux: work around missing libtbb-dev symlinks by patching cmake files locally.
# Auto-detect the OCCT cmake directory (differs between aarch64/x86_64).
file(GLOB_RECURSE _occt_config_search "/usr/lib/*/cmake/opencascade/OpenCASCADEConfig.cmake")
if(_occt_config_search)
    list(GET _occt_config_search 0 _occt_config_file)
    get_filename_component(OCCT_CMAKE_SRC "${_occt_config_file}" DIRECTORY)
else()
    # Fallback: try standard multiarch path
    execute_process(COMMAND dpkg-architecture -qDEB_HOST_MULTIARCH
                    OUTPUT_VARIABLE _multiarch OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(OCCT_CMAKE_SRC "/usr/lib/${_multiarch}/cmake/opencascade")
endif()
message(STATUS "OCCT cmake source: ${OCCT_CMAKE_SRC}")

# Detect the lib directory for patching .so paths
get_filename_component(_occt_lib_dir "${OCCT_CMAKE_SRC}/../.." ABSOLUTE)
set(OCCT_CMAKE_LOCAL "${CMAKE_BINARY_DIR}/cmake_occt_patched")
file(MAKE_DIRECTORY ${OCCT_CMAKE_LOCAL})

file(GLOB OCCT_CMAKE_FILES "${OCCT_CMAKE_SRC}/*.cmake")
foreach(_file ${OCCT_CMAKE_FILES})
    get_filename_component(_fname ${_file} NAME)
    file(READ ${_file} _content)
    # Replace broken .so symlinks with versioned .so files that actually exist
    # Handle both /path/libtbb.so" and /path/libtbb.so; patterns
    string(REGEX REPLACE "/usr/lib/[^/]+/libtbb\\.so([;\"])" "${_occt_lib_dir}/libtbb.so.12\\1" _content "${_content}")
    string(REGEX REPLACE "/usr/lib/[^/]+/libtbbmalloc\\.so([;\"])" "${_occt_lib_dir}/libtbbmalloc.so.2\\1" _content "${_content}")
    string(REGEX REPLACE "/usr/lib/[^/]+/libfreetype\\.so([;\"])" "${_occt_lib_dir}/libfreetype.so.6\\1" _content "${_content}")
    # Fix install prefix in Config: replace the two get_filename_component calls
    string(REGEX REPLACE
        "get_filename_component \\(OpenCASCADE_INSTALL_PREFIX \"\\$\\{CMAKE_CURRENT_LIST_FILE\\}\" PATH\\)[^\n]*\nget_filename_component \\(OpenCASCADE_INSTALL_PREFIX \"\\$\\{OpenCASCADE_INSTALL_PREFIX\\}\" PATH\\)"
        "set(OpenCASCADE_INSTALL_PREFIX \"/usr\")"
        _content "${_content}")
    # Fix _IMPORT_PREFIX in Targets files: replace the prefix computation block
    # that goes up 4 levels from the cmake file location with a hardcoded /usr
    string(REGEX REPLACE
        "get_filename_component\\(_IMPORT_PREFIX \"\\$\\{CMAKE_CURRENT_LIST_FILE\\}\" PATH\\)"
        "set(_IMPORT_PREFIX \"/usr\")\nif(FALSE)  # patched out"
        _content "${_content}")
    string(REGEX REPLACE
        "if\\(_IMPORT_PREFIX STREQUAL \"/\"\\)\n  set\\(_IMPORT_PREFIX \"\"\\)\nendif\\(\\)"
        "endif()  # end patched-out block"
        _content "${_content}")
    file(WRITE "${OCCT_CMAKE_LOCAL}/${_fname}" "${_content}")
endforeach()

find_package(OpenCASCADE REQUIRED
    PATHS ${OCCT_CMAKE_LOCAL}
    NO_DEFAULT_PATH
)
endif()

# ─── Main executable ─────────────────────────────────────────────────────────

add_executable(materializr
    $<$<PLATFORM_ID:Windows>:packaging/windows/app.rc>
    src/main.cpp
    src/app/Application.cpp
    src/app/InteractiveOpController.cpp
    src/app/FaceOpControllers.cpp
    src/app/Application_Dialogs.cpp
    src/app/Application_InteractiveOps.cpp
    src/app/layout/LayoutCommon.cpp
    src/app/layout/classic/ClassicLayout.cpp
    src/app/layout/modern/ModernLayout.cpp
    src/app/layout/imtouch/ImTouchLayout.cpp
    src/app/Application_Viewport.cpp
    src/app/Window.cpp
    src/plugin/PluginContext.cpp
    src/plugin/PluginRegistry.cpp
    src/plugins/ForceLink.cpp
    src/plugins/CoreCommandsPlugin.cpp
    src/plugins/StepIOPlugin.cpp
    src/plugins/IgesIOPlugin.cpp
    src/plugins/StlImportPlugin.cpp
    src/plugins/StlExportPlugin.cpp
    src/plugins/GltfExportPlugin.cpp
    src/plugins/BooleanPlugin.cpp
    src/plugins/DeletePlugin.cpp
    src/plugins/TransformPlugin.cpp
    src/plugins/MirrorPlugin.cpp
    src/plugins/CopyPlugin.cpp
    src/plugins/PatternPlugin.cpp
    src/plugins/ShellPlugin.cpp
    src/plugins/SplitBodyPlugin.cpp
    src/plugins/ConstructionPlanePlugin.cpp
    src/plugins/ConstructionAxisPlugin.cpp
    src/plugins/RevolvePlugin.cpp
    src/plugins/FilletPlugin.cpp
    src/plugins/ChamferPlugin.cpp
    src/plugins/ExtrudePlugin.cpp
    src/plugins/PushPullPlugin.cpp
    src/plugins/SketchPlugin.cpp
    src/plugins/GizmoDragPlugin.cpp
    src/plugins/LoftPlugin.cpp
    src/plugins/SvgImportPlugin.cpp
    src/plugins/PrimitivesPlugin.cpp
    src/plugins/TutorialPlugin.cpp
    src/core/Document.cpp
    src/core/History.cpp
    src/core/SelectionManager.cpp
    src/core/VariableManager.cpp
    src/core/Material.cpp
    src/core/Verbose.cpp
    src/core/VersionManager.cpp
    src/modeling/ExtrudeOp.cpp
    src/modeling/PushPullOp.cpp
    src/modeling/MoveFaceOp.cpp
    src/modeling/CombineSketchesOp.cpp
    src/modeling/DuplicateSketchOp.cpp
    src/modeling/MoveHoleOp.cpp
    src/modeling/ReplayOp.cpp
    src/modeling/OperationFactory.cpp
    src/modeling/SubShapeIndex.cpp
    src/modeling/EdgeAnchor.cpp
    src/modeling/FaceAnchor.cpp
    src/modeling/TopoName.cpp
    src/modeling/GenerationLedger.cpp
    src/modeling/ThreadOp.cpp
    src/modeling/FilletOp.cpp
    src/modeling/ChamferOp.cpp
    src/modeling/ResizeCylindricalOp.cpp
    src/modeling/BooleanOp.cpp
    src/modeling/TransformOp.cpp
    src/modeling/ShellOp.cpp
    src/modeling/TaperOp.cpp
    src/modeling/ScaleFaceOp.cpp
    src/modeling/DefeatureOp.cpp
    src/modeling/ProjectSketchOp.cpp
    src/modeling/SplitBodyOp.cpp
    src/modeling/MirrorOp.cpp
    src/modeling/PatternOp.cpp
    src/modeling/RevolveOp.cpp
    src/modeling/SweepOp.cpp
    src/modeling/LoftOp.cpp
    src/modeling/SketchTransformOp.cpp
    src/modeling/ConstructionPlaneOp.cpp
    src/modeling/ConstructionAxisOp.cpp
    src/modeling/PlaneTransformOp.cpp
    src/modeling/AxisTransformOp.cpp
    src/modeling/AlignOp.cpp
    src/modeling/CopyOp.cpp
    src/modeling/DeleteOp.cpp
    src/modeling/PrimitiveOp.cpp
    src/modeling/TextSketchOp.cpp
    src/modeling/SvgImport.cpp
    src/modeling/Sketch.cpp
    src/modeling/Unfold.cpp
    src/modeling/SketchSolver.cpp
    src/modeling/SketchTool.cpp
    src/modeling/SketchEditOp.cpp
    src/viewport/Viewport.cpp
    src/viewport/Camera.cpp
    src/viewport/Grid.cpp
    src/viewport/ShapeRenderer.cpp
    src/viewport/SketchRenderer.cpp
    src/viewport/ViewCube.cpp
    src/viewport/Picker.cpp
    src/viewport/EdgeRenderer.cpp
    src/viewport/PlaneRenderer.cpp
    src/viewport/AxisRenderer.cpp
    src/viewport/BackgroundRenderer.cpp
    src/viewport/Gizmo.cpp
    src/viewport/BoxSelect.cpp
    src/viewport/SectionView.cpp
    src/viewport/SelectionHighlight.cpp
    src/ui/Toolbar.cpp
    src/ui/TouchWidgets.cpp
    src/ui/HistoryPanel.cpp
    src/ui/ItemsPanel.cpp
    src/ui/StatusBar.cpp
    src/ui/ThemeManager.cpp
    src/ui/PropertiesPanel.cpp
    src/ui/MeasureTool.cpp
    src/ui/VariablePanel.cpp
    src/ui/DimensionInput.cpp
    src/ui/MaterialPanel.cpp
    src/ui/DrawingView.cpp
    src/ui/VersionPanel.cpp
    src/ui/SectionPanel.cpp
    src/ui/SettingsPanel.cpp
    src/ui/ToastNotification.cpp
    src/ui/ShortcutsPanel.cpp
    src/ui/AboutDialog.cpp
    src/ui/UpdateChecker.cpp
    src/ui/HelpPanel.cpp
    src/io/ProjectIO.cpp
    src/io/SketchRecovery.cpp
    src/io/ProjectRecovery.cpp
    src/io/Settings.cpp
    src/io/StepIO.cpp
    src/io/StlIO.cpp
    src/io/MeshDecimate.cpp
    src/io/StlExport.cpp
    src/io/SvgExport.cpp
    src/io/IgesIO.cpp
    src/io/ImageExport.cpp
    src/io/GltfExport.cpp
    src/io/FileDialogs.cpp
)

target_include_directories(materializr PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
# Mark OpenCASCADE as a SYSTEM include so its own deprecated declarations
# (e.g. std::iterator under C++17) don't spam our build with warnings.
target_include_directories(materializr SYSTEM PRIVATE
    ${OpenCASCADE_INCLUDE_DIR}
)

target_link_libraries(materializr PRIVATE
    imgui_lib
    ${MZ_SDL2_TARGET}
    OpenGL::GL
    glm::glm
)

if(WIN32)
    # vcpkg ships OpenCASCADE 7.8+, which renamed several data-exchange toolkits
    # (TKSTEP -> TKDESTEP, etc.). Link via the umbrella variable from OCCT's
    # CMake config so we don't have to track its toolkit reshuffles by hand.
    message(STATUS "OpenCASCADE libraries: ${OpenCASCADE_LIBRARIES}")
    target_link_libraries(materializr PRIVATE ${OpenCASCADE_LIBRARIES})
else()
    # Linux (system OCCT) and macOS (Homebrew) link the explicit set of toolkits
    # the app actually uses. This is deliberately NOT OCCT's umbrella
    # ${OpenCASCADE_LIBRARIES}: that pulls in the Draw/test harnesses (TKDraw,
    # TKViewerTest, TKQADraw, ...), which the app never calls and which bloat the
    # bundled macOS .dmg with ~dozens of extra dylibs. OCCT 7.8 renamed the
    # data-exchange toolkits (TKSTEP -> TKDESTEP, etc.); pick names by version so
    # we link on both pre- and post-refactor OCCT (distros + Homebrew ship 7.9+).
    if(OpenCASCADE_VERSION VERSION_GREATER_EQUAL 7.8)
        set(OCCT_DATAEXCHANGE_LIBS TKDESTEP TKDEIGES TKDESTL)
    else()
        set(OCCT_DATAEXCHANGE_LIBS TKSTEP TKIGES TKSTL)
    endif()
    target_link_libraries(materializr PRIVATE
        TKernel
        TKMath
        TKG3d
        TKBRep
        TKGeomBase
        TKGeomAlgo
        TKTopAlgo
        TKPrim
        TKBool
        TKBO
        TKFillet
        TKOffset
        TKShHealing
        TKMesh
        TKXSBase
        ${OCCT_DATAEXCHANGE_LIBS}
        # TKBin: BinTools::Write / Read (binary BREP, used by the v3
        # .materializr format — smaller + faster than BRepTools' ASCII).
        TKBin
        # TKService + TKV3d: Font_BRepFont / Font_BRepTextBuilder (the
        # sketch Text tool renders TrueType glyphs into outline wires).
        TKService
        TKV3d
    )
endif()

# zlib for the v3 .materializr file format (gzipped output, transparent on
# read). System zlib on Linux; vcpkg ships one on Windows.
find_package(ZLIB REQUIRED)
target_link_libraries(materializr PRIVATE ZLIB::ZLIB)

# ─── Windows specifics ───────────────────────────────────────────────────────
# Everything here is gated on Windows/MSVC, so the Linux build is unaffected.
if(WIN32)
    # GL loader (opengl32.dll only provides GL 1.1). GLEW comes from vcpkg.
    find_package(GLEW REQUIRED)
    target_link_libraries(materializr PRIVATE GLEW::GLEW)
endif()

if(MSVC)
    # _USE_MATH_DEFINES: M_PI etc.  NOMINMAX: keep std::min/max usable.
    # /bigobj: OpenCASCADE-heavy translation units exceed the default obj limit.
    target_compile_definitions(materializr PRIVATE
        _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(materializr PRIVATE /bigobj)
endif()

if(APPLE)
    # OpenGL was deprecated on macOS 10.14; silence the framework headers app-wide.
    # gl_common.h also defines this, but ImGui's GL3 backend pulls <OpenGL/gl3.h>
    # in on its own, so define it on both targets. Apple still ships/supports the
    # framework, and there's no Metal/MoltenVK port yet.
    target_compile_definitions(materializr PRIVATE GL_SILENCE_DEPRECATION)
    target_compile_definitions(imgui_lib PRIVATE GL_SILENCE_DEPRECATION)
endif()

# Surface the project version to the C++ code so About / update-checker can use
# a single source of truth (the project() VERSION at the top of this file).
target_compile_definitions(materializr PRIVATE
    MATERIALIZR_VERSION="${PROJECT_VERSION}${MATERIALIZR_VERSION_SUFFIX}")

# OCC_CONVERT_SIGNALS turns the OCC_CATCH_SIGNALS macro into a real
# Standard_ErrorHandler so that an OCCT kernel fault (SIGSEGV/SIGFPE deep inside
# the modelling algorithms) longjmps to the nearest op-level try{} instead of
# aborting the whole app. OSD::SetSignal() (in main.cpp) installs the handler;
# this macro is what lets a `catch` actually receive the converted exception.
# OCCT's own cmake only exports this for the RelWithDebInfo config, so our
# Release AppImage was shipping with it OFF — every OCC_CATCH_SIGNALS (ShellOp,
# MoveFaceOp, …) silently expanded to nothing and kernel faults killed the app
# (issue #3: Shell-on-Shell crash). Define it unconditionally on Unix/Linux.
# Not for Windows/MSVC, which throws from the signal handler natively (/EHa).
if(NOT WIN32)
    target_compile_definitions(materializr PRIVATE OCC_CONVERT_SIGNALS)
endif()

# libcurl powers the Help → Check for Updates feature (a small HTTPS GET to
# GitHub's releases API). Required on both platforms.
find_package(CURL REQUIRED)
target_link_libraries(materializr PRIVATE CURL::libcurl)

# ─── Tests ───────────────────────────────────────────────────────────────────

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
