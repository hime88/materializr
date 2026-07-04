#include "ChamferOp.h"
#include "SubShapeIndex.h"
#include "EdgeAnchor.h"
#include <cstdio>
#include <cstdlib>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
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

TopoDS_Face ChamferOp::sharedReferenceFace(const TopoDS_Shape& body,
                                           const std::vector<TopoDS_Edge>& edges) {
    if (edges.empty() || body.IsNull()) return TopoDS_Face();
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);
    if (!edgeFaceMap.Contains(edges.front())) return TopoDS_Face();

    // Candidates = faces adjacent to the first edge; intersect down with each
    // subsequent edge's adjacent faces. Whatever survives borders every edge.
    std::vector<TopoDS_Shape> cands;
    for (const TopoDS_Shape& f : edgeFaceMap.FindFromKey(edges.front()))
        cands.push_back(f);
    for (size_t i = 1; i < edges.size(); ++i) {
        if (!edgeFaceMap.Contains(edges[i])) return TopoDS_Face();
        const TopTools_ListOfShape& fs = edgeFaceMap.FindFromKey(edges[i]);
        std::vector<TopoDS_Shape> keep;
        for (const auto& c : cands)
            for (const TopoDS_Shape& f : fs)
                if (f.IsSame(c)) { keep.push_back(c); break; }
        cands.swap(keep);
        if (cands.empty()) return TopoDS_Face(); // no common face for this set
    }
    return TopoDS::Face(cands.front());
}

// See FilletOp::anchorSketches — anchoring consults every sketch in the doc,
// preferring the cascade override (the edited sketch's final state) over the
// live sketch, which is rolled back through its snapshots mid-replay.
static std::vector<EdgeAnchor::SketchRef> anchorSketches(
        Document& doc, std::vector<std::shared_ptr<materializr::Sketch>>& keep) {
    std::vector<EdgeAnchor::SketchRef> refs;
    for (int sid : doc.getAllSketchIds()) {
        if (auto ov = doc.cascadeSketchOverride(sid)) {
            keep.push_back(ov);
            refs.push_back({ sid, ov.get() });
        } else if (auto sk = doc.getSketch(sid)) {
            keep.push_back(sk);
            refs.push_back({ sid, sk.get() });
        }
    }
    return refs;
}

void ChamferOp::computeAnchors(Document& doc) {
    m_edgeAnchors.clear();
    std::vector<std::shared_ptr<materializr::Sketch>> keep;
    m_edgeAnchors = EdgeAnchor::compute(m_edges, anchorSketches(doc, keep));
}

bool ChamferOp::resolveAnchors(Document& doc, const TopoDS_Shape& base) {
    if (m_edgeAnchors.size() != m_edges.size()) return false;
    std::vector<TopoDS_Edge> resolved;
    std::vector<std::shared_ptr<materializr::Sketch>> keep;
    if (!EdgeAnchor::resolve(m_edgeAnchors, anchorSketches(doc, keep), base, resolved))
        return false;
    m_edges = std::move(resolved);
    return true;
}

