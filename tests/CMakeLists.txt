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
    ${CMAKE_SOURCE_DIR}/src/modeling/BlendCut.cpp
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
    ${CMAKE_SOURCE_DIR}/src/modeling/ThreadOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SubShapeIndex.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PlaneTransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/AxisTransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/CopyOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/AlignOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/MirrorOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SplitBodyOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SeparateBodyOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchTransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/LoftOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/GuidedLoftOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/BoundaryFillOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SweepOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ConstructionPlaneOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ConstructionAxisOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/OperationFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PatternOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/RevolveOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/TaperOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ScaleFaceOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/DefeatureOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ResizeCylindricalOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/CombineSketchesOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PrimitiveOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ReplayOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ProjectSketchOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/EdgeAnchor.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/FaceAnchor.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/TopoName.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/GenerationLedger.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ProjectIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ImageDecode.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/FaceLineage.cpp
    ${CMAKE_SOURCE_DIR}/src/io/BrepIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/IgesIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ObjExport.cpp
    ${CMAKE_SOURCE_DIR}/src/io/DxfExport.cpp
    ${CMAKE_SOURCE_DIR}/src/io/DxfImport.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ThreeMfExport.cpp
    ${CMAKE_SOURCE_DIR}/src/io/StlIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/MeshDecimate.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ProjectRecovery.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Verbose.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/Unfold.cpp
    ${CMAKE_SOURCE_DIR}/src/viewport/SectionCap.cpp
)

target_include_directories(materializr_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
# Mark OpenCASCADE as a SYSTEM include so its own deprecated declarations
# (e.g. std::iterator under C++17) don't spam our build with warnings.
target_include_directories(materializr_core SYSTEM PUBLIC
    ${OpenCASCADE_INCLUDE_DIR}
)

# OCCT 7.8 renamed the data-exchange toolkits (TKSTL -> TKDESTL). The app target
# version-gates this (see the top-level CMakeLists); the test lib must too, or it
# won't link on the pre-refactor OCCT that distros like Ubuntu 24.04 ship (7.6.3,
# where the STL toolkit is still TKSTL). Gate on MAJOR/MINOR — the config sets
# those even when it leaves the combined OpenCASCADE_VERSION string empty.
if(OpenCASCADE_MAJOR_VERSION EQUAL 7 AND OpenCASCADE_MINOR_VERSION LESS 8)
    set(OCCT_STL_LIB TKSTL)
    set(OCCT_IGES_LIB TKIGES)
else()
    set(OCCT_STL_LIB TKDESTL)
    set(OCCT_IGES_LIB TKDEIGES)
endif()

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
    ${OCCT_STL_LIB}  # RWStl (STL read) for StlIO — TKSTL (<7.8) / TKDESTL (>=7.8)
    ${OCCT_IGES_LIB} # IGESControl for IgesIO (test_exchange_io) — same version gate
    TKXSBase     # exchange base for the IGES reader/writer
    TKShHealing  # ShapeUpgrade_UnifySameDomain for StlIO
    TKMesh       # BRepMesh_IncrementalMesh for Unfold (curved faces)
    ZLIB::ZLIB
)

# ─── Test executables ────────────────────────────────────────────────────────

# test_document
add_executable(test_document test_document.cpp)
target_link_libraries(test_document PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_document COMMAND test_document)

# test_separate_body - Separate op splits disconnected solids into bodies
add_executable(test_separate_body test_separate_body.cpp)
target_link_libraries(test_separate_body PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_separate_body COMMAND test_separate_body)

# test_stepped_hole_profile - #60: stepped hole doesn't extrude a floating plug
add_executable(test_stepped_hole_profile test_stepped_hole_profile.cpp)
target_link_libraries(test_stepped_hole_profile PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_stepped_hole_profile COMMAND test_stepped_hole_profile)

# test_edgeop_highlight_reload - fallback recovers bevel faces without gen=
add_executable(test_edgeop_highlight_reload test_edgeop_highlight_reload.cpp)
target_link_libraries(test_edgeop_highlight_reload PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_edgeop_highlight_reload COMMAND test_edgeop_highlight_reload)

# test_topo_contract - THE topological-naming contract: one case per
# (reference kind x upstream-edit kind), driven through the real replay path.
add_executable(test_topo_contract test_topo_contract.cpp)
target_link_libraries(test_topo_contract PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_contract COMMAND test_topo_contract)

# test_thread_profiles - generalized thread cross-sections build valid solids
# (external + internal) + the fit-clearance path.
add_executable(test_thread_profiles test_thread_profiles.cpp)
target_link_libraries(test_thread_profiles PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_thread_profiles COMMAND test_thread_profiles)

# test_thread_reflow - ops on threaded bodies reflow beneath the Thread step
# (threads-last by reflow, not refusal) and the thread re-cuts on the result.
add_executable(test_thread_reflow test_thread_reflow.cpp)
target_link_libraries(test_thread_reflow PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_thread_reflow COMMAND test_thread_reflow)

