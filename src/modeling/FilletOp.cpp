#include "FilletOp.h"
#include "SubShapeIndex.h"
#include <cstdio>
#include <cstdlib>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <imgui.h>

namespace {
// Representative point on a face (midpoint of its UV bounds). Stable for the
// same face geometry, so it survives re-tessellation between picks.
bool faceCenter(const TopoDS_Face& face, gp_Pnt& out) {
    try {
        BRepGProp_Face gp(face);
        Standard_Real u0, u1, v0, v1;
        gp.Bounds(u0, u1, v0, v1);
        gp_Vec n;
        gp.Normal((u0 + u1) * 0.5, (v0 + v1) * 0.5, out, n);
        return true;
    } catch (...) { return false; }
}
} // namespace

FilletOp::FilletOp() = default;

void FilletOp::setBody(int bodyId) {
    m_bodyId = bodyId;
}

void FilletOp::setEdges(const std::vector<TopoDS_Edge>& edges) {
    m_edges = edges;
}

void FilletOp::setRadius(double radius) {
    m_radius = radius;
}

bool FilletOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_edges.empty() || m_radius <= 0.0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        // If an upstream edit regenerated the body, our stored edges have
        // stale TShapes — re-bind them to their successors by carrier
        // geometry so editing (say) a neighbouring fillet's radius doesn't
        // kill this op. Fails (loudly, via editStep) only when an edge was
        // genuinely consumed by the upstream change.
        if (!SubShapeIndex::rebindEdges(m_previousShape, m_edges)) {
            std::fprintf(stderr,
                "[Fillet] rebindEdges failed (R=%.2f, %zu edges) — "
                "selected edge isn't in the current body's edge map.\n",
                m_radius, m_edges.size());
            return false;
        }

        // Create fillet on the body shape
        BRepFilletAPI_MakeFillet fillet(m_previousShape);

        for (const auto& edge : m_edges) {
            fillet.Add(m_radius, edge);
        }

        fillet.Build();
        if (!fillet.IsDone()) {
            std::fprintf(stderr,
                "[Fillet] BRepFilletAPI.IsDone() returned false (R=%.2f) "
                "— OCCT refused to build the fillet at this radius.\n",
                m_radius);
            return false;
        }

        TopoDS_Shape candidate = fillet.Shape();

        // OCCT's fillet API is permissive — IsDone() returns true even when
        // the radius exceeds what the geometry can support, and the result
        // is then a self-intersecting / overlapping mess instead of a clean
        // refusal. Two narrow sanity checks reject those without flagging
        // legitimate concave fillets (which ADD material and so make the
        // upper-bound volume check we used to have backwards):
        //   • Bounding box: a fillet should never GROW the body's bbox by
        //     more than a hair. Garbled-cube case (radius > half-extent)
        //     produces inverted shells whose bbox blows out — that's the
        //     signal we catch.
        //   • Volume: must be strictly > 0. Truly degenerate output (zero
        //     or negative volume) is the other failure mode.
        // (Steve: a coffee-cup rim could only fillet to 1.5 mm on the
        //  inside, and not at all on the outside — the old "volume must
        //  not exceed input × 1.01" rule rejected the inside concave
        //  fillets even when geometrically fine.)
        {
            // AddOptimal walks the actual geometry rather than the looser
            // tolerance-padded extents the plain Add uses. Shelled bodies
            // tend to land in OCCT with face seams at ~1e-3 tolerance,
            // which inflated the result bbox by ~8 mm on a 100 mm cup and
            // tripped the growth gate even on 0.1 mm fillets.
            Bnd_Box bbIn, bbOut;
            BRepBndLib::AddOptimal(m_previousShape, bbIn);
            BRepBndLib::AddOptimal(candidate,       bbOut);
            if (!bbIn.IsVoid() && !bbOut.IsVoid()) {
                Standard_Real ix0, iy0, iz0, ix1, iy1, iz1;
                Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
                bbIn .Get(ix0, iy0, iz0, ix1, iy1, iz1);
                bbOut.Get(ox0, oy0, oz0, ox1, oy1, oz1);
                const double slop = 1.01; // 1% tolerance for fp noise
                if (ox1 - ox0 > (ix1 - ix0) * slop ||
                    oy1 - oy0 > (iy1 - iy0) * slop ||
                    oz1 - oz0 > (iz1 - iz0) * slop) {
                    std::fprintf(stderr,
                        "[Fillet] bbox grew past slop (R=%.2f): "
                        "%.2fx%.2fx%.2f -> %.2fx%.2fx%.2f mm.\n",
                        m_radius,
                        ix1 - ix0, iy1 - iy0, iz1 - iz0,
                        ox1 - ox0, oy1 - oy0, oz1 - oz0);
                    return false;
                }
            }

            GProp_GProps gpOut;
            BRepGProp::VolumeProperties(candidate, gpOut);
            if (gpOut.Mass() < 1e-6) {
                std::fprintf(stderr,
                    "[Fillet] result volume ~= 0 (R=%.2f mm).\n",
                    m_radius);
                return false;
            }
        }

        // Record the blend faces generated from each input edge so a later face
        // click can be traced back to this fillet for re-editing.
        m_generatedFaces.clear();
        for (const auto& edge : m_edges) {
            try {
                const TopTools_ListOfShape& gen = fillet.Generated(edge);
                // Range-based loop instead of TopTools_ListIteratorOfListOfShape,
                // whose header was removed in OCCT 8.0 (still works on 7.x).
                for (const TopoDS_Shape& s : gen) {
                    if (s.ShapeType() == TopAbs_FACE)
                        m_generatedFaces.push_back(s);
                }
            } catch (...) {}
        }

        // Update the body with the filleted shape (kept on the op too, so
        // serializeParams can index the generated faces against the result).
        m_resultShape = candidate;
        doc.updateBody(m_bodyId, m_resultShape);
        return true;
    } catch (...) {
        return false;
    }
}

