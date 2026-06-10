#include "ProjectSketchOp.h"
#include "Sketch.h"
#include "SubShapeIndex.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <BRepProj_Projection.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <imgui.h>

namespace {

double shapeVolume(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

double faceArea(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(s, g);
    return g.Mass();
}

// Project one sketch wire onto the target face; of the closed candidates
// (a full cylindrical face yields a near AND a far wire), keep the one
// nearest the sketch plane along the projection direction.
TopoDS_Wire projectNearest(const TopoDS_Wire& w, const TopoDS_Face& f,
                           const gp_Dir& dir, const gp_Pnt& sketchOrigin) {
    TopoDS_Wire best;
    int total = 0, closedCount = 0;
    try {
        BRepProj_Projection proj(w, f, dir);
        double bestD = 1e100;
        for (; proj.More(); proj.Next()) {
            TopoDS_Wire c = proj.Current();
            ++total;
            if (!BRep_Tool::IsClosed(c)) continue;
            ++closedCount;
            GProp_GProps g;
            BRepGProp::LinearProperties(c, g);
            gp_Vec d(sketchOrigin, g.CentreOfMass());
            double t = d.Dot(gp_Vec(dir));
            if (t < bestD) { bestD = t; best = c; }
        }
    } catch (...) {}
    if (best.IsNull()) {
        std::fprintf(stderr,
            "[ProjectSketch]   projection produced %d wire(s), %d closed "
            "— need at least one closed wire to stamp.\n",
            total, closedCount);
    }
    return best;
}

// One projected wire as an oriented face ON the target's surface.
// ShapeFix_Face supplies pcurves / natural-bound trimming on periodic
// surfaces; a clockwise-wound wire shows up as negative area, in which
// case one reversed retry fixes it.
TopoDS_Face singleWireFace(const Handle(Geom_Surface)& surf, TopoDS_Wire w) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        BRepBuilderAPI_MakeFace mf(surf, w);
        if (!mf.IsDone()) return TopoDS_Face();
        ShapeFix_Face fix(mf.Face());
        fix.FixOrientationMode() = 1;
        fix.FixWireMode() = 1;
        fix.Perform();
        TopoDS_Face cand = fix.Face();
        if (faceArea(cand) > 0.0 && BRepCheck_Analyzer(cand).IsValid())
            return cand;
        w.Reverse();
    }
    return TopoDS_Face();
}

// Region face on the target surface: outer wire face MINUS hole wire
// faces, via a boolean cut. Building outer+holes into one MakeFace needs
// every wire's winding coordinated — with several projected holes the
// orientation search chased its tail (a six-bladed aperture logo failed
// both flip attempts). Single-wire faces orient reliably, and the cut
// needs no orientation reasoning at all.
TopoDS_Shape faceOnSurface(const TopoDS_Face& target, TopoDS_Wire outer,
                           const std::vector<TopoDS_Wire>& holes) {
    Handle(Geom_Surface) surf = BRep_Tool::Surface(target);
    TopoDS_Face outerF = singleWireFace(surf, outer);
    if (outerF.IsNull()) return TopoDS_Shape();
    if (holes.empty()) return outerF;

    TopTools_ListOfShape holeFaces;
    for (const auto& h : holes) {
        TopoDS_Face hf = singleWireFace(surf, h);
        if (hf.IsNull()) return TopoDS_Shape();
        holeFaces.Append(hf);
    }
    try {
        BRepAlgoAPI_Cut cut;
        TopTools_ListOfShape args;
        args.Append(outerF);
        cut.SetArguments(args);
        cut.SetTools(holeFaces);
        cut.Build();
        if (!cut.IsDone()) return TopoDS_Shape();
        TopoDS_Shape res = cut.Shape();
        if (res.IsNull() || faceArea(res) <= 0.0) return TopoDS_Shape();
        return res;
    } catch (...) { return TopoDS_Shape(); }
}

} // namespace

ProjectSketchOp::ProjectSketchOp() = default;

