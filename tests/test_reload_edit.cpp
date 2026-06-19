// Regression tests for: editing an operation that is UPSTREAM of a boolean /
// delete in a RELOADED project silently did nothing.
//
// Root cause (project-box.materializr, 2026-06): boolean & delete ops carried
// no serialised params, so on reload they came back as baked ReplayOps that
// replay a stale saved shape. Editing a fillet feeding a downstream union thus
// rebuilt the fillet but the baked union overwrote it — the change vanished and
// the "Edit Fillet" affordance disappeared (its regenerated faces matched
// nothing in the stale body). The fix makes BooleanOp/DeleteOp rehydrate as
// real, re-executable ops (and restore consumed bodies under their original id
// via putBody so an editStep replay can re-run them).

#include "core/Document.h"
#include "core/History.h"
#include "core/Operation.h"
#include "modeling/FilletOp.h"
#include "modeling/BooleanOp.h"
#include "modeling/DeleteOp.h"
#include "modeling/TransformOp.h"
#include "modeling/PushPullOp.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Pln.hxx>
#include <gp_Dir.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <cmath>
#include <vector>

namespace {

TopoDS_Shape boxAt(double x, double y, double z) {
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), 10.0, 10.0, 10.0).Shape();
}

std::vector<TopoDS_Edge> firstEdge(const TopoDS_Shape& s) {
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next())
        return { TopoDS::Edge(ex.Current()) };
    return {};
}

double volume(Document& d, int id) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(d.getBody(id), g);
    return g.Mass();
}

double surfaceArea(Document& d, int id) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(d.getBody(id), g);
    return g.Mass();
}

} // namespace

// The headline regression: a reloaded Boolean Union downstream of an editable
// fillet must RECOMPUTE when the fillet is edited — not bake over the change.
TEST(ReloadEdit, EditingFilletUpstreamOfReloadedBooleanPropagates) {
    // --- 1. Produce the "saved" geometry by running the real ops once. Two
    //        disjoint boxes; fillet an edge of the tool (B), then union into A.
    Document src;
    int A = src.addBody(boxAt(0, 0, 0), "A");
    int B = src.addBody(boxAt(20, 0, 0), "B");
    const TopoDS_Shape A0 = src.getBody(A);
    const TopoDS_Shape B0 = src.getBody(B);

    FilletOp f0;
    f0.setBody(B);
    f0.setEdges(firstEdge(B0));
    f0.setRadius(1.0);
    ASSERT_TRUE(f0.execute(src));
    const TopoDS_Shape Bf = src.getBody(B);

    BooleanOp b0;
    b0.setTargetBodyId(A);
    b0.setToolBodyId(B);
    b0.setMode(BooleanMode::Union);
    ASSERT_TRUE(b0.execute(src));
    const TopoDS_Shape Uf = src.getBody(A);

    const std::string fParams = f0.serializeParams();
    const std::string bParams = b0.serializeParams();
    ASSERT_FALSE(bParams.empty()) << "BooleanOp must serialise params to reload as a real op";

    // --- 2. Simulate project reload (mirrors Application::rebuildHistoryFromProject):
    //        fresh doc at the FINAL saved state, history rebuilt from real ops.
    Document doc;
    doc.putBody(A, A0, "A");
    doc.putBody(B, B0, "B");
    doc.updateBody(A, Uf);   // final state: A holds the union ...
    doc.removeBody(B);       // ... and B was consumed.

    History H;
    {
        auto f = std::make_unique<FilletOp>();
        ASSERT_TRUE(f->deserializeParams(fParams));
        Operation::ReloadState rs;
        rs.modifiedBefore = {{B, B0}};
        rs.modifiedAfter  = {{B, Bf}};
        ASSERT_TRUE(f->rehydrateFromReload(rs, doc));
        H.pushExecuted(std::move(f));
    }
    {
        auto b = std::make_unique<BooleanOp>();
        ASSERT_TRUE(b->deserializeParams(bParams));
        EXPECT_EQ(b->getTargetBodyId(), A);
        EXPECT_EQ(b->getToolBodyId(), B);
        EXPECT_EQ(b->getMode(), BooleanMode::Union);
        Operation::ReloadState rs;
        rs.modifiedBefore = {{A, A0}};
        rs.modifiedAfter  = {{A, Uf}};
        rs.deletedBefore  = {{B, Bf}};
        // The crux: boolean must rehydrate as a REAL op (true), not bake.
        ASSERT_TRUE(b->rehydrateFromReload(rs, doc));
        H.pushExecuted(std::move(b));
    }

    const double vBefore = volume(doc, A);

    // --- 3. Edit the fillet to a larger radius and replay the history.
    auto* f = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(H.getStep(0)));
    ASSERT_NE(f, nullptr) << "fillet step must reload as a real, editable FilletOp";
    f->setRadius(3.0);
    ASSERT_TRUE(H.editStep(0, doc))
        << "editStep must succeed — re-running the fillet and the downstream union";

    const double vAfter = volume(doc, A);

    // A larger fillet removes more material from B, so the union volume must
    // shrink. If the boolean had baked (the bug), vAfter would equal vBefore.
    EXPECT_LT(vAfter, vBefore - 1.0)
        << "the fillet edit did not propagate through the reloaded boolean "
           "(vBefore=" << vBefore << " vAfter=" << vAfter << ")";
}

