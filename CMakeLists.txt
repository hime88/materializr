cmake_minimum_required(VERSION 3.20)
project(Materializr VERSION 0.3.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── FetchContent dependencies ───────────────────────────────────────────────

include(FetchContent)

# GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

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
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw)

# ─── Find system packages ────────────────────────────────────────────────────

find_package(OpenGL REQUIRED)

# OpenCASCADE
if(WIN32)
    # Windows: OpenCASCADE is provided by vcpkg (see vcpkg.json). Its CMake config
    # exports the same TK* targets and sets OpenCASCADE_INCLUDE_DIR, so the rest
    # of this file is identical to the Linux path.
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
    src/main.cpp
    src/app/Application.cpp
    src/app/Application_Dialogs.cpp
    src/app/Application_InteractiveOps.cpp
    src/app/Application_Viewport.cpp
    src/app/Window.cpp
    src/plugin/PluginContext.cpp
    src/plugin/PluginRegistry.cpp
    src/plugins/ForceLink.cpp
    src/plugins/CoreCommandsPlugin.cpp
    src/plugins/StepIOPlugin.cpp
    src/plugins/IgesIOPlugin.cpp
    src/plugins/StlExportPlugin.cpp
    src/plugins/GltfExportPlugin.cpp
    src/plugins/ImageExportPlugin.cpp
    src/plugins/BooleanPlugin.cpp
    src/plugins/DeletePlugin.cpp
    src/plugins/TransformPlugin.cpp
    src/plugins/MirrorPlugin.cpp
    src/plugins/CopyPlugin.cpp
    src/plugins/PatternPlugin.cpp
    src/plugins/ShellPlugin.cpp
    src/plugins/SplitBodyPlugin.cpp
    src/plugins/ConstructionPlanePlugin.cpp
    src/plugins/FilletPlugin.cpp
    src/plugins/ChamferPlugin.cpp
    src/plugins/ExtrudePlugin.cpp
    src/plugins/PushPullPlugin.cpp
    src/plugins/SketchPlugin.cpp
    src/plugins/GizmoDragPlugin.cpp
    src/core/Document.cpp
    src/core/History.cpp
    src/core/SelectionManager.cpp
    src/core/VariableManager.cpp
    src/core/Material.cpp
    src/core/Verbose.cpp
    src/core/VersionManager.cpp
    src/modeling/ExtrudeOp.cpp
    src/modeling/PushPullOp.cpp
    src/modeling/ReplayOp.cpp
    src/modeling/FilletOp.cpp
    src/modeling/ChamferOp.cpp
    src/modeling/ResizeCylindricalOp.cpp
    src/modeling/BooleanOp.cpp
    src/modeling/TransformOp.cpp
    src/modeling/ShellOp.cpp
    src/modeling/SplitBodyOp.cpp
    src/modeling/MirrorOp.cpp
    src/modeling/PatternOp.cpp
    src/modeling/RevolveOp.cpp
    src/modeling/SweepOp.cpp
    src/modeling/LoftOp.cpp
    src/modeling/ConstructionPlaneOp.cpp
    src/modeling/AlignOp.cpp
    src/modeling/CopyOp.cpp
    src/modeling/DeleteOp.cpp
    src/modeling/TextSketchOp.cpp
    src/modeling/Sketch.cpp
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
    src/viewport/BackgroundRenderer.cpp
    src/viewport/Gizmo.cpp
    src/viewport/BoxSelect.cpp
    src/viewport/SectionView.cpp
    src/viewport/SelectionHighlight.cpp
    src/ui/Toolbar.cpp
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
    src/io/Settings.cpp
    src/io/StepIO.cpp
    src/io/StlExport.cpp
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
    glfw
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
        TKSTEP
        TKXSBase
        TKIGES
        TKSTL
    )
endif()

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

# Surface the project version to the C++ code so About / update-checker can use
# a single source of truth (the project() VERSION at the top of this file).
target_compile_definitions(materializr PRIVATE
    MATERIALIZR_VERSION="${PROJECT_VERSION}")

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