void ProjectSketchOp::setBody(int id) { m_bodyId = id; }
void ProjectSketchOp::setTargetFace(const TopoDS_Face& f) { m_targetFace = f; }
void ProjectSketchOp::setSketchId(int id) { m_sketchId = id; }
void ProjectSketchOp::setRegionFilter(std::vector<int> indices) {
    m_regionFilter = std::move(indices);
}
void ProjectSketchOp::setDepth(double d) { m_depth = d; }
void ProjectSketchOp::setMode(Mode m) { m_mode = m; }

bool ProjectSketchOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_sketchId < 0 || m_targetFace.IsNull() ||
        m_depth < 0.01) {
        return false;
    }
    auto sketch = doc.getSketch(m_sketchId);
    if (!sketch) {
        std::fprintf(stderr, "[ProjectSketch] sketch %d not found\n",
                     m_sketchId);
        return false;
    }
    try {
        m_previousShape = doc.getBody(m_bodyId);

        auto regions = sketch->buildRegions();
        if (regions.empty()) {
            std::fprintf(stderr,
                         "[ProjectSketch] sketch has no closed regions\n");
            return false;
        }

        // Projection direction: sketch-plane normal, oriented toward the
        // target face.
        const gp_Pln& pln = sketch->getPlane();
        gp_Pnt org = pln.Location();
        gp_Dir dir = pln.Axis().Direction();
        GProp_GProps fg;
        BRepGProp::SurfaceProperties(m_targetFace, fg);
        gp_Vec toFace(org, fg.CentreOfMass());
        if (toFace.Dot(gp_Vec(dir)) < 0.0) dir.Reverse();

        // One stamp tool per region: project outer + hole wires, rebuild as
        // a face on the target surface, sweep along the projection direction
        // with a small overlap past the surface so the boolean never sees a
        // tangent contact.
        const double eps = 0.05;
        const gp_Vec dv(dir);
        TopTools_ListOfShape tools;
        double toolVolume = 0.0;
        int skipped = 0;
        for (size_t ri = 0; ri < regions.size(); ++ri) {
            if (!m_regionFilter.empty()) {
                bool wanted = false;
                for (int idx : m_regionFilter)
                    if (idx == static_cast<int>(ri)) { wanted = true; break; }
                if (!wanted) continue;
            }
            const auto& reg = regions[ri];
            std::fprintf(stderr,
                "[ProjectSketch] region %zu: projecting outer wire onto face.\n",
                ri);
            TopoDS_Wire outer =
                projectNearest(reg.outerWire, m_targetFace, dir, org);
            if (outer.IsNull()) { skipped++; continue; }
            std::vector<TopoDS_Wire> holes;
            bool holesOk = true;
            for (const auto& hw : reg.holeWires) {
                TopoDS_Wire ph = projectNearest(hw, m_targetFace, dir, org);
                if (ph.IsNull()) { holesOk = false; break; }
                holes.push_back(ph);
            }
            if (!holesOk) { skipped++; continue; }
            TopoDS_Shape sub = faceOnSurface(m_targetFace, outer, holes);
            if (sub.IsNull()) { skipped++; continue; }

            gp_Trsf shift;
            TopoDS_Shape tool;
            if (m_mode == Mode::Engrave) {
                // start a hair OUTSIDE the surface, dig in along dir
                shift.SetTranslation(dv * (-eps));
                TopoDS_Shape start =
                    BRepBuilderAPI_Transform(sub, shift).Shape();
                tool = BRepPrimAPI_MakePrism(start, dv * (m_depth + eps))
                           .Shape();
            } else {
                // start a hair INSIDE the body, raise out against dir
                shift.SetTranslation(dv * eps);
                TopoDS_Shape start =
                    BRepBuilderAPI_Transform(sub, shift).Shape();
                tool = BRepPrimAPI_MakePrism(start, dv * (-(m_depth + eps)))
                           .Shape();
            }
            if (tool.IsNull()) { skipped++; continue; }
            toolVolume += std::abs(shapeVolume(tool));
            tools.Append(tool);
        }
        if (tools.IsEmpty()) {
            std::fprintf(stderr,
                         "[ProjectSketch] no region projected cleanly onto "
                         "the face — the sketch must land fully inside it\n");
            return false;
        }
        if (skipped > 0) {
            std::fprintf(stderr,
                         "[ProjectSketch] %d region(s) skipped (projected "
                         "off the face)\n", skipped);
        }

        TopTools_ListOfShape args;
        args.Append(m_previousShape);
        TopoDS_Shape result;
        if (m_mode == Mode::Engrave) {
            BRepAlgoAPI_Cut cut;
            cut.SetArguments(args);
            cut.SetTools(tools);
            cut.SetRunParallel(Standard_True);
            cut.Build();
            if (!cut.IsDone()) return false;
            result = cut.Shape();
        } else {
            BRepAlgoAPI_Fuse fuse;
            fuse.SetArguments(args);
            fuse.SetTools(tools);
            fuse.SetRunParallel(Standard_True);
            fuse.Build();
            if (!fuse.IsDone()) return false;
            result = fuse.Shape();
        }
        if (result.IsNull() || !BRepCheck_Analyzer(result).IsValid())
            return false;

        // The volume must move the right way, by no more than the tools.
        double v0 = shapeVolume(m_previousShape);
        double v1 = shapeVolume(result);
        double delta = (m_mode == Mode::Engrave) ? (v0 - v1) : (v1 - v0);
        if (delta < -1e-6 || delta > toolVolume * 1.5 + 1e-6) {
            std::fprintf(stderr,
                         "[ProjectSketch] boolean produced a suspicious "
                         "volume change (%.3f of %.3f tool) — refusing\n",
                         delta, toolVolume);
            return false;
        }

        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        std::fprintf(stderr, "[ProjectSketch] execute threw\n");
        return false;
    }
}

