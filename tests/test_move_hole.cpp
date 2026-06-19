// Geometry tests for MoveHoleOp: sliding a THROUGH-HOLE across its face must
// conserve volume (fill old + cut new, same-size void) and relocate the hole
// (old centre becomes solid, new centre becomes void). Works for round AND
// square (any prismatic section). Pockets are refused.

#include "core/Document.h"
#include "modeling/MoveHoleOp.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRep_Tool.hxx>
#include <BRepGProp_Face.hxx>
#include <Geom_Surface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <cmath>

namespace {

double volume(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

int countFaces(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) ++n;
    return n;
}

// IN = inside material, OUT = empty (hole/outside).
bool isSolidAt(const TopoDS_Shape& s, double x, double y, double z) {
    BRepClass3d_SolidClassifier cl(s, gp_Pnt(x, y, z), 1e-7);
    return cl.State() == TopAbs_IN;
}

TopoDS_Face findCylWall(const TopoDS_Shape& s) {
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
        if (!surf.IsNull() && surf->IsKind(STANDARD_TYPE(Geom_CylindricalSurface)))
            return f;
    }
    return TopoDS_Face();
}

// The interior wall of a prismatic (square) hole: a vertical planar face whose
// centroid in XY is closest to the hole centre (outer box walls sit far away).
TopoDS_Face findInteriorWall(const TopoDS_Shape& s, double cx, double cy) {
    TopoDS_Face best; double bestD = 1e300;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
        if (surf.IsNull() || !surf->IsKind(STANDARD_TYPE(Geom_Plane))) continue;
        BRepGProp_Face gf(f);
        double u1, u2, v1, v2; gf.Bounds(u1, u2, v1, v2);
        gp_Pnt p; gp_Vec n; gf.Normal(0.5*(u1+u2), 0.5*(v1+v2), p, n);
        if (std::abs(n.Z()) > 0.1) continue; // want vertical walls only
        double d = std::hypot(p.X() - cx, p.Y() - cy);
        if (d < bestD) { bestD = d; best = f; }
    }
    return best;
}

} // namespace

TEST(MoveHole, RoundThroughHoleRelocatesAndConservesVolume) {
    // 20×20×10 box with a Ø4 hole through Z at (5,5).
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 10).Shape();
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(5,5,-1), gp_Dir(0,0,1)), 2.0, 12.0).Shape();
    TopoDS_Shape part = BRepAlgoAPI_Cut(box, cyl).Shape();

    Document doc;
    int id = doc.addBody(part, "part");
    double v0 = volume(part);
    int f0 = countFaces(part);
    ASSERT_TRUE(isSolidAt(part, 11,5,5)) << "new spot starts solid";
    ASSERT_FALSE(isSolidAt(part, 5,5,5)) << "old spot starts void";

    TopoDS_Face wall = findCylWall(doc.getBody(id));
    ASSERT_FALSE(wall.IsNull());

    MoveHoleOp op;
    op.setBody(id);
    op.setSeedWall(wall);
    op.setMoveVector(gp_Vec(6, 0, 0));      // slide +6 in X → centre (11,5)
    ASSERT_TRUE(op.execute(doc));
    EXPECT_FALSE(op.wasPocket());

    TopoDS_Shape moved = doc.getBody(id);
    EXPECT_NEAR(volume(moved), v0, 1e-6) << "same-size hole, just moved";
    EXPECT_TRUE(isSolidAt(moved, 5,5,5))  << "old hole filled solid";
    EXPECT_FALSE(isSolidAt(moved, 11,5,5)) << "new hole is void";
    EXPECT_EQ(countFaces(moved), f0)
        << "no ghost face/edge left where the hole was (unified)";
}

TEST(MoveHole, SquareThroughHoleRelocates) {
    // 20×20×10 box with a 4×4 square hole through Z, centred at (6,6).
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 10).Shape();
    TopoDS_Shape inner = BRepPrimAPI_MakeBox(gp_Pnt(4,4,-1), 4, 4, 12).Shape();
    TopoDS_Shape part = BRepAlgoAPI_Cut(box, inner).Shape();

    Document doc;
    int id = doc.addBody(part, "part");
    double v0 = volume(part);
    int f0 = countFaces(part);
    ASSERT_FALSE(isSolidAt(part, 6,6,5)) << "old square hole is void";

    TopoDS_Face wall = findInteriorWall(doc.getBody(id), 6, 6);
    ASSERT_FALSE(wall.IsNull());

    MoveHoleOp op;
    op.setBody(id);
    op.setSeedWall(wall);
    op.setMoveVector(gp_Vec(6, 0, 0));      // → centred (12,6)
    ASSERT_TRUE(op.execute(doc));

    TopoDS_Shape moved = doc.getBody(id);
    EXPECT_NEAR(volume(moved), v0, 1e-6);
    EXPECT_TRUE(isSolidAt(moved, 6,6,5))   << "old square hole filled";
    EXPECT_FALSE(isSolidAt(moved, 12,6,5)) << "new square hole is void";
    EXPECT_EQ(countFaces(moved), f0) << "no ghost face/edge left behind";
}

TEST(MoveHole, PocketIsRefused) {
    // 20×20×10 box with a BLIND Ø4 pocket from the top (z=10) down to z=4.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 10).Shape();
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(5,5,4), gp_Dir(0,0,1)), 2.0, 7.0).Shape(); // into top, blind
    TopoDS_Shape part = BRepAlgoAPI_Cut(box, cyl).Shape();

    Document doc;
    int id = doc.addBody(part, "part");

    TopoDS_Face wall = findCylWall(doc.getBody(id));
    ASSERT_FALSE(wall.IsNull());

    MoveHoleOp op;
    op.setBody(id);
    op.setSeedWall(wall);
    op.setMoveVector(gp_Vec(6, 0, 0));
    EXPECT_FALSE(op.execute(doc)) << "pocket move must be refused (v1)";
    EXPECT_TRUE(op.wasPocket());
}