bool ChamferOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_edges.empty() || m_distance <= 0.0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        // Re-bind stored edges to the (possibly regenerated) body before
        // chamfering — see FilletOp::execute. Without this, editing an
        // upstream fillet's radius left every chamfer edge stale: the
        // edge-face map silently skipped them all and the chamfer vanished.
        if (!SubShapeIndex::rebindEdges(m_previousShape, m_edges)) {
            // Ordinal/carrier match failed (a sketch dimension edit moved the
            // edges) — re-find them by their sketch feature. See EdgeAnchor.
            if (!resolveAnchors(doc, m_previousShape)) return false;
        }
        if (m_edgeAnchors.empty()) computeAnchors(doc);

        // Build an edge-face map so we can find a face adjacent to each edge
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
        TopExp::MapShapesAndAncestors(m_previousShape, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);

        // Create chamfer on the body shape
        BRepFilletAPI_MakeChamfer chamfer(m_previousShape);

        // For an asymmetric chamfer across multiple edges, distance 1 must be
        // measured along ONE consistent face (the loop's shared face) for every
        // edge, or A/B would flip from edge to edge. sharedRef is that face when
        // it exists (always, for a single edge); null = no common face, in which
        // case we fall back to each edge's first face (symmetric is unaffected).
        TopoDS_Face sharedRef;
        if (m_distance2 > 0.0) sharedRef = sharedReferenceFace(m_previousShape, m_edges);

        for (const auto& edge : m_edges) {
            // Find a face adjacent to this edge
            if (edgeFaceMap.Contains(edge)) {
                const TopTools_ListOfShape& faces = edgeFaceMap.FindFromKey(edge);
                if (!faces.IsEmpty()) {
                    TopoDS_Face face = sharedRef.IsNull()
                                           ? TopoDS::Face(faces.First())
                                           : sharedRef;
                    // d1 is measured along `face`; d2 along the other adjacent
                    // face. Symmetric when m_distance2 <= 0.
                    double d2 = (m_distance2 > 0.0) ? m_distance2 : m_distance;
                    chamfer.Add(m_distance, d2, edge, face);
                }
            }
        }

        chamfer.Build();
        if (!chamfer.IsDone()) {
            return false;
        }

        TopoDS_Shape candidate = chamfer.Shape();
        if (candidate.IsNull()) {
            std::fprintf(stderr, "[Chamfer] result shape is null (d=%.2f).\n",
                         m_distance);
            return false;
        }

        // IsDone() is necessary but NOT sufficient — see FilletOp::execute.
        // Chamfering many adjacent edges at once can yield a result OCCT
        // reports as done but is topologically INVALID (self-intersections,
        // dropped faces): the "faces disappear" bug. BRepCheck_Analyzer is the
        // authoritative validity test; reject anything it flags so a corrupt
        // body never reaches the document/history.
        if (!BRepCheck_Analyzer(candidate).IsValid()) {
            std::fprintf(stderr,
                "[Chamfer] result failed BRepCheck_Analyzer (d=%.2f, %zu edges) "
                "— invalid topology, refusing to commit.\n",
                m_distance, m_edges.size());
            return false;
        }

        // Same narrow defence as FilletOp::execute: bbox-must-not-grow
        // and volume must be strictly > 0. The old volume-cap (output ≤
        // input × 1.01) wrongly rejected concave chamfers which add a
        // sliver of material in the corner.
        {
            // AddOptimal walks the actual geometry rather than tolerance-
            // padded extents — see FilletOp::execute for the full story.
            Bnd_Box bbIn, bbOut;
            BRepBndLib::AddOptimal(m_previousShape, bbIn);
            BRepBndLib::AddOptimal(candidate,       bbOut);
            if (!bbIn.IsVoid() && !bbOut.IsVoid()) {
                Standard_Real ix0, iy0, iz0, ix1, iy1, iz1;
                Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
                bbIn .Get(ix0, iy0, iz0, ix1, iy1, iz1);
                bbOut.Get(ox0, oy0, oz0, ox1, oy1, oz1);
                const double slop = 1.01;
                if (ox1 - ox0 > (ix1 - ix0) * slop ||
                    oy1 - oy0 > (iy1 - iy0) * slop ||
                    oz1 - oz0 > (iz1 - iz0) * slop) {
                    return false;
                }
            }

            GProp_GProps gpOut;
            BRepGProp::VolumeProperties(candidate, gpOut);
            if (gpOut.Mass() < 1e-6) return false;
        }

        // Record the chamfer faces generated from each input edge so a later
        // face click can be traced back to this op for re-editing.
        // Publish the generation map so "gen" can name a bevel face by its
        // generating edge (general-kernel path for op-produced faces).
        m_ledger.capture(chamfer, m_previousShape, TopAbs_EDGE);

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

        // Update the body with the chamfered shape (kept on the op too, so
        // serializeParams can index the generated faces against the result).
        m_resultShape = candidate;
        doc.updateBody(m_bodyId, m_resultShape);
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
    if (m_distance2 > 0.0)
        return "Chamfer D" + std::to_string(m_distance) + "/" +
               std::to_string(m_distance2) + " on " +
               std::to_string(m_edges.size()) + " edge(s)";
    return "Chamfer D" + std::to_string(m_distance) + " on " +
           std::to_string(m_edges.size()) + " edge(s)";
}

void ChamferOp::renderProperties() {
    ImGui::Text("Chamfer");
    ImGui::Separator();

    ImGui::InputDouble("Distance", &m_distance, 0.1, 1.0, "%.3f");
    bool asym = (m_distance2 > 0.0);
    if (ImGui::Checkbox("Two distances", &asym))
        m_distance2 = asym ? m_distance : -1.0;
    if (m_distance2 > 0.0)
        ImGui::InputDouble("Distance 2", &m_distance2, 0.1, 1.0, "%.3f");

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
    // Same persistent sub-shape scheme as FilletOp: edges indexed into the
    // INPUT shape, generated faces into the RESULT (see SubShapeIndex.h).
    std::string blob;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "body=%d;distance=%.6f", m_bodyId, m_distance);
    blob += buf;
    if (m_distance2 > 0.0) {
        std::snprintf(buf, sizeof(buf), ";distance2=%.6f", m_distance2);
        blob += buf;
    }
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
    std::string anc = EdgeAnchor::serialize(m_edgeAnchors);
    if (!anc.empty()) blob += ";anchor=" + anc;
    return blob;
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
        if      (key == "distance") { m_distance = std::atof(val.c_str()); any = true; }
        else if (key == "distance2"){ m_distance2 = std::atof(val.c_str()); any = true; }
        else if (key == "body")     { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "edges")    { m_edgeIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "gen")      { m_genFaceIndices = SubShapeIndex::parse(val); any = true; }
        else if (key == "anchor")   { EdgeAnchor::parse(val, m_edgeAnchors); any = true; }
        pos = end + 1;
    }
    return any;
}

bool ChamferOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0 || m_edgeIndices.empty()) return false;

    m_previousShape.Nullify();
    m_resultShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    for (const auto& [id, shp] : state.modifiedAfter)
        if (id == m_bodyId) { m_resultShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_edgeIndices,
                                   TopAbs_EDGE, resolved)) {
        return false;
    }
    m_edges.clear();
    for (const auto& s : resolved) m_edges.push_back(TopoDS::Edge(s));

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

void ChamferOp::refreshGeneratedFaces(const TopoDS_Shape& currentBody) {
    if (m_genFaceIndices.empty() || currentBody.IsNull()) return;
    std::vector<TopoDS_Shape> gen;
    if (SubShapeIndex::resolveAll(currentBody, m_genFaceIndices, TopAbs_FACE, gen))
        m_generatedFaces = std::move(gen);
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
