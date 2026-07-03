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

find_package(ZLIB REQUIRED)

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
    ${CMAKE_SOURCE_DIR}/src/modeling/MoveHoleOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchSolver.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ShellOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/MoveFaceOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SubShapeIndex.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/EdgeAnchor.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ProjectIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/StlIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/MeshDecimate.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ProjectRecovery.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Verbose.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/Unfold.cpp
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
    TKBin
    TKDESTL      # RWStl (STL read) for StlIO
    TKShHealing  # ShapeUpgrade_UnifySameDomain for StlIO
    TKMesh       # BRepMesh_IncrementalMesh for Unfold (curved faces)
    ZLIB::ZLIB
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

# test_recovery — multi-instance recovery slots (flock claim/probe/orphan scan)
add_executable(test_recovery test_recovery.cpp)
target_link_libraries(test_recovery PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_recovery COMMAND test_recovery)

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

# test_move_hole — moving a through-hole (round/square) re-cuts cleanly
add_executable(test_move_hole test_move_hole.cpp)
target_link_libraries(test_move_hole PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_move_hole COMMAND test_move_hole)

# test_pushpull_normal — outward-normal correction on holed/annular faces
add_executable(test_pushpull_normal test_pushpull_normal.cpp)
target_link_libraries(test_pushpull_normal PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_pushpull_normal COMMAND test_pushpull_normal)

# test_stl_import — STL triangle soup sews into a clean 6-face solid (parked feature)
add_executable(test_stl_import test_stl_import.cpp)
target_link_libraries(test_stl_import PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_stl_import COMMAND test_stl_import)

# test_unfold — planar-net flatten engine (box -> area-preserving net)
add_executable(test_unfold test_unfold.cpp)
target_link_libraries(test_unfold PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_unfold COMMAND test_unfold)

# test_generative_edges — fillet corner anchored to sketch vertex survives resize
add_executable(test_generative_edges test_generative_edges.cpp)
target_link_libraries(test_generative_edges PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_generative_edges COMMAND test_generative_edges)

# bench_stl_import — manual stopwatch for STL import stages (not run by ctest)
add_executable(bench_stl_import bench_stl_import.cpp)
target_link_libraries(bench_stl_import PRIVATE materializr_core)

# probe_anchor_coverage — diagnostic: anchor classification of a real project's
# fillets against every sketch (experiment/generative-edges, not run by ctest)
add_executable(probe_anchor_coverage probe_anchor_coverage.cpp)
target_link_libraries(probe_anchor_coverage PRIVATE materializr_core)

# test_sketch_regions — multi-loop interior selection (wire-orientation fix)
add_executable(test_sketch_regions test_sketch_regions.cpp)
target_link_libraries(test_sketch_regions PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_regions COMMAND test_sketch_regions)

# probe_shell — diagnostic: reproduce Shell crash/failure headless (not ctest)
add_executable(probe_shell probe_shell.cpp)
target_link_libraries(probe_shell PRIVATE materializr_core)
target_compile_definitions(probe_shell PRIVATE OCC_CONVERT_SIGNALS)

# test_shell — ShellOp over-thick fails fast (no hang) + open-face rebind
add_executable(test_shell test_shell.cpp)
target_link_libraries(test_shell PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_shell COMMAND test_shell)

# probe_rebuild — repro: does fillet re-execution stack a phantom fillet
add_executable(probe_rebuild probe_rebuild.cpp)
target_link_libraries(probe_rebuild PRIVATE materializr_core)

# probe_fillet_repair — recover a fillet-corrupted project from its clean params
add_executable(probe_fillet_repair probe_fillet_repair.cpp)
target_link_libraries(probe_fillet_repair PRIVATE materializr_core)

# probe_undoredo — repro: phantom fillet via undo/redo cascade
add_executable(probe_undoredo probe_undoredo.cpp)
target_link_libraries(probe_undoredo PRIVATE materializr_core)

# probe_twist_face — de-risk: does a ruled ThruSections base->rotated-top loft
# produce a genuine twist, and at what angle does OCCT refuse?
add_executable(probe_twist_face probe_twist_face.cpp)
target_link_libraries(probe_twist_face PRIVATE materializr_core)

# test_twist_face — MoveFaceOp Kind::Twist: valid, twisted, volume-preserving
add_executable(test_twist_face test_twist_face.cpp)
target_link_libraries(test_twist_face PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_twist_face COMMAND test_twist_face)
