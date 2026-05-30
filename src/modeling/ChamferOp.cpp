#include "ChamferOp.h"
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <BRepGProp_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <imgui.h>

namespace {
// Representative point on a face (midpoint of its UV bounds).
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

ChamferOp::ChamferOp() = default;

void ChamferOp::setBody(int bodyId) {
    m_bodyId = bodyId;
}

void ChamferOp::setEdges(const std::vector<TopoDS_Edge>& edges) {
    m_edges = edges;
}

void ChamferOp::setDistance(double distance) {
    m_distance = distance;
}

bool ChamferOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_edges.empty() || m_distance <= 0.0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        // Build an edge-face map so we can find a face adjacent to each edge
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
        TopExp::MapShapesAndAncestors(m_previousShape, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);

        // Create chamfer on the body shape
        BRepFilletAPI_MakeChamfer chamfer(m_previousShape);

        for (const auto& edge : m_edges) {
            // Find a face adjacent to this edge
            if (edgeFaceMap.Contains(edge)) {
                const TopTools_ListOfShape& faces = edgeFaceMap.FindFromKey(edge);
                if (!faces.IsEmpty()) {
                    const TopoDS_Face& face = TopoDS::Face(faces.First());
                    chamfer.Add(m_distance, m_distance, edge, face);
                }
            }
        }

        chamfer.Build();
        if (!chamfer.IsDone()) {
            return false;
        }

        // Record the chamfer faces generated from each input edge so a later
        // face click can be traced back to this op for re-editing.
        m_generatedFaces.clear();
        for (const auto& edge : m_edges) {
            try {
                const TopTools_ListOfShape& gen = chamfer.Generated(edge);
                // Range-based loop instead of TopTools_ListIteratorOfListOfShape,
                // whose header was removed in OCCT 8.0 (still works on 7.x).
                for (const TopoDS_Shape& s : gen) {
                    if (s.ShapeType() == TopAbs_FACE)
                        m_generatedFaces.push_back(s);
                }
            } catch (...) {}
        }

        // Update the body with the chamfered shape
        doc.updateBody(m_bodyId, chamfer.Shape());
        return true;
    } catch (...) {
        return false;
    }
}

bool ChamferOp::undo(Document& doc) {
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

std::string ChamferOp::description() const {
    return "Chamfer D" + std::to_string(m_distance) + " on " +
           std::to_string(m_edges.size()) + " edge(s)";
}

void ChamferOp::renderProperties() {
    ImGui::Text("Chamfer");
    ImGui::Separator();

    ImGui::InputDouble("Distance", &m_distance, 0.1, 1.0, "%.3f");

    ImGui::Text("Edges: %d selected", static_cast<int>(m_edges.size()));
    ImGui::Text("Body ID: %d", m_bodyId);
}

OperationDiff ChamferOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string ChamferOp::serializeParams() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "distance=%.6f", m_distance);
    return buf;
}

bool ChamferOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if (key == "distance") { m_distance = std::atof(val.c_str()); any = true; }
        pos = end + 1;
    }
    return any;
}

bool ChamferOp::ownsFace(const TopoDS_Shape& face) const {
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE) return false;
    for (const auto& f : m_generatedFaces) {
        if (f.IsSame(face)) return true;
    }
    gp_Pnt q;
    if (!faceCenter(TopoDS::Face(face), q)) return false;
    for (const auto& f : m_generatedFaces) {
        gp_Pnt p;
        if (faceCenter(TopoDS::Face(f), p) && p.Distance(q) < 1e-4) return true;
    }
    return false;
}
