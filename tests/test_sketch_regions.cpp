// Regression: interior selection of a multi-loop sketch. Two disjoint
// triangles produced BOP region faces whose wires contain TopAbs_REVERSED
// edges; densifyWire2D sampled those f->l regardless of orientation, so the
// densified polygon self-intersected and the even-odd point-in-polygon test
// miscounted — interior clicks missed and the loop was only selectable near
// its edges. densifyWire2D now honours the wire traversal orientation.
//
// (Mirrors the user's Sketch 3: triangle A (90,90)-(90,55)-(25,90) and
//  triangle B (90,35)-(90,0)-(25,0), both with an edge on x=90.)

#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <glm/glm.hpp>
#include <vector>

using materializr::Sketch;

namespace {

// A triangle from three (x,y) sketch-plane corners. Returns nothing; adds to sk.
void addTriangle(Sketch& sk, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    int pa = sk.addPoint(a), pb = sk.addPoint(b), pc = sk.addPoint(c);
    sk.addLine(pa, pb);
    sk.addLine(pb, pc);
    sk.addLine(pc, pa);
}

// Centroid of a triangle — guaranteed strictly interior.
glm::vec2 centroid(glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    return (a + b + c) / 3.0f;
}

} // namespace

TEST(SketchRegions, TwoTrianglesInteriorSelectable) {
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));

    const glm::vec2 a0(90, 90), a1(90, 55), a2(25, 90); // triangle A
    const glm::vec2 b0(90, 35), b1(90, 0),  b2(25, 0);  // triangle B
    addTriangle(sk, a0, a1, a2);
    addTriangle(sk, b0, b1, b2);

    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 2u) << "two disjoint triangles = two fill regions";

    // Each triangle's centroid must land INSIDE exactly one region via the
    // strict interior test (not merely the edge-proximity fallback).
    const glm::vec2 cA = centroid(a0, a1, a2);
    const glm::vec2 cB = centroid(b0, b1, b2);

    auto interiorHits = [&](glm::vec2 p) {
        int n = 0;
        for (const auto& r : regions) if (sk.isPointInRegion(r, p)) ++n;
        return n;
    };
    EXPECT_EQ(interiorHits(cA), 1) << "triangle A centroid must be strictly inside its region";
    EXPECT_EQ(interiorHits(cB), 1) << "triangle B centroid must be strictly inside its region";

    // A point clearly outside both triangles is inside neither.
    EXPECT_EQ(interiorHits(glm::vec2(0, 45)), 0);
}

// Single triangle sanity — the simple case must keep working.
TEST(SketchRegions, SingleTriangleInterior) {
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const glm::vec2 a(0, 0), b(60, 0), c(0, 40);
    addTriangle(sk, a, b, c);

    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_TRUE(sk.isPointInRegion(regions[0], centroid(a, b, c)));
    EXPECT_FALSE(sk.isPointInRegion(regions[0], glm::vec2(50, 35))); // outside hypotenuse
}
