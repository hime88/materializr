cmake_minimum_required(VERSION 3.20)

# ─── Google Test via FetchContent ────────────────────────────────────────────

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent overriding the parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# ─── Core library (sources under test) ──────────────────────────────────────
# OpenCASCADE and imgui_lib are already available from the parent CMakeLists.txt

add_library(materializr_core STATIC
    ${CMAKE_SOURCE_DIR}/src/core/Document.cpp
    ${CMAKE_SOURCE_DIR}/src/core/History.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SelectionManager.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ExtrudeOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/FilletOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ChamferOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/BooleanOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/DeleteOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/TransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PushPullOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/Sketch.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchEditOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/DuplicateSketchOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchSolver.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SubShapeIndex.cpp
)

target_include_directories(materializr_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
# Mark OpenCASCADE as a SYSTEM include so its own deprecated declarations
# (e.g. std::iterator under C++17) don't spam our build with warnings.
target_include_directories(materializr_core SYSTEM PUBLIC
    ${OpenCASCADE_INCLUDE_DIR}
)

target_link_libraries(materializr_core PUBLIC
    imgui_lib
    glm::glm
    TKernel
    TKMath
    TKG3d
    TKBRep
    TKGeomBase
    TKTopAlgo
    TKPrim
    TKBool
    TKFillet
    TKOffset
    TKBO
)

# ─── Test executables ────────────────────────────────────────────────────────

# test_document
add_executable(test_document test_document.cpp)
target_link_libraries(test_document PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_document COMMAND test_document)

# test_history
add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_history COMMAND test_history)

# test_operations
add_executable(test_operations test_operations.cpp)
target_link_libraries(test_operations PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_operations COMMAND test_operations)

# test_selection
add_executable(test_selection test_selection.cpp)
target_link_libraries(test_selection PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_selection COMMAND test_selection)

# test_reload_edit — boolean/delete rehydrate so edits upstream of them propagate
add_executable(test_reload_edit test_reload_edit.cpp)
target_link_libraries(test_reload_edit PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_reload_edit COMMAND test_reload_edit)
