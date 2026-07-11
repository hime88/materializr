// Exchange formats round 1 (#41 BREP in/out, #42 OBJ export, #43 DXF sketch
// export, #44 3MF export). BREP is verified by geometric round-trip; the
// mesh/drawing exporters by structural parses of what they wrote — every
// numeric claim is checked against the source document, not just "file
// exists".
#include <gtest/gtest.h>

#include "../src/core/Document.h"
#include "../src/io/BrepIO.h"
#include "../src/io/DxfExport.h"
#include "../src/io/ObjExport.h"
#include "../src/io/ThreeMfExport.h"
#include "../src/modeling/Sketch.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax2.hxx>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace materializr;

namespace {

std::string tmpPath(const char* name) {
    return std::string(::testing::TempDir()) + name;
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Document twoBodyDoc() {
    Document doc;
    doc.addBody(BRepPrimAPI_MakeBox(20.0, 30.0, 40.0).Shape(), "Box");
    doc.addBody(BRepPrimAPI_MakeCylinder(
                    gp_Ax2(gp_Pnt(100.0, 0.0, 0.0), gp_Dir(0.0, 1.0, 0.0)),
                    10.0, 25.0).Shape(),
                "Cylinder");
    return doc;
}

} // namespace

// ── BREP: exact round-trip ──────────────────────────────────────────────────

TEST(BrepIO, RoundTripsTwoBodiesWithExactVolumes) {
    Document doc = twoBodyDoc();
    const std::string path = tmpPath("exchange.brep");

    auto ex = BrepIO::exportFile(path, doc);
    ASSERT_TRUE(ex.success) << ex.errorMessage;

    Document back;
    auto im = BrepIO::import(path, back);
    ASSERT_TRUE(im.success) << im.errorMessage;
    EXPECT_EQ(im.bodiesImported, 2);
    ASSERT_EQ(back.getAllBodyIds().size(), 2u);

    // Volumes survive exactly (BREP is lossless; the Y/Z-up rotations cancel).
    std::vector<double> want = {20.0 * 30.0 * 40.0, M_PI * 10.0 * 10.0 * 25.0};
    std::vector<double> got;
    for (int id : back.getAllBodyIds()) got.push_back(volumeOf(back.getBody(id)));
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    for (size_t i = 0; i < want.size(); ++i)
        EXPECT_NEAR(got[i], want[i], want[i] * 1e-9);
}

TEST(BrepIO, SingleBodyExportsBareAndReimports) {
    Document doc;
    doc.addBody(BRepPrimAPI_MakeBox(5.0, 5.0, 5.0).Shape(), "Cube");
    const std::string path = tmpPath("single.brep");
    ASSERT_TRUE(BrepIO::exportFile(path, doc).success);

    Document back;
    auto im = BrepIO::import(path, back);
    ASSERT_TRUE(im.success) << im.errorMessage;
    EXPECT_EQ(im.bodiesImported, 1);
    EXPECT_NEAR(volumeOf(back.getBody(back.getAllBodyIds().front())), 125.0, 1e-6);
}

TEST(BrepIO, ImportRejectsGarbage) {
    const std::string path = tmpPath("garbage.brep");
    { std::ofstream(path) << "this is not a brep file\n"; }
    Document doc;
    EXPECT_FALSE(BrepIO::import(path, doc).success);
    EXPECT_TRUE(doc.getAllBodyIds().empty());
}

// ── OBJ: structural parse of the written mesh ───────────────────────────────

TEST(ObjExport, WritesIndexedMeshWithValidReferences) {
    Document doc = twoBodyDoc();
    const std::string path = tmpPath("exchange.obj");
    auto ex = ObjExport::exportFile(path, doc);
    ASSERT_TRUE(ex.success) << ex.errorMessage;

    std::ifstream in(path);
    ASSERT_TRUE(in.good());
    long vCount = 0, fCount = 0, oCount = 0;
    long maxIndex = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("v ", 0) == 0) ++vCount;
        else if (line.rfind("o ", 0) == 0) ++oCount;
        else if (line.rfind("f ", 0) == 0) {
            ++fCount;
            long a = 0, b = 0, c = 0;
            ASSERT_EQ(std::sscanf(line.c_str(), "f %ld %ld %ld", &a, &b, &c), 3)
                << "unparsable face line: " << line;
            EXPECT_GE(a, 1); EXPECT_GE(b, 1); EXPECT_GE(c, 1);
            maxIndex = std::max({maxIndex, a, b, c});
        }
    }
    EXPECT_EQ(oCount, 2);            // one group per body
    EXPECT_GE(vCount, 8);            // at least the box's corners
    EXPECT_GE(fCount, 12);           // at least the box's triangles
    EXPECT_LE(maxIndex, vCount);     // every face references a real vertex
}

