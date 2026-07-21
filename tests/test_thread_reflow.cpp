// THREADS-LAST IS ENFORCED BY REFLOW, NOT REFUSAL. An op pushed onto a
// thread-modified body must reorder beneath the Thread step (the op runs
// against clean geometry, the thread re-cuts parametrically on the result)
// instead of being declined. Guards History::pushOperation's reflow path —
// the June-2026 "threads-last discipline" refusal is gone.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/History.h"
#include "modeling/BooleanOp.h"
#include "modeling/ThreadOp.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax2.hxx>
#include <memory>

using namespace materializr;

namespace {

double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

constexpr double R = 10.0, L = 9.0; // 3 coarse turns — fast

std::unique_ptr<ThreadOp> makeThread(int bodyId) {
    auto t = std::make_unique<ThreadOp>();
    t->setBody(bodyId);
    t->setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t->setRadius(R);
    t->setLength(L);
    t->setPitch(3.0);
    t->setDepth(1.2);
    t->setIsHole(false);
    return t;
}

// Transverse cutter cylinder through the rod at height z.
TopoDS_Shape transverseCutter(double z, double r = 2.0) {
    return BRepPrimAPI_MakeCylinder(
               gp_Ax2(gp_Pnt(-20.0, 0.0, z), gp_Dir(1, 0, 0)), r, 40.0)
        .Shape();
}

} // namespace

TEST(ThreadReflow, SubtractOnThreadedBodyReflowsBeneathThread) {
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    const double vRod = vol(rod);
    int rodId = doc.addBody(rod, "rod");

    ASSERT_TRUE(hist.pushOperation(makeThread(rodId), doc));
    const double vThreaded = vol(doc.getBody(rodId));
    ASSERT_LT(vThreaded, vRod);

    TopoDS_Shape cutter = transverseCutter(L * 0.5);
    const double vPlainHoled = vol(BRepAlgoAPI_Cut(rod, cutter).Shape());
    int cutId = doc.addBody(cutter, "cutter");
    auto cut = std::make_unique<BooleanOp>();
    cut->setTargetBodyId(rodId);
    cut->setToolBodyId(cutId);
    cut->setMode(BooleanMode::Subtract);

    // The old discipline REFUSED this push. It must now reflow and succeed.
    ASSERT_TRUE(hist.pushOperation(std::move(cut), doc));

    // The thread ended up LAST in history.
    ASSERT_GE(hist.currentStep(), 1);
    EXPECT_EQ(hist.getStep(hist.currentStep())->typeId(), "thread");
    EXPECT_EQ(hist.getStep(hist.currentStep() - 1)->typeId(), "boolean");

    // Result: valid, holed AND threaded (less material than the plain holed
    // rod — the re-cut thread grooves came back on the new geometry).
    TopoDS_Shape res = doc.getBody(rodId);
    ASSERT_FALSE(res.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
    EXPECT_LT(vol(res), vThreaded);
    EXPECT_LT(vol(res), vPlainHoled - 1.0);
    EXPECT_GT(vol(res), 0.5 * vRod);
}

TEST(ThreadReflow, SecondOpStacksAndThreadStaysLast) {
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    int rodId = doc.addBody(rod, "rod");
    ASSERT_TRUE(hist.pushOperation(makeThread(rodId), doc));

    double vPrev = vol(doc.getBody(rodId));
    for (double z : {L * 0.35, L * 0.7}) {
        int cutId = doc.addBody(transverseCutter(z, 1.5), "cutter");
        auto cut = std::make_unique<BooleanOp>();
        cut->setTargetBodyId(rodId);
        cut->setToolBodyId(cutId);
        cut->setMode(BooleanMode::Subtract);
        ASSERT_TRUE(hist.pushOperation(std::move(cut), doc)) << "z=" << z;
        double vNow = vol(doc.getBody(rodId));
        EXPECT_LT(vNow, vPrev) << "z=" << z;
        vPrev = vNow;
    }
    EXPECT_EQ(hist.getStep(hist.currentStep())->typeId(), "thread");
    EXPECT_TRUE(BRepCheck_Analyzer(doc.getBody(rodId)).IsValid());
}

TEST(ThreadReflow, RoundedRecutOnHoledRodKeepsSweptGeometry) {
    // The reflow re-cut of a SMOOTH profile on a modified rod must derive its
    // cutter from the sweep — falling into the rope groove instead produced
    // the deep-scoop "stacked discs" body (2026-07-21). Same-geometry check:
    // (rod ⊖ thread) ⊖ hole and (rod ⊖ hole) ⊖ thread must agree in volume.
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    int rodId = doc.addBody(rod, "rod");

    auto th = makeThread(rodId);
    th->setProfile(ThreadProfile::Rounded);
    TopoDS_Shape cutter = transverseCutter(L * 0.5);
    ThreadOp ref; // sweep on the clean rod = the approved gentle sine
    ref.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    ref.setRadius(R); ref.setLength(L); ref.setPitch(3.0); ref.setDepth(1.2);
    ref.setIsHole(false); ref.setProfile(ThreadProfile::Rounded);
    TopoDS_Shape sweptClean = ref.buildResult(rod);
    ASSERT_FALSE(sweptClean.IsNull());
    const double vExpected =
        vol(BRepAlgoAPI_Cut(sweptClean, cutter).Shape());

    ASSERT_TRUE(hist.pushOperation(std::move(th), doc));
    int cutId = doc.addBody(cutter, "cutter");
    auto cut = std::make_unique<BooleanOp>();
    cut->setTargetBodyId(rodId);
    cut->setToolBodyId(cutId);
    cut->setMode(BooleanMode::Subtract);
    ASSERT_TRUE(hist.pushOperation(std::move(cut), doc));

    TopoDS_Shape res = doc.getBody(rodId);
    ASSERT_FALSE(res.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
    // Rope scoops remove several times the sine grooves' material — a 1%
    // band on the expected volume rules them out without being brittle.
    EXPECT_NEAR(vol(res), vExpected, 0.01 * vExpected);
}

TEST(ThreadReflow, UndoRedoAcrossReflowedTimeline) {
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    const double vRod = vol(rod);
    int rodId = doc.addBody(rod, "rod");
    ASSERT_TRUE(hist.pushOperation(makeThread(rodId), doc));

    int cutId = doc.addBody(transverseCutter(L * 0.5), "cutter");
    auto cut = std::make_unique<BooleanOp>();
    cut->setTargetBodyId(rodId);
    cut->setToolBodyId(cutId);
    cut->setMode(BooleanMode::Subtract);
    ASSERT_TRUE(hist.pushOperation(std::move(cut), doc));
    const double vFinal = vol(doc.getBody(rodId));

    // The reflowed timeline is [boolean, thread]: undoing walks back to the
    // holed-unthreaded rod, then the clean rod; redoing rebuilds the result.
    ASSERT_TRUE(hist.undo(doc)); // pops the thread
    const double vHoled = vol(doc.getBody(rodId));
    EXPECT_GT(vHoled, vFinal);
    EXPECT_LT(vHoled, vRod);
    ASSERT_TRUE(hist.undo(doc)); // pops the boolean
    EXPECT_NEAR(vol(doc.getBody(rodId)), vRod, 1e-3);

    ASSERT_TRUE(hist.redo(doc));
    ASSERT_TRUE(hist.redo(doc));
    EXPECT_NEAR(vol(doc.getBody(rodId)), vFinal, 1e-3);
    EXPECT_TRUE(BRepCheck_Analyzer(doc.getBody(rodId)).IsValid());
}
