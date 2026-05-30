#include "FilletOp.h"
#include <BRepFilletAPI_MakeFillet.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepGProp_Face.hxx>
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

        // Create fillet on the body shape
        BRepFilletAPI_MakeFillet fillet(m_previousShape);

        for (const auto& edge : m_edges) {
            fillet.Add(m_radius, edge);
        }

        fillet.Build();
        if (!fillet.IsDone()) {
            return false;
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

        // Update the body with the filleted shape
        doc.updateBody(m_bodyId, fillet.Shape());
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
    // Single-line key=value blob. Only scalar inputs for now — the edge set is
    // tied to OCCT TopoDS pointers that don't survive save/load, so re-edit
    // after reload is gated on a future face-/edge-ID stability pass.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "radius=%.6f", m_radius);
    return buf;
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
        if (key == "radius") { m_radius = std::atof(val.c_str()); any = true; }
        pos = end + 1;
    }
    return any;
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