TEST(ObjExport, SkipsHiddenBodies) {
    Document doc = twoBodyDoc();
    auto ids = doc.getAllBodyIds();
    doc.setBodyVisible(ids.front(), false);
    const std::string path = tmpPath("hidden.obj");
    ASSERT_TRUE(ObjExport::exportFile(path, doc).success);
    const std::string text = slurp(path);
    long oCount = 0;
    for (size_t p = text.find("\no "); p != std::string::npos;
         p = text.find("\no ", p + 1))
        ++oCount;
    EXPECT_EQ(oCount, 1);
}

// ── DXF: entity-level parse against the source sketch ───────────────────────

TEST(DxfExport, WritesAllEntityKindsWithExactValues) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({50.0f, 0.0f});
    sk.addLine(a, b);
    int c = sk.addPoint({10.0f, 20.0f});
    sk.addCircle(c, 7.5);
    // CCW quarter arc: centre origin-ish, start +X, end +Y.
    int ac = sk.addPoint({-30.0f, 0.0f});
    int as = sk.addPoint({-20.0f, 0.0f});
    int ae = sk.addPoint({-30.0f, 10.0f});
    sk.addArc(ac, as, ae, 10.0);

    const std::string path = tmpPath("sketch.dxf");
    auto ex = DxfExport::exportSketch(path, sk);
    ASSERT_TRUE(ex.success) << ex.errorMessage;
    EXPECT_EQ(ex.entityCount, 3);

    const std::string text = slurp(path);
    EXPECT_NE(text.find("$ACADVER"), std::string::npos);
    EXPECT_NE(text.find("AC1009"), std::string::npos);
    EXPECT_NE(text.find("\nLINE\n"), std::string::npos);
    EXPECT_NE(text.find("\nCIRCLE\n"), std::string::npos);
    EXPECT_NE(text.find("\nARC\n"), std::string::npos);
    EXPECT_NE(text.find("\nEOF\n"), std::string::npos);
    // Circle radius written under group 40.
    EXPECT_NE(text.find("40\n7.500000"), std::string::npos);
    // Arc angles: group 50 (start) ≈ 0°, group 51 (end) ≈ 90°. Parsed, not
    // string-matched — the sketch stores points as float vec2 and addArc
    // re-derives the end point through float trig, so the written angle is
    // 90-ish to ~1e-5° (observed 90.000003), which is exactly right for the
    // exporter to pass through.
    auto groupAfterArc = [&](const char* code) -> double {
        size_t arc = text.find("\nARC\n");
        EXPECT_NE(arc, std::string::npos);
        size_t pos = text.find(std::string("\n") + code + "\n", arc);
        EXPECT_NE(pos, std::string::npos);
        return std::atof(text.c_str() + pos + 1 + std::strlen(code) + 1);
    };
    EXPECT_NEAR(groupAfterArc("50"), 0.0, 1e-3);
    EXPECT_NEAR(groupAfterArc("51"), 90.0, 1e-3);
}

TEST(DxfExport, RejectsSketchWithNoGeometry) {
    Sketch sk;
    sk.addPoint({0.0f, 0.0f}); // a bare point is not a cuttable entity
    const std::string path = tmpPath("empty.dxf");
    EXPECT_FALSE(DxfExport::exportSketch(path, sk).success);
}

// ── 3MF: container + model structure ────────────────────────────────────────

TEST(ThreeMfExport, WritesValidZipContainerWithModel) {
    Document doc = twoBodyDoc();
    const std::string path = tmpPath("exchange.3mf");
    auto ex = ThreeMfExport::exportFile(path, doc);
    ASSERT_TRUE(ex.success) << ex.errorMessage;

    const std::string zip = slurp(path);
    ASSERT_GT(zip.size(), 100u);
    // ZIP magic, end-of-central-directory record, and the three OPC parts.
    EXPECT_EQ(zip.compare(0, 4, "PK\x03\x04"), 0);
    EXPECT_NE(zip.find("PK\x05\x06"), std::string::npos);
    EXPECT_NE(zip.find("[Content_Types].xml"), std::string::npos);
    EXPECT_NE(zip.find("_rels/.rels"), std::string::npos);
    EXPECT_NE(zip.find("3D/3dmodel.model"), std::string::npos);

    // Model payload (stored entries → readable in place).
    EXPECT_NE(zip.find("unit=\"millimeter\""), std::string::npos);
    EXPECT_NE(zip.find("<object id=\"1\""), std::string::npos);
    EXPECT_NE(zip.find("<object id=\"2\""), std::string::npos);
    EXPECT_NE(zip.find("<triangle "), std::string::npos);
    EXPECT_NE(zip.find("<vertex "), std::string::npos);

    // EOCD says exactly 3 entries.
    size_t eocd = zip.rfind("PK\x05\x06");
    ASSERT_NE(eocd, std::string::npos);
    uint16_t entries;
    std::memcpy(&entries, zip.data() + eocd + 10, 2);
    EXPECT_EQ(entries, 3);
}

TEST(ThreeMfExport, FailsCleanlyWithNoBodies) {
    Document doc;
    const std::string path = tmpPath("empty.3mf");
    EXPECT_FALSE(ThreeMfExport::exportFile(path, doc).success);
}