// Free-space sketch push/pull with cut-intersecting: subtracts from VISIBLE
// intersecting bodies (separately), skips hidden ones, and falls back to a new
// body when it hits nothing. (The "free-space sketch that runs into a body
// cuts it" feature.)
namespace {
PushPullOp makeCutOp(const TopoDS_Face& profile) {
    PushPullOp op;
    PushPullOp::Target t; t.profile = profile; t.sourceBodyId = -1;
    op.setTargets({t});
    op.setDistance(15.0);
    op.setSymmetric(true);          // sweep both ways → guaranteed to reach
    op.setCutIntersecting(true);
    return op;
}
TopoDS_Face xyProfile(double x0, double x1, double y0, double y1) {
    return BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0,0,0), gp_Dir(0,0,1)),
                                   x0, x1, y0, y1).Face();
}
} // namespace

TEST(SmartCut, IntersectingVisibleBodyIsCutNotNewBody) {
    Document d;
    int b = d.addBody(boxAt(0,0,0), "box");
    const double v0 = volume(d, b);
    auto op = makeCutOp(xyProfile(4,6,4,6));   // small column through the box
    ASSERT_TRUE(op.execute(d));
    EXPECT_EQ(d.getAllBodyIds().size(), 1u) << "should NOT create a new body";
    EXPECT_LT(volume(d, b), v0 - 1.0) << "box should lose the cut volume";
}

TEST(SmartCut, NonIntersectingFallsBackToNewBody) {
    Document d;
    int b = d.addBody(boxAt(0,0,0), "box");
    const double v0 = volume(d, b);
    auto op = makeCutOp(xyProfile(100,102,100,102)); // far away
    ASSERT_TRUE(op.execute(d));
    EXPECT_EQ(d.getAllBodyIds().size(), 2u) << "should create a new body";
    EXPECT_DOUBLE_EQ(volume(d, b), v0) << "the box must be untouched";
}

TEST(SmartCut, HiddenBodiesAreSkipped) {
    Document d;
    int b = d.addBody(boxAt(0,0,0), "box");
    d.setBodyVisible(b, false);
    const double v0 = volume(d, b);
    auto op = makeCutOp(xyProfile(4,6,4,6));
    ASSERT_TRUE(op.execute(d));
    EXPECT_DOUBLE_EQ(volume(d, b), v0) << "a hidden body must not be cut";
    EXPECT_EQ(d.getAllBodyIds().size(), 2u) << "falls back to a new body";
}

TEST(SmartCut, TwoBodiesCutSeparately) {
    Document d;
    int b1 = d.addBody(boxAt(0,0,0), "b1");
    int b2 = d.addBody(boxAt(20,0,0), "b2");
    const double v1 = volume(d, b1), v2 = volume(d, b2);
    auto op = makeCutOp(xyProfile(-1,31,4,6));  // slab spanning both
    ASSERT_TRUE(op.execute(d));
    EXPECT_EQ(d.getAllBodyIds().size(), 2u) << "both bodies stay separate";
    EXPECT_LT(volume(d, b1), v1 - 1.0);
    EXPECT_LT(volume(d, b2), v2 - 1.0);
}

// A cut whose sketch IS attached to a body (sourceBodyId set) must still cut
// THROUGH to the other bodies in its path — the original bug report: a sketch
// drawn on the lid, cut downward, only cut the lid and ignored the box.
TEST(SmartCut, AttachedCutGoesThroughOtherBodies) {
    Document d;
    int lid = d.addBody(boxAt(0,0,0), "lid");   // the source body
    int box = d.addBody(boxAt(20,0,0), "box");  // another body in the path
    const double vl = volume(d, lid), vb = volume(d, box);
    PushPullOp op;
    PushPullOp::Target t;
    t.profile = xyProfile(-1,31,4,6);
    t.sourceBodyId = lid;                       // sketch attached to the lid
    op.setTargets({t});
    op.setDistance(-15.0);                       // cut direction
    op.setSymmetric(true);
    op.setCutIntersecting(true);
    ASSERT_TRUE(op.execute(d));
    EXPECT_EQ(d.getAllBodyIds().size(), 2u);
    EXPECT_LT(volume(d, lid), vl - 1.0) << "the source (lid) is cut";
    EXPECT_LT(volume(d, box), vb - 1.0) << "the other body in the path is also cut";
}