# test_sketch_edit_cascade - editing a sketch drives a hole through a chamfer
add_executable(test_sketch_edit_cascade test_sketch_edit_cascade.cpp)
target_link_libraries(test_sketch_edit_cascade PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_edit_cascade COMMAND test_sketch_edit_cascade)

# test_extrude_recovered_profile - seed miss uses saved footprint, not ALL regions
add_executable(test_extrude_recovered_profile test_extrude_recovered_profile.cpp)
target_link_libraries(test_extrude_recovered_profile PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_extrude_recovered_profile COMMAND test_extrude_recovered_profile)

# test_section_cap - section-view cross-section cap geometry (issue #15)
add_executable(test_section_cap test_section_cap.cpp)
target_link_libraries(test_section_cap PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_section_cap COMMAND test_section_cap)

# test_history
add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_history COMMAND test_history)

# test_recovery — multi-instance recovery slots (flock claim/probe/orphan scan)
add_executable(test_recovery test_recovery.cpp)
target_link_libraries(test_recovery PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_recovery COMMAND test_recovery)

# Exchange formats: BREP round-trip, OBJ/DXF/3MF structural checks
add_executable(test_exchange_io test_exchange_io.cpp)
target_link_libraries(test_exchange_io PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_exchange_io COMMAND test_exchange_io)

# Face lineage: bevel identity through boolean splits + save/load (#49/#51)
add_executable(test_face_lineage test_face_lineage.cpp)
target_link_libraries(test_face_lineage PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_face_lineage COMMAND test_face_lineage)

# Extrude region identity: draw/extrude/draw-more replays correctly (#53)
add_executable(test_extrude_regions test_extrude_regions.cpp)
target_link_libraries(test_extrude_regions PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_extrude_regions COMMAND test_extrude_regions)

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

# test_pushpull_through — a cut consuming the whole body must be refused
add_executable(test_pushpull_through test_pushpull_through.cpp)
target_link_libraries(test_pushpull_through PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_pushpull_through COMMAND test_pushpull_through)

# test_duplicate_sketch — a duplicated sketch is severed from the source's body (#21)
add_executable(test_duplicate_sketch test_duplicate_sketch.cpp)
target_link_libraries(test_duplicate_sketch PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_duplicate_sketch COMMAND test_duplicate_sketch)

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

# test_topo_name — unified naming layer: best-first resolve, unknown-scheme
# skip + preservation, ordinal round-trip (experiment/face-anchors)
add_executable(test_topo_name test_topo_name.cpp)
target_link_libraries(test_topo_name PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_name COMMAND test_topo_name)

# test_topo_edge — EdgeAnchor migrated behind the registry ("sketchedge"):
# resolveSet distinctness + survives-resize through the unified layer
add_executable(test_topo_edge test_topo_edge.cpp)
target_link_libraries(test_topo_edge PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_edge COMMAND test_topo_edge)

# test_topo_thread — a live op (ThreadOp) follows an upstream cylinder move by
# re-resolving its target-face topo::Ref and re-deriving axis/radius (rung 3)
add_executable(test_topo_thread test_topo_thread.cpp)
target_link_libraries(test_topo_thread PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_thread COMMAND test_topo_thread)

# test_topo_gen — generation-map lineage naming: a prism side face named by its
# generating profile edge resolves correctly after a profile edit (rung 4)
add_executable(test_topo_gen test_topo_gen.cpp)
target_link_libraries(test_topo_gen PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_gen COMMAND test_topo_gen)

# bench_stl_import — manual stopwatch for STL import stages (not run by ctest)
add_executable(bench_stl_import bench_stl_import.cpp)
target_link_libraries(bench_stl_import PRIVATE materializr_core)

# probe_anchor_coverage — diagnostic: anchor classification of a real project's
# fillets against every sketch (experiment/generative-edges, not run by ctest)
add_executable(probe_anchor_coverage probe_anchor_coverage.cpp)
target_link_libraries(probe_anchor_coverage PRIVATE materializr_core)

# probe_face_coverage — diagnostic: generative FACE naming coverage of a real
# project's bodies against every sketch (experiment/face-anchors)
add_executable(probe_face_coverage probe_face_coverage.cpp)
target_link_libraries(probe_face_coverage PRIVATE materializr_core)

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


# test_topo_fillet_gen — FilletOp publishes a generation ledger; a blend face
# named by lineage survives a corner-moving resize (general-kernel on real op)
add_executable(test_topo_fillet_gen test_topo_fillet_gen.cpp)
target_link_libraries(test_topo_fillet_gen PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_fillet_gen COMMAND test_topo_fillet_gen)

# test_topo_chamfer_gen — ChamferOp generation ledger; bevel face named by lineage
add_executable(test_topo_chamfer_gen test_topo_chamfer_gen.cpp)
target_link_libraries(test_topo_chamfer_gen PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_chamfer_gen COMMAND test_topo_chamfer_gen)


# test_topo_pushpull — PushPullOp face-driven target via topo::Ref survives a resize
add_executable(test_topo_pushpull test_topo_pushpull.cpp)
target_link_libraries(test_topo_pushpull PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_pushpull COMMAND test_topo_pushpull)

