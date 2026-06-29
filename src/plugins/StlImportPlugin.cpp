#include "../plugin/PluginMacro.h"
#include "../core/Document.h"
#include "../core/EventBus.h"
#include "../core/Events.h"
#include "../io/StlIO.h"
#include "../io/FileDialogs.h"
#include <cstdio>

// STL import — PARKED / DISABLED.
//
// The importer (StlIO) works, but converting a triangle mesh into a B-rep solid
// produces a body with thousands of tiny faces, which makes the viewport's
// per-frame per-face hover-picking and the per-triangle wireframe unusably slow.
// Decimating the mesh enough to be smooth threw away too much fidelity. Until a
// mesh-native path exists (render the triangulation directly rather than a heavy
// B-rep), the menu entry stays disabled: the IO-format registration below is
// commented out, so "Import ▸ STL" does not appear. The code is retained (and
// covered by test_stl_import) so the rework has a working starting point.
//
// To re-enable, uncomment the ctx.registerIOFormat(...) block.
REGISTER_PLUGIN(StlImport, [](materializr::PluginContext& ctx) {
    (void)ctx;
#if 0
    ctx.registerIOFormat({"STL", {"stl"}, /*canImport=*/true, /*canExport=*/false,
        [](materializr::PluginContext& ctx, const std::string&) {
            materializr::FileDialogs::openFile("Import STL",
                {{"STL Files", "*.stl *.STL"}},
                [&ctx](const std::string& path) {
                    if (path.empty()) return;
                    auto result = materializr::StlIO::import(path, ctx.document());
                    if (result.success) {
                        ctx.markMeshesDirty();
                        ctx.events().publish(materializr::ToastEvent{
                            "Imported STL as a faceted mesh body. Good for "
                            "reference or boolean tools; fillet/chamfer/Move Face "
                            "may not work on it.",
                            9.0});
                    } else {
                        std::fprintf(stderr, "STL import failed: %s\n",
                                     result.errorMessage.c_str());
                        ctx.events().publish(materializr::ToastEvent{
                            "STL import failed: " + result.errorMessage, 6.0});
                    }
                });
            return true;
        },
        nullptr});
#endif
})
