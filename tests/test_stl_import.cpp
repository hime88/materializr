// Verifies StlIO::import turns a triangle-soup STL into a clean solid:
//  - a 12-triangle binary cube imports as one body,
//  - sewing closes it into a SOLID,
//  - ShapeUpgrade_UnifySameDomain merges the 12 facets back to 6 planar faces,
//  - winding correction yields a positive volume.
// (The STL import menu entry is parked/disabled — see StlImportPlugin — but the
//  importer itself is exercised here so the parked code keeps building/working.)
#include <gtest/gtest.h>

#include "io/StlIO.h"
#include "core/Document.h"

#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cstdio>
#include <string>

using namespace materializr;

namespace {

// Append one ASCII-STL facet (the normal is ignored on read, so we emit zeros).
void writeTri(std::string& s,
              float ax, float ay, float az,
              float bx, float by, float bz,
              float cx, float cy, float cz) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        " facet normal 0 0 0\n"
        "  outer loop\n"
        "   vertex %g %g %g\n"
        "   vertex %g %g %g\n"
        "   vertex %g %g %g\n"
        "  endloop\n"
        " endfacet\n",
        ax, ay, az, bx, by, bz, cx, cy, cz);
    s += buf;
}

// Write an ASCII STL of an axis-aligned cube [0,s]^3 (12 triangles) to a temp file.
std::string writeCubeStl(float s) {
    const float o = 0.0f;
    std::string stl = "solid cube\n";
    writeTri(stl, o,o,o, s,o,o, s,s,o); writeTri(stl, o,o,o, s,s,o, o,s,o); // z=o
    writeTri(stl, o,o,s, s,s,s, s,o,s); writeTri(stl, o,o,s, o,s,s, s,s,s); // z=s
    writeTri(stl, o,o,o, s,o,s, s,o,o); writeTri(stl, o,o,o, o,o,s, s,o,s); // y=o
    writeTri(stl, o,s,o, s,s,o, s,s,s); writeTri(stl, o,s,o, s,s,s, o,s,s); // y=s
    writeTri(stl, o,o,o, o,s,o, o,s,s); writeTri(stl, o,o,o, o,s,s, o,o,s); // x=o
    writeTri(stl, s,o,o, s,s,s, s,s,o); writeTri(stl, s,o,o, s,o,s, s,s,s); // x=s
    stl += "endsolid cube\n";

    std::string path = std::string(std::tmpnam(nullptr)) + ".stl";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(stl.data(), 1, stl.size(), f);
    std::fclose(f);
    return path;
}

int countSubShapes(const TopoDS_Shape& s, TopAbs_ShapeEnum kind) {
    int n = 0;
    for (TopExp_Explorer ex(s, kind); ex.More(); ex.Next()) ++n;
    return n;
}

} // namespace

TEST(StlImport, CubeBecomesSolidWithSixFaces) {
    const float s = 10.0f;
    std::string path = writeCubeStl(s);

    Document doc;
    ImportResult res = StlIO::import(path, doc);
    std::remove(path.c_str());

    ASSERT_TRUE(res.success) << res.errorMessage;
    EXPECT_EQ(res.bodiesImported, 1);

    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 1u);

    const TopoDS_Shape& body = doc.getBody(ids[0]);
    ASSERT_FALSE(body.IsNull());

    // Sewing should have produced exactly one solid.
    EXPECT_EQ(countSubShapes(body, TopAbs_SOLID), 1);
    // UnifySameDomain should collapse the 12 coplanar facets to 6 cube faces.
    EXPECT_EQ(countSubShapes(body, TopAbs_FACE), 6);

    // Volume should be s^3 (winding correction makes it positive).
    GProp_GProps props;
    BRepGProp::VolumeProperties(body, props);
    EXPECT_NEAR(props.Mass(), static_cast<double>(s) * s * s, 1e-3);
}