bool FilletOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) {
        return false;
    }

    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

std::string FilletOp::description() const {
    return "Fillet R" + std::to_string(m_radius) + " on " +
           std::to_string(m_edges.size()) + " edge(s)";
}

void FilletOp::renderProperties() {
    ImGui::Text("Fillet");
    ImGui::Separator();

    ImGui::InputDouble("Radius", &m_radius, 0.1, 1.0, "%.3f");

    ImGui::Text("Edges: %d selected", static_cast<int>(m_edges.size()));
    ImGui::Text("Body ID: %d", m_bodyId);
}

OperationDiff FilletOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string FilletOp::serializeParams() const {
    // The edge set is persisted as ordinal indices into the INPUT shape's
    // canonical sub-shape map (see SubShapeIndex.h) — BREP round-trips the
    // shape byte-identically, so the indices resolve on reload. Generated
    // blend faces are indexed against the RESULT shape for click-to-edit.
    std::string blob;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "body=%d;radius=%.6f", m_bodyId, m_radius);
    blob += buf;
    if (!m_previousShape.IsNull() && !m_edges.empty()) {
        std::vector<TopoDS_Shape> edges(m_edges.begin(), m_edges.end());
        std::string idx = SubShapeIndex::serialize(m_previousShape, edges,
                                                   TopAbs_EDGE);
        if (!idx.empty()) blob += ";edges=" + idx;
    }
    if (!m_resultShape.IsNull() && !m_generatedFaces.empty()) {
        std::string idx = SubShapeIndex::serialize(m_resultShape,
                                                   m_generatedFaces,
                                                   TopAbs_FACE);
        if (!idx.empty()) blob += ";gen=" + idx;
    }
    return blob;
}

bool FilletOp::deserializeParams(const std::string& blob) {
    // Tolerant key=value parser. Unknown keys are ignored; missing keys keep
    // current defaults. Returns true if at least one key was understood.
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "radius") { m_radius = std::atof(val.c_str()); any = true; }
        else if (key == "body")   { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "edges")  { m_edgeIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "gen")    { m_genFaceIndices = SubShapeIndex::parse(val); any = true; }
        pos = end + 1;
    }
    return any;
}

bool FilletOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0 || m_edgeIndices.empty()) return false;

    // Bind the before/after shapes for our body from the saved step.
    m_previousShape.Nullify();
    m_resultShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    for (const auto& [id, shp] : state.modifiedAfter)
        if (id == m_bodyId) { m_resultShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    // Re-resolve the filleted edges against the input shape. ALL must resolve
    // — a partial set would fillet the wrong geometry, so decline to ReplayOp.
    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_edgeIndices,
                                   TopAbs_EDGE, resolved)) {
        return false;
    }
    m_edges.clear();
    for (const auto& s : resolved) m_edges.push_back(TopoDS::Edge(s));

    // Blend faces (click-to-edit mapping) resolve against the result —
    // best-effort: their absence only disables face-click mapping.
    m_generatedFaces.clear();
    if (!m_resultShape.IsNull() && !m_genFaceIndices.empty()) {
        std::vector<TopoDS_Shape> gen;
        if (SubShapeIndex::resolveAll(m_resultShape, m_genFaceIndices,
                                      TopAbs_FACE, gen)) {
            m_generatedFaces = std::move(gen);
        }
    }
    return true;
}

bool FilletOp::ownsFace(const TopoDS_Shape& face) const {
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE) return false;
    for (const auto& f : m_generatedFaces) {
        if (f.IsSame(face)) return true;
    }
    // Geometric fallback for when the body's faces were rebuilt (e.g. after a
    // replay) and are no longer IsSame to the stored ones.
    gp_Pnt q;
    if (!faceCenter(TopoDS::Face(face), q)) return false;
    for (const auto& f : m_generatedFaces) {
        gp_Pnt p;
        if (faceCenter(TopoDS::Face(f), p) && p.Distance(q) < 1e-4) return true;
    }
    return false;
}
