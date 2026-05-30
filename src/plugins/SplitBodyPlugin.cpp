#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../modeling/SplitBodyOp.h"
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Shape.hxx>
#include <memory>

namespace {

// Split the first selected body with a plane through the body's bounding-box
// centre, oriented with the given normal. Anchoring at the centre (rather than
// the world origin) guarantees the plane actually passes through the solid — a
// world-origin z=0 plane misses any body resting above the XY plane, leaving the
// splitter to return it unchanged.
void doSplit(materializr::PluginContext& ctx, const gp_Dir& normal) {
    const auto& sel = ctx.selection().getSelection();
    if (sel.empty() || sel[0].bodyId < 0) return;

    gp_Pnt origin(0, 0, 0);
    try {
        const TopoDS_Shape& shape = ctx.document().getBody(sel[0].bodyId);
        Bnd_Box bb;
        BRepBndLib::Add(shape, bb);
        if (!bb.IsVoid()) {
            double xmin, ymin, zmin, xmax, ymax, zmax;
            bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            origin = gp_Pnt((xmin + xmax) * 0.5, (ymin + ymax) * 0.5, (zmin + zmax) * 0.5);
        }
    } catch (...) {}

    auto op = std::make_unique<SplitBodyOp>();
    op->setBody(sel[0].bodyId);
    op->setSplitPlane(gp_Pln(origin, normal));
    if (ctx.history().pushOperation(std::move(op), ctx.document())) {
        ctx.markMeshesDirty();
    }
}

} // namespace

REGISTER_PLUGIN(SplitBody, [](materializr::PluginContext& ctx) {
    // Axis labels follow user / 3D-printer convention (X = left/right,
    // Y = forward/back, Z = up). Materializr's world is Y-up internally,
    // so user-Y → world Z and user-Z → world Y. Hand-mirroring the helper
    // in Application::userAxisToWorldVec since plugins can't reach it.
    ctx.registerToolbarButton({"Split X", "Feature",
        materializr::SelectionContext::HasBodies, 502,
        [](materializr::PluginContext& ctx) { doSplit(ctx, gp_Dir(1, 0, 0)); }, nullptr,
        "Cut the selected body in half along the X axis (left / right halves)."});
    ctx.registerToolbarButton({"Split Y", "Feature",
        materializr::SelectionContext::HasBodies, 503,
        [](materializr::PluginContext& ctx) { doSplit(ctx, gp_Dir(0, 0, 1)); }, nullptr,
        "Cut the selected body in half along the Y axis (front / back halves)."});
    ctx.registerToolbarButton({"Split Z", "Feature",
        materializr::SelectionContext::HasBodies, 504,
        [](materializr::PluginContext& ctx) { doSplit(ctx, gp_Dir(0, 1, 0)); }, nullptr,
        "Cut the selected body in half along the Z axis (top / bottom halves)."});
})
