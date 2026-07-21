// Generalized thread profiles: every cross-section family must build a VALID
// solid, external AND internal, at a coarse printed-thread pitch. Guards the
// ThreadOp profile generalization (Standard V + the maker set: Trapezoidal,
// Square, Buttress, Rounded) and the fit-clearance path.
#include <gtest/gtest.h>

#include "modeling/ThreadOp.h"

#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax2.hxx>

using namespace materializr;

namespace {
double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}
// A tube (outer cylinder minus a coaxial bore) so the inner face can be
// threaded internally.
TopoDS_Shape tube(double rOuter, double rBore, double len) {
    TopoDS_Shape solid = BRepPrimAPI_MakeCylinder(rOuter, len).Shape();
    TopoDS_Shape bore  = BRepPrimAPI_MakeCylinder(rBore,  len).Shape();
    return BRepAlgoAPI_Cut(solid, bore).Shape();
}
void configure(ThreadOp& t, double r, double len, ThreadProfile p, double clr) {
    t.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t.setRadius(r);
    t.setLength(len);
    t.setPitch(3.0);       // coarse — the printed-thread case
    t.setDepth(1.2);
    t.setProfile(p);
    t.setClearance(clr);
}
} // namespace

TEST(ThreadProfiles, ExternalEachProfileValid) {
    const double R = 10.0, L = 9.0;  // 3 turns — fast
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, L).Shape();
    for (int i = 0; i <= static_cast<int>(ThreadProfile::Rounded); ++i) {
        ThreadOp t;
        configure(t, R, L, static_cast<ThreadProfile>(i), 0.0);
        t.setIsHole(false);
        TopoDS_Shape rod = t.buildResult(cyl);
        ASSERT_FALSE(rod.IsNull()) << "profile " << i << " built no solid";
        EXPECT_TRUE(BRepCheck_Analyzer(rod).IsValid()) << "profile " << i;
        // A thread removes material (or, for a rounded rope groove, only a
        // little) — never grows the body.
        EXPECT_LT(vol(rod), vol(cyl) + 1e-3) << "profile " << i;
        EXPECT_GT(vol(rod), 0.5 * vol(cyl)) << "profile " << i;
    }
}

TEST(ThreadProfiles, InternalEachProfileValid) {
    const double R = 10.0, L = 9.0;
    TopoDS_Shape t8 = tube(16.0, R, L);   // bore radius R = thread radius
    ASSERT_FALSE(t8.IsNull());
    for (int i = 0; i <= static_cast<int>(ThreadProfile::Rounded); ++i) {
        ThreadOp t;
        configure(t, R, L, static_cast<ThreadProfile>(i), 0.0);
        t.setIsHole(true);
        TopoDS_Shape res = t.buildResult(t8);
        ASSERT_FALSE(res.IsNull()) << "internal profile " << i << " built nothing";
        EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid()) << "internal profile " << i;
    }
}

TEST(ThreadProfiles, ClearanceThinsExternalThread) {
    // A fit clearance pulls the crest in, so the cleared thread has strictly
    // less material than the exact one (it fits its mate).
    const double R = 10.0, L = 9.0;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(R, L).Shape();
    auto build = [&](double clr) {
        ThreadOp t;
        configure(t, R, L, ThreadProfile::Trapezoidal, clr);
        t.setIsHole(false);
        return t.buildResult(cyl);
    };
    TopoDS_Shape exact = build(0.0), cleared = build(0.4);
    ASSERT_FALSE(exact.IsNull());
    ASSERT_FALSE(cleared.IsNull());
    EXPECT_LT(vol(cleared), vol(exact))
        << "clearance should remove more material (thinner thread)";
}