bool ProjectSketchOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) { return false; }
}

std::string ProjectSketchOp::description() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s sketch %.2f mm",
                  m_mode == Mode::Engrave ? "Engrave" : "Emboss", m_depth);
    return buf;
}

void ProjectSketchOp::renderProperties() {
    ImGui::Text("Projection");
    ImGui::Separator();
    ImGui::Text("Mode: %s",
                m_mode == Mode::Engrave ? "Engrave" : "Emboss");
    ImGui::InputDouble("Depth (mm)", &m_depth, 0.1, 1.0, "%.2f");
    ImGui::Text("Sketch ID: %d", m_sketchId);
    ImGui::Text("Body ID: %d", m_bodyId);
}

std::string ProjectSketchOp::serializeParams() const {
    std::string blob;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "body=%d;sketch=%d;depth=%.6f;mode=%d",
                  m_bodyId, m_sketchId, m_depth, static_cast<int>(m_mode));
    blob += buf;
    if (!m_regionFilter.empty()) {
        blob += ";regions=";
        for (size_t i = 0; i < m_regionFilter.size(); ++i) {
            if (i) blob += ',';
            blob += std::to_string(m_regionFilter[i]);
        }
    }
    if (!m_previousShape.IsNull() && !m_targetFace.IsNull()) {
        std::string idx = SubShapeIndex::serialize(
            m_previousShape, {m_targetFace}, TopAbs_FACE);
        if (!idx.empty()) blob += ";face=" + idx;
    }
    return blob;
}

bool ProjectSketchOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "body")   { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "sketch") { m_sketchId = std::atoi(val.c_str()); any = true; }
        else if (key == "depth")  { m_depth = std::atof(val.c_str()); any = true; }
        else if (key == "mode") {
            m_mode = std::atoi(val.c_str()) == 1 ? Mode::Emboss
                                                 : Mode::Engrave;
            any = true;
        }
        else if (key == "regions") {
            m_regionFilter.clear();
            size_t p = 0;
            while (p < val.size()) {
                size_t c = val.find(',', p);
                if (c == std::string::npos) c = val.size();
                m_regionFilter.push_back(
                    std::atoi(val.substr(p, c - p).c_str()));
                p = c + 1;
            }
            any = true;
        }
        else if (key == "face") {
            m_faceIndices = SubShapeIndex::parse(val);
            any = true;
        }
        pos = end + 1;
    }
    return any;
}

bool ProjectSketchOp::rehydrateFromReload(const ReloadState& state,
                                          Document& /*doc*/) {
    if (m_bodyId < 0 || m_faceIndices.empty()) return false;

    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_faceIndices,
                                   TopAbs_FACE, resolved) ||
        resolved.empty()) {
        return false;
    }
    m_targetFace = TopoDS::Face(resolved.front());
    return true;
}

OperationDiff ProjectSketchOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}