// BooleanOp params round-trip cleanly (target/tool/mode), for each mode.
TEST(ReloadEdit, BooleanParamsRoundTrip) {
    for (BooleanMode m : {BooleanMode::Union, BooleanMode::Subtract, BooleanMode::Intersect}) {
        BooleanOp a;
        a.setTargetBodyId(7);
        a.setToolBodyId(12);
        a.setMode(m);
        BooleanOp b;
        ASSERT_TRUE(b.deserializeParams(a.serializeParams()));
        EXPECT_EQ(b.getTargetBodyId(), 7);
        EXPECT_EQ(b.getToolBodyId(), 12);
        EXPECT_EQ(b.getMode(), m);
    }
}

// Editing a fillet UPSTREAM of a reloaded transform must propagate — the
// transform re-applies to the edited live body instead of baking its stale
// result over the change.
TEST(ReloadEdit, EditUpstreamOfReloadedTransformPropagates) {
    Document src;
    int B = src.addBody(boxAt(0, 0, 0), "B");
    const TopoDS_Shape B0 = src.getBody(B);

    FilletOp f0;
    f0.setBody(B);
    f0.setEdges(firstEdge(B0));
    f0.setRadius(1.0);
    ASSERT_TRUE(f0.execute(src));
    const TopoDS_Shape Bf = src.getBody(B);

    TransformOp t0;
    t0.setBodyId(B);
    t0.setType(TransformType::Translate);
    t0.setTranslation(50.0, 0.0, 0.0);
    ASSERT_TRUE(t0.execute(src));
    const TopoDS_Shape Bt = src.getBody(B);

    const std::string fParams = f0.serializeParams();
    const std::string tParams = t0.serializeParams();
    ASSERT_FALSE(tParams.empty()) << "TransformOp must serialise params";

    // Reload: fresh doc at the final (translated) state, history rebuilt.
    Document doc;
    doc.putBody(B, B0, "B");
    doc.updateBody(B, Bt);
    History H;
    {
        auto f = std::make_unique<FilletOp>();
        ASSERT_TRUE(f->deserializeParams(fParams));
        Operation::ReloadState rs;
        rs.modifiedBefore = {{B, B0}}; rs.modifiedAfter = {{B, Bf}};
        ASSERT_TRUE(f->rehydrateFromReload(rs, doc));
        H.pushExecuted(std::move(f));
    }
    {
        auto t = std::make_unique<TransformOp>();
        ASSERT_TRUE(t->deserializeParams(tParams));
        Operation::ReloadState rs;
        rs.modifiedBefore = {{B, Bf}}; rs.modifiedAfter = {{B, Bt}};
        ASSERT_TRUE(t->rehydrateFromReload(rs, doc));
        H.pushExecuted(std::move(t));
    }

    const double aBefore = surfaceArea(doc, B);
    auto* f = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(H.getStep(0)));
    ASSERT_NE(f, nullptr);
    f->setRadius(3.0);
    ASSERT_TRUE(H.editStep(0, doc));
    const double aAfter = surfaceArea(doc, B);
    EXPECT_GT(std::abs(aAfter - aBefore), 1.0)
        << "fillet edit did not propagate through the reloaded transform";
}

// Legacy transforms (no params blob) reconstruct their rigid transform from the
// step's before/after snapshots.
TEST(ReloadEdit, RigidTrsfReconstruction) {
    TopoDS_Shape before = boxAt(0, 0, 0);
    gp_Trsf t; t.SetTranslation(gp_Vec(7.0, -3.0, 11.0));
    BRepBuilderAPI_Transform xf(before, t, true);
    xf.Build();
    ASSERT_TRUE(xf.IsDone());
    TopoDS_Shape after = xf.Shape();

    gp_Trsf recovered;
    ASSERT_TRUE(TransformOp::rigidTrsfBetween(before, after, recovered));
    // A probe point on `before` maps to the same place under the recovered trsf.
    gp_Pnt p(2.0, 5.0, 1.0);
    EXPECT_LT(p.Transformed(t).Distance(p.Transformed(recovered)), 1e-6);
}

// DeleteOp reloads as a real op: params round-trip and rehydrate recovers the
// deleted shape so an editStep replay can roll it back.
TEST(ReloadEdit, DeleteRehydrates) {
    Document src;
    int id = src.addBody(boxAt(0, 0, 0), "victim");
    const TopoDS_Shape shape = src.getBody(id);

    DeleteOp d0;
    d0.setBodyId(id);
    ASSERT_TRUE(d0.execute(src));
    const std::string params = d0.serializeParams();
    ASSERT_FALSE(params.empty());

    DeleteOp d1;
    ASSERT_TRUE(d1.deserializeParams(params));
    EXPECT_EQ(d1.getBodyId(), id);

    Document doc;  // freshly reloaded: the body is already gone
    Operation::ReloadState rs;
    rs.deletedBefore = {{id, shape}};
    ASSERT_TRUE(d1.rehydrateFromReload(rs, doc));

    // undo() restores the body under its ORIGINAL id (so upstream refs resolve).
    ASSERT_TRUE(d1.undo(doc));
    EXPECT_NO_THROW(doc.getBody(id));
}