# test_topo_shell — ShellOp opened faces via topo::Ref through a REAL history
# replay (stale-sketch rollback + cascade override) on an L-body where the
# geometric fallback mis-picks
add_executable(test_topo_shell test_topo_shell.cpp)
target_link_libraries(test_topo_shell PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_shell COMMAND test_topo_shell)

# probe_moveface_shell — diagnostic: does MoveFaceOp keep a shelled body's cavity?
add_executable(probe_moveface_shell probe_moveface_shell.cpp)
target_link_libraries(probe_moveface_shell PRIVATE materializr_core)

# test_moveface_hollow — MoveFace on a shelled body refuses cleanly instead of
# inverting the shell / silently filling the cavity (Steve's in-app corruption)
add_executable(test_moveface_hollow test_moveface_hollow.cpp)
target_link_libraries(test_moveface_hollow PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_moveface_hollow COMMAND test_moveface_hollow)

# probe_thread_loft — diagnostic: native twist-loft thread build timing/quality
add_executable(probe_thread_loft probe_thread_loft.cpp)
target_link_libraries(probe_thread_loft PRIVATE materializr_core)

# probe_thread_chunk — diagnostic: >40-turn rods as chunked swept segments
add_executable(probe_thread_chunk probe_thread_chunk.cpp)
target_link_libraries(probe_thread_chunk PRIVATE materializr_core)

# probe_thread_hole — diagnostic: boolean-path cost for internal threads
add_executable(probe_thread_hole probe_thread_hole.cpp)
target_link_libraries(probe_thread_hole PRIVATE materializr_core)

# test_topo_boolean_gen — boolean seam edge named by two-input gen lineage
# survives an upstream sketch edit (the unnameable-seam breakthrough)
add_executable(test_topo_boolean_gen test_topo_boolean_gen.cpp)
target_link_libraries(test_topo_boolean_gen PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_boolean_gen COMMAND test_topo_boolean_gen)

# probe_owns_face — diagnostic: which faces fillet/chamfer steps claim on reload
add_executable(probe_owns_face probe_owns_face.cpp)
target_link_libraries(probe_owns_face PRIVATE materializr_core)

# test_full_replay — THE contract: every op type reloads editable through the
# real ProjectIO and the whole chain replays from step 0 after a sketch edit
add_executable(test_full_replay test_full_replay.cpp)
target_link_libraries(test_full_replay PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_full_replay COMMAND test_full_replay)

# test_svg_roundtrip — sketch -> SVG -> sketch: loops stay closed, corners
# shared, circles native (Steve's re-import-the-logo breakage)
add_executable(test_svg_roundtrip test_svg_roundtrip.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SvgExport.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SvgImport.cpp)
# SvgImport renders SVG text to outline wires via StdPrs_BRepFont /
# StdPrs_BRepTextBuilder (TKV3d) on Font_* base classes (TKService) — the same
# font toolkits the app target links. materializr_core doesn't carry them, so
# this target must add them itself or it won't link.
target_link_libraries(test_svg_roundtrip PRIVATE materializr_core gtest gtest_main
    TKV3d TKService)
add_test(NAME test_svg_roundtrip COMMAND test_svg_roundtrip)

# probe_kernel_matrix — one-line-per-case battery of kernel-sensitive modeling
# ops for diffing behavior across OCCT versions (7.6.x apt vs 7.9.3 vs 8.0.0);
# fork-isolated cases so crashes/hangs report instead of killing the battery.
add_executable(probe_kernel_matrix probe_kernel_matrix.cpp)
target_link_libraries(probe_kernel_matrix PRIVATE materializr_core)

# probe_validity_fuzz — deterministic randomized validity sweep (same seed =>
# identical case stream on every OCCT version, for cross-kernel diffing).
add_executable(probe_validity_fuzz probe_validity_fuzz.cpp)
target_link_libraries(probe_validity_fuzz PRIVATE materializr_core)

# test_blend_cut — #55: chamfer across a surface feature via the swept-wedge
# cut fallback matches chamfer-then-cut exactly; native-success path untouched
add_executable(test_blend_cut test_blend_cut.cpp)
target_link_libraries(test_blend_cut PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_blend_cut COMMAND test_blend_cut)


# test_audit_hardening — audit residual-finding fixes: settings clamping and
# .cfg injection, transactional-on-failure ProjectIO::load, SVG vertex budget
add_executable(test_audit_hardening test_audit_hardening.cpp
    ${CMAKE_SOURCE_DIR}/src/io/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SvgImport.cpp)
target_link_libraries(test_audit_hardening PRIVATE materializr_core gtest gtest_main
    TKV3d TKService)
add_test(NAME test_audit_hardening COMMAND test_audit_hardening)

# test_brepio_dos — the BREP length-prefix DoS guard (impossible section count
# rejected fast, valid file still round-trips, truncated file fails cleanly)
add_executable(test_brepio_dos test_brepio_dos.cpp)
target_link_libraries(test_brepio_dos PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_brepio_dos COMMAND test_brepio_dos)

