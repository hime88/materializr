#include "gl_common.h"

#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "app/Application.h"
#include "viewport/Viewport.h"
#include "viewport/Camera.h"
#include "viewport/ShapeRenderer.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/SelectionManager.h"

#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "modeling/SketchTool.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/MoveFaceOp.h"
#include "modeling/MoveHoleOp.h"
#include "modeling/FilletOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/ShellOp.h"
#include "modeling/TaperOp.h"
#include "modeling/ScaleFaceOp.h"
#include "modeling/PrimitiveOp.h"
#include "app/UserAxes.h"
#include "modeling/ResizeCylindricalOp.h"
#include "modeling/ThreadOp.h"
#include <BRepMesh_IncrementalMesh.hxx>
#include <future>
#include "modeling/PatternOp.h"
#include "modeling/LoftOp.h"
#include "modeling/ConstructionPlaneOp.h"
#include "modeling/ConstructionAxisOp.h"
#include <Geom_Plane.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_BezierSurface.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_BezierCurve.hxx>
#include <Geom_Surface.hxx>

#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <Geom_SurfaceOfRevolution.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <gp_Lin.hxx>
#include <gp_Vec.hxx>
#include <Geom_Curve.hxx>
#include <IntAna_QuadQuadGeo.hxx>
#include <Precision.hxx>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Implementations split out of Application.cpp — every interactive operation
// (Fillet / Chamfer / Edit Fillet-Chamfer, Edit Diameter, Shell, Extrude,
// Push/Pull) lives here. The shared pattern is begin → update (per-frame live
// preview) → commit (push to history) → cancel (rollback). The corresponding
// popup panels stay in Application_Dialogs.cpp; the on-viewport overlays
// (drag handles, arrows, measurement readouts) stay in Application_Viewport.cpp.
namespace materializr {

// ─── Fillet / Chamfer interactive ───────────────────────────────────────────

void Application::computeEdgeOpFaceDirs() {
    // The two faces meeting at the first edge each get one chamfer setback.
    // For the drag arrows we want a direction lying in each face, perpendicular
    // to the edge, pointing away from the edge into the face. Approximate "into
    // the face" as the perp-to-edge component of (face centroid − edge mid) —
    // robust without surface-normal evaluation, and good enough for a handle.
    m_edgeOpHasFaceDirs = false;
    m_edgeOpCanTwoDist = false;
    if (m_edgeOpEdges.empty() || m_edgeOpPreviousShape.IsNull()) return;
    try {
        std::vector<TopoDS_Edge> typedEdges;
        for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));

        // Distance-1 reference face must match ChamferOp::execute. For a single
        // edge that's one of its two faces; for multiple edges it's the face
        // they ALL share (a planar edge loop). No shared face → no two-distance.
        TopoDS_Face faceA =
            ChamferOp::sharedReferenceFace(m_edgeOpPreviousShape, typedEdges);
        if (faceA.IsNull()) return;

        // Face B = the other face adjacent to the first edge (the one that
        // isn't the shared/A face).
        TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
        TopExp::MapShapesAndAncestors(m_edgeOpPreviousShape, TopAbs_EDGE,
                                      TopAbs_FACE, edgeFaceMap);
        TopoDS_Edge e0 = typedEdges.front();
        if (!edgeFaceMap.Contains(e0)) return;
        TopoDS_Shape faceB;
        for (const TopoDS_Shape& f : edgeFaceMap.FindFromKey(e0))
            if (!f.IsSame(faceA)) { faceB = f; break; }
        if (faceB.IsNull()) return;

        auto inFaceDir = [&](const TopoDS_Shape& f) -> glm::vec3 {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f, props);
            gp_Pnt cm = props.CentreOfMass();
            glm::vec3 c(cm.X(), cm.Y(), cm.Z());
            glm::vec3 d = c - m_edgeOpMid;
            d -= glm::dot(d, m_edgeOpDir) * m_edgeOpDir; // perpendicular to edge
            return (glm::length(d) > 1e-6f) ? glm::normalize(d) : m_edgeOpOutDir;
        };
        m_edgeOpFaceDirA = inFaceDir(faceA);
        m_edgeOpFaceDirB = inFaceDir(faceB);
        m_edgeOpHasFaceDirs = true;
        m_edgeOpCanTwoDist = true;
    } catch (...) {
        m_edgeOpHasFaceDirs = false;
        m_edgeOpCanTwoDist = false;
    }
}

void Application::beginInteractiveEdgeOp(EdgeOpType type) {
    const auto& sel = m_selection->getSelection();
    int bodyId = -1;
    std::vector<TopoDS_Shape> edges;
    for (const auto& entry : sel) {
        if (entry.type == SelectionType::Edge && !entry.shape.IsNull()) {
            if (bodyId < 0) bodyId = entry.bodyId;
            if (entry.bodyId == bodyId) edges.push_back(entry.shape);
        }
    }
    if (bodyId < 0 || edges.empty()) return;

    // Drop any other in-progress preview (incl. a previous fillet/chamfer)
    // BEFORE snapshotting — otherwise the new op captures the previewed
    // body as its pre-state and Cancel restores the preview, not the
    // original. Matches what beginIop already does for InteractiveOp
    // controllers.
    cancelAllInteractivePreviews();

    m_edgeOpType = type;
    m_edgeOpActive = true;
    m_edgeOpBodyId = bodyId;
    m_edgeOpEdges = edges;
    m_edgeOpValue = 0.0f; // start at no change; drag the arrow outward or type a value
    std::snprintf(m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf), "%.1f", m_edgeOpValue);
    m_edgeOpInputFocus = true;

    // Save original shape for live preview
    try {
        m_edgeOpPreviousShape = m_document->getBody(bodyId);
    } catch (...) { m_edgeOpActive = false; return; }

    // First edge's midpoint + tangent, for the drag handle / measurement.
    m_edgeOpHasHandle = false;
    try {
        BRepAdaptor_Curve curve(TopoDS::Edge(edges.front()));
        double t = (curve.FirstParameter() + curve.LastParameter()) * 0.5;
        gp_Pnt p; gp_Vec tan;
        curve.D1(t, p, tan);
        m_edgeOpMid = glm::vec3(p.X(), p.Y(), p.Z());
        if (tan.Magnitude() > 1e-9) {
            m_edgeOpDir = glm::normalize(glm::vec3(tan.X(), tan.Y(), tan.Z()));
            // Outward handle direction = the average of the two adjacent faces'
            // OUTWARD normals at the edge, made perpendicular to the edge. This
            // points the arrow the way the fillet actually grows for BOTH convex
            // (outer) edges AND concave inner corners — e.g. the inside corners
            // of a thin-wall hollow box, where the fillet bulges into the cavity.
            // The old "bbox centre → edge" heuristic was inverted on concave
            // edges (arrow faced out toward the wall). Falls back to it if the
            // adjacent faces can't be read.
            glm::vec3 out(0.0f);
            try {
                TopTools_IndexedDataMapOfShapeListOfShape efMap;
                TopExp::MapShapesAndAncestors(m_edgeOpPreviousShape, TopAbs_EDGE,
                                              TopAbs_FACE, efMap);
                const TopoDS_Edge& e0 = TopoDS::Edge(edges.front());
                if (efMap.Contains(e0)) {
                    const TopTools_ListOfShape& fl = efMap.FindFromKey(e0);
                    for (const TopoDS_Shape& fs : fl) {
                        BRepGProp_Face gf(TopoDS::Face(fs));
                        Standard_Real u0,u1,v0,v1; gf.Bounds(u0,u1,v0,v1);
                        gp_Pnt fp; gp_Vec fn;
                        gf.Normal(0.5*(u0+u1), 0.5*(v0+v1), fp, fn); // outward (orientation-corrected)
                        if (fn.Magnitude() > 1e-9) {
                            fn.Normalize();
                            out += glm::vec3(fn.X(), fn.Y(), fn.Z());
                        }
                    }
                }
            } catch (...) {}
            if (glm::length(out) <= 1e-5f) {   // fallback: bbox centre → edge
                Bnd_Box bb; BRepBndLib::Add(m_edgeOpPreviousShape, bb);
                if (!bb.IsVoid()) {
                    double x1,y1,z1,x2,y2,z2; bb.Get(x1,y1,z1,x2,y2,z2);
                    glm::vec3 c((x1+x2)*0.5f, (y1+y2)*0.5f, (z1+z2)*0.5f);
                    out = m_edgeOpMid - c;
                }
            }
            out -= glm::dot(out, m_edgeOpDir) * m_edgeOpDir; // perpendicular to edge
            if (glm::length(out) > 1e-5f) m_edgeOpOutDir = glm::normalize(out);
            m_edgeOpHasHandle = true;
        }
    } catch (...) {}

    // Two-distance chamfer: default symmetric; the panel toggles it on. Only
    // offered for a chamfer on a single edge for now.
    m_edgeOpTwoDist = false;
    m_edgeOpValue2 = 0.0f;
    m_edgeOpGrab = -1;
    std::snprintf(m_edgeOpInputBuf2, sizeof(m_edgeOpInputBuf2), "%.1f", m_edgeOpValue2);
    if (type == EdgeOpType::Chamfer) computeEdgeOpFaceDirs();
    else m_edgeOpHasFaceDirs = false;

    m_edgeOpEditingIndex = -1; // creating new
    updateInteractiveEdgeOp();
}

void Application::beginInteractiveEdgeOpEdit(int historyIndex) {
    const Operation* opRaw = m_history->getStep(historyIndex);
    if (!opRaw) return;

    // Pull parameters from the existing op. dynamic_cast picks the right
    // sub-type; nothing else in history uses ownsFace + this typeId, so the
    // toolbar's filter is the only thing that should reach here.
    const FilletOp*  filletOp  = nullptr;
    const ChamferOp* chamferOp = nullptr;
    if (opRaw->typeId() == "fillet")
        filletOp = dynamic_cast<const FilletOp*>(opRaw);
    else if (opRaw->typeId() == "chamfer")
        chamferOp = dynamic_cast<const ChamferOp*>(opRaw);
    if (!filletOp && !chamferOp) return;

    m_edgeOpEdges.clear();
    if (filletOp) {
        m_edgeOpType  = EdgeOpType::Fillet;
        m_edgeOpBodyId = filletOp->getBodyId();
        for (const auto& e : filletOp->getEdges()) m_edgeOpEdges.push_back(e);
        m_edgeOpValue = static_cast<float>(filletOp->getRadius());
        m_edgeOpPreviousShape = filletOp->getPreviousShape();
    } else {
        m_edgeOpType  = EdgeOpType::Chamfer;
        m_edgeOpBodyId = chamferOp->getBodyId();
        for (const auto& e : chamferOp->getEdges()) m_edgeOpEdges.push_back(e);
        m_edgeOpValue = static_cast<float>(chamferOp->getDistance());
        m_edgeOpTwoDist = chamferOp->isAsymmetric();
        m_edgeOpValue2 = m_edgeOpTwoDist
            ? static_cast<float>(chamferOp->getDistance2()) : m_edgeOpValue;
        m_edgeOpPreviousShape = chamferOp->getPreviousShape();
    }
    if (m_edgeOpBodyId < 0 || m_edgeOpEdges.empty() ||
        m_edgeOpPreviousShape.IsNull()) return;

    m_edgeOpActive        = true;
    m_edgeOpEditingIndex  = historyIndex;
    m_edgeOpOrigValue     = m_edgeOpValue; // restored on cancel
    m_edgeOpOrigValue2    = m_edgeOpValue2;
    std::snprintf(m_edgeOpInputBuf, sizeof(m_edgeOpInputBuf), "%.1f", m_edgeOpValue);
    m_edgeOpInputFocus    = true;

    // Same handle-position logic as the create flow — first edge's midpoint
    // + outward perpendicular for the drag arrow.
    m_edgeOpHasHandle = false;
    try {
        BRepAdaptor_Curve curve(TopoDS::Edge(m_edgeOpEdges.front()));
        double t = (curve.FirstParameter() + curve.LastParameter()) * 0.5;
        gp_Pnt p; gp_Vec tan;
        curve.D1(t, p, tan);
        m_edgeOpMid = glm::vec3(p.X(), p.Y(), p.Z());
        if (tan.Magnitude() > 1e-9) {
            m_edgeOpDir = glm::normalize(glm::vec3(tan.X(), tan.Y(), tan.Z()));
            Bnd_Box bb; BRepBndLib::Add(m_edgeOpPreviousShape, bb);
            if (!bb.IsVoid()) {
                double x1,y1,z1,x2,y2,z2; bb.Get(x1,y1,z1,x2,y2,z2);
                glm::vec3 c((x1+x2)*0.5f, (y1+y2)*0.5f, (z1+z2)*0.5f);
                glm::vec3 out = m_edgeOpMid - c;
                out -= glm::dot(out, m_edgeOpDir) * m_edgeOpDir;
                if (glm::length(out) > 1e-5f) m_edgeOpOutDir = glm::normalize(out);
            }
            m_edgeOpHasHandle = true;
        }
    } catch (...) {}

    m_edgeOpGrab = -1;
    std::snprintf(m_edgeOpInputBuf2, sizeof(m_edgeOpInputBuf2), "%.1f", m_edgeOpValue2);
    if (m_edgeOpType == EdgeOpType::Chamfer) computeEdgeOpFaceDirs();
    else m_edgeOpHasFaceDirs = false;

    // Clear the face selection so the gizmo / overlay rendering doesn't fight
    // a stale "Face Operations" panel while editing.
    m_selection->clear();

    // Snapshot the picked body's geometry NOW — before the first editStep preview
    // runs below. commitInteractiveEdgeOp() uses these as the "before" baseline
    // to detect whether the edit actually changed anything.  If we let commit
    // measure volBefore after the preview, it would compare "new radius" vs
    // "new radius" (the preview already changed the body) and always report
    // "unchanged" even for a successful edit.
    m_edgeOpPrePickedVol  = 0.0;
    m_edgeOpPrePickedArea = 0.0;
    if (m_edgeOpPickedBodyId >= 0) {
        try {
            TopoDS_Shape s = m_document->getBody(m_edgeOpPickedBodyId);
            if (!s.IsNull()) {
                GProp_GProps gv, ga;
                BRepGProp::VolumeProperties(s, gv);
                BRepGProp::SurfaceProperties(s, ga);
                m_edgeOpPrePickedVol  = gv.Mass();
                m_edgeOpPrePickedArea = ga.Mass();
            }
        } catch (...) {}
    }

    updateInteractiveEdgeOp();
}

namespace {
// Write the previewed radius/distance into the history op being re-edited.
// v2 > 0 sets an asymmetric chamfer's second distance; v2 <= 0 = symmetric.
void setEdgeOpParam(const Operation* opRaw, bool isFillet, float v, float v2 = -1.0f) {
    if (!opRaw) return;
    if (isFillet) {
        if (auto* op = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(opRaw)))
            op->setRadius(static_cast<double>(v));
    } else {
        if (auto* op = const_cast<ChamferOp*>(dynamic_cast<const ChamferOp*>(opRaw))) {
            op->setDistance(static_cast<double>(v));
            op->setDistance2(static_cast<double>(v2));
        }
    }
}
} // namespace

bool Application::updateInteractiveEdgeOp() {
    if (!m_edgeOpActive || m_edgeOpBodyId < 0) return false;

    if (m_edgeOpEditingIndex >= 0) {
        // EDIT mode: preview through the real history replay so downstream
        // steps (a chamfer stacked on this fillet) stay visible during the
        // drag instead of flickering out. Geometrically impossible values
        // are rejected inside editStep (the op snaps back to its last good
        // parameters), so the preview can never strand the model.
        if (m_edgeOpValue < 0.01f) return false; // don't preview "remove" mid-drag
        // Partial remesh: re-tessellate only the bodies the replay changes, not
        // every visible body — see CREATE mode below.
        std::map<int, TopoDS_Shape> before;
        for (int id : m_document->getAllBodyIds()) before[id] = m_document->getBody(id);
        setEdgeOpParam(m_history->getStep(m_edgeOpEditingIndex),
                       m_edgeOpType == EdgeOpType::Fillet,
                       m_edgeOpValue,
                       m_edgeOpTwoDist ? m_edgeOpValue2 : -1.0f);
        m_history->editStep(m_edgeOpEditingIndex, *m_document);
        std::set<int> now;
        for (int id : m_document->getAllBodyIds()) {
            now.insert(id);
            auto it = before.find(id);
            if (it == before.end() || !it->second.IsEqual(m_document->getBody(id)))
                m_dirtyBodyIds.insert(id);
        }
        for (auto& [id, s] : before) if (!now.count(id)) m_dirtyBodyIds.insert(id);
        return true;
    }

    // CREATE mode: transient op against the snapshotted pre-state, touching
    // ONLY m_edgeOpBodyId. Mark just that body dirty (partial remesh) instead
    // of the global m_meshesDirty — a fillet on one body in a large scene was
    // re-tessellating EVERY visible body per preview frame, which is why the
    // op felt heavy with siblings shown and snappy with them hidden. Restore
    // first, so dragging back to ~0 shows no fillet/chamfer.
    m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    m_dirtyBodyIds.insert(m_edgeOpBodyId); // rebuildMeshes re-meshes its final state
    if (m_edgeOpValue < 0.01f) return false;

    try {
        if (m_edgeOpType == EdgeOpType::Fillet) {
            auto op = std::make_unique<FilletOp>();
            op->setBody(m_edgeOpBodyId);
            std::vector<TopoDS_Edge> typedEdges;
            for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
            op->setEdges(typedEdges);
            op->setRadius(static_cast<double>(m_edgeOpValue));
            if (op->execute(*m_document)) return true;
            // Failed — restore original
            m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
        } else {
            auto op = std::make_unique<ChamferOp>();
            op->setBody(m_edgeOpBodyId);
            std::vector<TopoDS_Edge> typedEdges;
            for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
            op->setEdges(typedEdges);
            op->setDistance(static_cast<double>(m_edgeOpValue));
            if (m_edgeOpTwoDist) op->setDistance2(static_cast<double>(m_edgeOpValue2));
            if (op->execute(*m_document)) return true;
            m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
        }
    } catch (...) {
        m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    }
    return false;
}

void Application::commitInteractiveEdgeOp() {
    // CREATE mode previews a transient op against the snapshot — restore it
    // before pushing the real op. EDIT mode previews through editStep, so
    // the document already reflects the history; clobbering the body here
    // would just be churn.
    if (m_edgeOpEditingIndex < 0)
        m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);

    // Confirming with no size set is a no-op — just cancel out. In edit mode
    // a zero value would be a "remove this fillet" — surprising semantics, so
    // we treat that as cancel too: restore the ORIGINAL parameter (the live
    // preview mutates the real op) and replay.
    if (m_edgeOpValue < 0.01f) {
        if (m_edgeOpEditingIndex >= 0) {
            setEdgeOpParam(m_history->getStep(m_edgeOpEditingIndex),
                           m_edgeOpType == EdgeOpType::Fillet,
                           m_edgeOpOrigValue,
                           m_edgeOpTwoDist ? m_edgeOpOrigValue2 : -1.0f);
            m_history->editStep(m_edgeOpEditingIndex, *m_document);
            refreshAllEdgeOpFaces();   // replayed — rebind every op's faces
        }
        m_edgeOpActive = false;
        m_edgeOpEditingIndex = -1;
        m_edgeOpEdges.clear();
        m_edgeOpPreviousShape.Nullify();
        m_edgeOpType = EdgeOpType::None;
        m_meshesDirty = true;
        return;
    }

    bool committed = true; // false if execute() rejected the result (create mode)
    if (m_edgeOpEditingIndex >= 0) {
        // Update the existing op's parameter and rerun from that point so any
        // downstream ops (cuts, fillets stacked on this one, …) recompute too.
        setEdgeOpParam(m_history->getStep(m_edgeOpEditingIndex),
                       m_edgeOpType == EdgeOpType::Fillet,
                       m_edgeOpValue,
                       m_edgeOpTwoDist ? m_edgeOpValue2 : -1.0f);
        bool editOk = m_history->editStep(m_edgeOpEditingIndex, *m_document);
        // Refresh face→op mapping after the edit so ownsFace() works on the new
        // body positions. The replay re-ran EVERY op's execute(), so every
        // fillet/chamfer (not just the edited one) needs rebinding — otherwise
        // the others' faces stay at their pre-Transform positions and become
        // un-clickable until the next reload.
        if (editOk) refreshAllEdgeOpFaces();

        // Detect a frozen op: the clicked body's geometry matches what we measured
        // at beginInteractiveEdgeOpEdit() time — before any preview ran. If the
        // commit didn't change the body at all from its original pre-edit state, the
        // op likely drives a different/deleted body (save-corruption edge case).
        // NOTE: we compare against the PRE-EDIT snapshot, not the post-preview
        // snapshot. The preview already changed the body to the new radius, so a
        // naive "before vs after this editStep" comparison would always report
        // "unchanged" even for a perfectly working edit.
        if (m_edgeOpPickedBodyId >= 0 &&
            (m_edgeOpPrePickedVol != 0.0 || m_edgeOpPrePickedArea != 0.0)) {
            double volAfter = 0, areaAfter = 0;
            try {
                TopoDS_Shape s = m_document->getBody(m_edgeOpPickedBodyId);
                if (!s.IsNull()) {
                    GProp_GProps gv, ga;
                    BRepGProp::VolumeProperties(s, gv);  volAfter  = gv.Mass();
                    BRepGProp::SurfaceProperties(s, ga); areaAfter = ga.Mass();
                }
            } catch (...) {}
            const double vtol = 1e-6 * std::max(1.0, std::fabs(m_edgeOpPrePickedVol));
            const double atol = 1e-6 * std::max(1.0, std::fabs(m_edgeOpPrePickedArea));
            const bool unchanged =
                std::fabs(volAfter  - m_edgeOpPrePickedVol)  <= vtol &&
                std::fabs(areaAfter - m_edgeOpPrePickedArea) <= atol;
            if (unchanged) {
                showToast("This fillet/chamfer is baked into the model \xE2\x80\x94 the "
                          "geometry you clicked has no editable operation behind it. "
                          "Re-apply it to make it adjustable.");
            }
        }
        std::fprintf(stdout, "%s edited to %.1f mm\n",
                     m_edgeOpType == EdgeOpType::Fillet ? "Fillet" : "Chamfer",
                     m_edgeOpValue);
    } else if (m_edgeOpType == EdgeOpType::Fillet) {
        auto op = std::make_unique<FilletOp>();
        op->setBody(m_edgeOpBodyId);
        std::vector<TopoDS_Edge> typedEdges;
        for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
        op->setEdges(typedEdges);
        op->setRadius(static_cast<double>(m_edgeOpValue));
        // Generative anchoring: tell the fillet which sketch drives this body
        // so a filleted corner can follow a later dimension edit. Inert unless
        // every filleted edge is a corner over a sketch vertex.
        for (const auto& [sid, bodies] : sketchBodyLinks())
            if (bodies.count(m_edgeOpBodyId)) { op->setSourceSketch(sid); break; }
        committed = m_history->pushOperation(std::move(op), *m_document);
    } else {
        auto op = std::make_unique<ChamferOp>();
        op->setBody(m_edgeOpBodyId);
        std::vector<TopoDS_Edge> typedEdges;
        for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
        op->setEdges(typedEdges);
        op->setDistance(static_cast<double>(m_edgeOpValue));
        if (m_edgeOpTwoDist) op->setDistance2(static_cast<double>(m_edgeOpValue2));
        for (const auto& [sid, bodies] : sketchBodyLinks())
            if (bodies.count(m_edgeOpBodyId)) { op->setSourceSketch(sid); break; }
        committed = m_history->pushOperation(std::move(op), *m_document);
    }

    if (m_edgeOpEditingIndex < 0) {
        if (committed) {
            std::fprintf(stdout, "%s %.1f mm committed\n",
                         m_edgeOpType == EdgeOpType::Fillet ? "Fillet" : "Chamfer",
                         m_edgeOpValue);
        } else {
            // execute() rejected the result (invalid topology / unbuildable at
            // this size) and left the body untouched — tell the user instead of
            // silently doing nothing.
            showToast(std::string(m_edgeOpType == EdgeOpType::Fillet
                                      ? "Fillet" : "Chamfer") +
                      " couldn't be built on those edges \xE2\x80\x94 the result "
                      "wasn't valid geometry. Try a smaller size or fewer edges.");
        }
    }

    m_edgeOpActive = false;
    m_edgeOpDragging = false;
    m_edgeOpEditingIndex = -1;
    m_edgeOpEdges.clear();
    m_edgeOpPreviousShape.Nullify();
    m_selection->clear();
    m_meshesDirty = true;
    m_edgeOpType = EdgeOpType::None;
}

void Application::cancelInteractiveEdgeOp() {
    if (m_edgeOpEditingIndex >= 0) {
        // The live preview mutated the real op — restore the parameter it had
        // when the edit began, then replay so the committed state (including
        // downstream ops) returns.
        setEdgeOpParam(m_history->getStep(m_edgeOpEditingIndex),
                       m_edgeOpType == EdgeOpType::Fillet,
                       m_edgeOpOrigValue,
                       m_edgeOpTwoDist ? m_edgeOpOrigValue2 : -1.0f);
        m_history->editStep(m_edgeOpEditingIndex, *m_document);
    } else if (m_edgeOpBodyId >= 0 && !m_edgeOpPreviousShape.IsNull()) {
        m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    }
    m_edgeOpActive = false;
    m_edgeOpDragging = false;
    m_edgeOpEditingIndex = -1;
    m_edgeOpEdges.clear();
    m_edgeOpPreviousShape.Nullify();
    m_edgeOpType = EdgeOpType::None;
    refreshAllEdgeOpFaces();   // body was replayed — rebind every op's faces
    m_meshesDirty = true;
}

void Application::refreshAllEdgeOpFaces() {
    if (!m_history || !m_document) return;
    for (int i = 0; i < m_history->stepCount(); ++i) {
        const Operation* op = m_history->getStep(i);
        if (!op || !op->isEnabled()) continue;
        // A fillet/chamfer's body may have been DELETED by a later step (e.g. a
        // filleted lid that was then deleted). getBody() throws on a missing id,
        // which — uncaught here — aborted the whole app on load ("Fatal error:
        // Body not found: N"). Skip any op whose body is gone; nothing to refresh.
        try {
            if (auto* f = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(op))) {
                TopoDS_Shape b = m_document->getBody(f->getBodyId());
                if (!b.IsNull()) f->refreshGeneratedFaces(b);
            } else if (auto* c = const_cast<ChamferOp*>(dynamic_cast<const ChamferOp*>(op))) {
                TopoDS_Shape b = m_document->getBody(c->getBodyId());
                if (!b.IsNull()) c->refreshGeneratedFaces(b);
            }
        } catch (...) { /* body deleted downstream — nothing to refresh */ }
    }
}

// ─── Edit Diameter (resize cylindrical / conical face) ──────────────────────
//
// Accepts a face pick (edits both end edges → stays a cylinder) or a single
// circular edge pick (edits just that end → makes a cone). The detection
// gathers axis + height + radii + hole-or-solid; the begin/update/commit path
// drives a ResizeCylindricalOp whose execute builds a ring solid and
// fuses/cuts it into the body.

bool Application::detectCylindricalResizeCandidate() {
    if (!m_selection || !m_document) return false;

    // Accept either exactly one face (edits both ends → stays cylindrical) or
    // exactly one edge (edits just that end → makes a cone). Anything else is
    // unambiguous to interpret, so we bail.
    TopoDS_Shape pickedFace, pickedEdge;
    int bodyId = -1;
    int faceCount = 0, edgeCount = 0;
    for (const auto& e : m_selection->getSelection()) {
        if (e.shape.IsNull()) continue;
        if (e.type == SelectionType::Face) {
            ++faceCount; pickedFace = e.shape; bodyId = e.bodyId;
        } else if (e.type == SelectionType::Edge) {
            ++edgeCount; pickedEdge = e.shape; bodyId = e.bodyId;
        }
    }
    if (bodyId < 0) return false;
    if (faceCount + edgeCount != 1) return false;

    const TopoDS_Shape& body = m_document->getBody(bodyId);

    // Find the cylindrical face we'll operate on. For a face pick, it's the
    // pick itself (must be cylindrical). For an edge pick, walk the body's
    // faces and pick the first cylindrical one that contains the edge.
    TopoDS_Face cylFace;
    if (!pickedFace.IsNull()) {
        TopoDS_Face face = TopoDS::Face(pickedFace);
        Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
        if (Handle(Geom_CylindricalSurface)::DownCast(surf).IsNull()) return false;
        cylFace = face;
    } else {
        TopoDS_Edge edge = TopoDS::Edge(pickedEdge);
        // Edge must be a circle for the diameter concept to make sense.
        try {
            BRepAdaptor_Curve curve(edge);
            if (curve.GetType() != GeomAbs_Circle) return false;
        } catch (...) { return false; }

        TopExp_Explorer fex(body, TopAbs_FACE);
        for (; fex.More(); fex.Next()) {
            TopoDS_Face face = TopoDS::Face(fex.Current());
            Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
            if (Handle(Geom_CylindricalSurface)::DownCast(surf).IsNull()) continue;
            TopExp_Explorer eex(face, TopAbs_EDGE);
            for (; eex.More(); eex.Next()) {
                if (eex.Current().IsSame(edge)) { cylFace = face; break; }
            }
            if (!cylFace.IsNull()) break;
        }
        if (cylFace.IsNull()) return false;
    }

    Handle(Geom_Surface) surf = BRep_Tool::Surface(cylFace);
    Handle(Geom_CylindricalSurface) cylSurf =
        Handle(Geom_CylindricalSurface)::DownCast(surf);
    if (cylSurf.IsNull()) return false;

    // Bounded parametric range. U = angular wrap, V = along axis. Must be a
    // CLOSED cylinder (full 2π) — partial sleeves (fillet faces) don't have
    // a meaningful single diameter.
    double u1, u2, v1, v2;
    BRepTools::UVBounds(cylFace, u1, u2, v1, v2);
    const double kCircle = 2.0 * M_PI;
    if (std::abs((u2 - u1) - kCircle) > 1e-3) return false;
    double height = std::abs(v2 - v1);
    if (height < 1e-6) return false;

    gp_Cylinder cyl = cylSurf->Cylinder();
    gp_Pnt shifted = cyl.Position().Location()
                        .Translated(gp_Vec(cyl.Position().Direction()) * v1);
    gp_Ax2 axis(shifted, cyl.Position().Direction(), cyl.Position().XDirection());
    double radius = cyl.Radius();

    // Hole vs solid boundary, from the face's outward normal at its centre.
    // Normal toward axis → material is OUTSIDE → hole. Away → solid boundary.
    // BRepGProp_Face::Normal() already applies the face's orientation
    // internally — don't reverse again (doing so double-negates and makes
    // every hole look like a solid boundary).
    BRepGProp_Face prop(cylFace);
    gp_Pnt centerPt; gp_Vec normVec;
    prop.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), centerPt, normVec);
    gp_Vec axisVec(axis.Direction());
    gp_Vec toCenter(shifted, centerPt);
    toCenter -= axisVec * toCenter.Dot(axisVec);
    if (toCenter.Magnitude() < 1e-6) return false;
    bool isHole = (normVec.Dot(toCenter) < 0.0);

    // Which end(s) are we editing? Face pick → both. Edge pick → just the
    // end whose centroid is closer to that edge's circle centre.
    bool editBottom = true, editTop = true;
    if (!pickedEdge.IsNull()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(pickedEdge));
        gp_Pnt edgeC = curve.Circle().Location();
        gp_Pnt botPnt = shifted;
        gp_Pnt topPnt = shifted.Translated(gp_Vec(axis.Direction()) * height);
        if (edgeC.Distance(botPnt) < edgeC.Distance(topPnt)) {
            editBottom = true; editTop = false;
        } else {
            editBottom = false; editTop = true;
        }
    }

    m_resizeCylBodyId   = bodyId;
    m_resizeCylIsHole   = isHole;
    m_resizeCylAxisOX = axis.Location().X();
    m_resizeCylAxisOY = axis.Location().Y();
    m_resizeCylAxisOZ = axis.Location().Z();
    m_resizeCylAxisDX = axis.Direction().X();
    m_resizeCylAxisDY = axis.Direction().Y();
    m_resizeCylAxisDZ = axis.Direction().Z();
    m_resizeCylAxisXX = axis.XDirection().X();
    m_resizeCylAxisXY = axis.XDirection().Y();
    m_resizeCylAxisXZ = axis.XDirection().Z();
    m_resizeCylHeight = height;
    m_resizeCylOriginalBottomR = radius;
    m_resizeCylOriginalTopR    = radius;
    m_resizeCylEditBottom = editBottom;
    m_resizeCylEditTop    = editTop;
    return true;
}


// ─── Thread (helical screw thread) ──────────────────────────────────────────
//
// The Toolbar's Thread button dispatches through the same cylindrical-face
// detector as Edit Diameter; beginThread copies its m_resizeCyl* output and
// opens the popup. No live preview: a helical sweep + boolean per frame is a
// multi-second operation, so the thread computes once on Apply.

void Application::beginThread() {
    cancelActiveIops();
    // Threads need a true cylinder — a cone's helix would leave the surface.
    if (std::abs(m_resizeCylOriginalBottomR - m_resizeCylOriginalTopR) > 1e-5) {
        std::fprintf(stderr, "[Thread] picked face is conical — thread needs "
                             "a cylinder\n");
        return;
    }
    m_threadBodyId  = m_resizeCylBodyId;
    m_threadIsHole  = m_resizeCylIsHole;
    m_threadRadius  = m_resizeCylOriginalBottomR;
    m_threadLength  = m_resizeCylHeight;
    m_threadAxis[0] = m_resizeCylAxisOX; m_threadAxis[1] = m_resizeCylAxisOY;
    m_threadAxis[2] = m_resizeCylAxisOZ; m_threadAxis[3] = m_resizeCylAxisDX;
    m_threadAxis[4] = m_resizeCylAxisDY; m_threadAxis[5] = m_resizeCylAxisDZ;
    m_threadAxis[6] = m_resizeCylAxisXX; m_threadAxis[7] = m_resizeCylAxisXY;
    m_threadAxis[8] = m_resizeCylAxisXZ;

    // ISO metric coarse defaults: nearest standard pitch for this diameter
    // (M10 → 1.5, M6 → 1.0, …), thread depth at the ISO ratio 0.6134·P.
    // Gives a recognisable "standard coarse bolt" out of the box instead of
    // a hairline scratch.
    static const struct { double d, p; } kIsoCoarse[] = {
        {1.6, 0.35}, {2.0, 0.4}, {2.5, 0.45}, {3.0, 0.5}, {4.0, 0.7},
        {5.0, 0.8},  {6.0, 1.0}, {8.0, 1.25}, {10.0, 1.5}, {12.0, 1.75},
        {16.0, 2.0}, {20.0, 2.5}, {24.0, 3.0}, {30.0, 3.5}, {36.0, 4.0},
        {42.0, 4.5}, {48.0, 5.0},
    };
    double dia = m_threadRadius * 2.0;
    double pitch = kIsoCoarse[0].p;
    double bestDelta = 1e9;
    for (const auto& e : kIsoCoarse) {
        double delta = std::abs(e.d - dia);
        if (delta < bestDelta) { bestDelta = delta; pitch = e.p; }
    }
    m_threadPitch = static_cast<float>(pitch);
    m_threadDepth = static_cast<float>(0.6134 * pitch);
    m_threadRightHanded = true;
    std::snprintf(m_threadPitchBuf, sizeof(m_threadPitchBuf), "%.2f", m_threadPitch);
    std::snprintf(m_threadDepthBuf, sizeof(m_threadDepthBuf), "%.2f", m_threadDepth);

    // Name the picked cylinder face topologically so the committed thread
    // FOLLOWS an upstream edit (the cylinder moving or its diameter changing)
    // instead of floating at its original absolute axis. Best-effort — an
    // unnameable face (imported/primitive cylinder with no sketch) leaves the
    // ref empty and the thread keeps today's absolute-param behaviour.
    m_threadFaceRef = materializr::topo::Ref{};
    if (m_selection && m_document) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type != SelectionType::Face || e.shape.IsNull() ||
                e.shape.ShapeType() != TopAbs_FACE)
                continue;
            try {
                materializr::topo::Context ctx;
                ctx.doc = m_document.get();
                ctx.shape = m_document->getBody(m_threadBodyId);
                ctx.type = TopAbs_FACE;
                m_threadFaceRef = materializr::topo::mint(TopoDS::Face(e.shape), ctx);
            } catch (...) {}
            break;
        }
    }

    m_threadActive = true;
}

// Build a ThreadOp from the popup's current state. Shared by the async
// commit (worker computes, main thread pushes) so both ops are identical.
std::unique_ptr<ThreadOp> Application::makeThreadOpFromState() const {
    auto op = std::make_unique<ThreadOp>();
    op->setBody(m_threadBodyId);
    op->setAxis(gp_Ax2(gp_Pnt(m_threadAxis[0], m_threadAxis[1], m_threadAxis[2]),
                       gp_Dir(m_threadAxis[3], m_threadAxis[4], m_threadAxis[5]),
                       gp_Dir(m_threadAxis[6], m_threadAxis[7], m_threadAxis[8])));
    op->setRadius(m_threadRadius);
    op->setLength(m_threadLength);
    op->setPitch(static_cast<double>(m_threadPitch));
    op->setDepth(static_cast<double>(m_threadDepth));
    op->setIsHole(m_threadIsHole);
    op->setRightHanded(m_threadRightHanded);
    op->setTargetFaceRef(m_threadFaceRef);
    return op;
}

void Application::commitThread() {
    if (m_threadBodyId < 0) { cancelThread(); return; }
    if (m_threadComputing) {
        // Assigning a new std::async future while the old one is in flight
        // BLOCKS until it finishes — exactly the "app frozen and punishing
        // the CPU" failure. One compute at a time.
        std::fprintf(stderr, "[Thread] Apply ignored — still computing\n");
        return;
    }
    std::fprintf(stderr, "[Thread] Apply: launching worker\n");
    // Kick the multi-second sweep + boolean onto a worker thread. renderThreadPanel
    // polls the future and pushes the real op — with the precomputed result — when
    // it lands.
    //
    // DEEP-COPY the shape into the worker. A bare TopoDS_Shape handle copy is
    // refcount-shared with the live document's TShape, whose lazy OCCT caches
    // (triangulation, bounding box, surface adaptors) the RENDER thread populates
    // and reads every frame — concurrent read/write on the same TShape is a data
    // race (UB → intermittent crash/corruption). BRepBuilderAPI_Copy gives the
    // worker an independent TShape, so the two threads share nothing.
    TopoDS_Shape live;
    try { live = m_document->getBody(m_threadBodyId); } catch (...) {}
    if (live.IsNull()) { cancelThread(); return; }
    TopoDS_Shape body = BRepBuilderAPI_Copy(live).Shape();
    if (body.IsNull()) { cancelThread(); return; }
    auto worker = std::make_shared<ThreadOp>();
    {
        auto cfg = makeThreadOpFromState();
        *worker = *cfg; // same params; worker only calls const buildResult()
    }
    // Pre-mesh on the worker at the CURRENT quality so the renderer's
    // tessellate() reuses the cache — meshing the swept rod's helicoid faces
    // on the main thread froze the app ~10s after the popup closed. Finer
    // angular pass (helicoids show 0.3 rad facets); linear must match the
    // app's exactly for the cache check.
    float mdefl, mang;
    meshQualityParams(mdefl, mang);
    const float meshAng = std::min(mang, 0.15f);
    m_threadFuture = std::async(std::launch::async,
        [worker, body, mdefl, meshAng]() {
            TopoDS_Shape r = worker->buildResult(body);
            if (!r.IsNull()) {
                try {
                    BRepMesh_IncrementalMesh mesh(r, mdefl, Standard_False,
                                                  meshAng, Standard_True);
                    mesh.Perform();
                } catch (...) {}
            }
            return r;
        });
    m_threadComputing = true;
    // Popup stays up (disabled) so the modal has an anchor; state is cleared
    // when the future resolves.
}

void Application::cancelThread() {
    m_threadActive = false;
    m_threadBodyId = -1;
}

void Application::beginResizeCylindrical() {
    cancelActiveIops();
    m_resizeCylPreviewFailed = false;
    if (m_resizeCylBodyId < 0) return;
    if (m_history->isBodyThreaded(m_resizeCylBodyId)) {
        std::fprintf(stderr, "[Resize] declined: body has a Thread step "
                             "(threads must be applied last)\n");
        showThreadsLastToast();
        m_resizeCylBodyId = -1;
        return;
    }
    try {
        m_resizeCylPreviousShape = m_document->getBody(m_resizeCylBodyId);
    } catch (...) { return; }

    m_resizeCylNewBottomDiameter = m_resizeCylOriginalBottomR * 2.0;
    m_resizeCylNewTopDiameter    = m_resizeCylOriginalTopR    * 2.0;
    std::snprintf(m_resizeCylBotBuf, sizeof(m_resizeCylBotBuf),
                  "%.2f", m_resizeCylNewBottomDiameter);
    std::snprintf(m_resizeCylTopBuf, sizeof(m_resizeCylTopBuf),
                  "%.2f", m_resizeCylNewTopDiameter);
    m_resizeCylInputFocus = true;
    m_resizeCylActive     = true;
}

void Application::updateResizeCylindrical() {
    if (!m_resizeCylActive || m_resizeCylBodyId < 0) return;
    m_document->updateBody(m_resizeCylBodyId, m_resizeCylPreviousShape);
    m_meshesDirty = true;

    double newBot = m_resizeCylEditBottom ? m_resizeCylNewBottomDiameter * 0.5
                                          : m_resizeCylOriginalBottomR;
    double newTop = m_resizeCylEditTop    ? m_resizeCylNewTopDiameter    * 0.5
                                          : m_resizeCylOriginalTopR;
    if (newBot < 1e-4 || newTop < 1e-4) return;
    if (std::abs(newBot - m_resizeCylOriginalBottomR) < 1e-5 &&
        std::abs(newTop - m_resizeCylOriginalTopR)    < 1e-5) return;

    try {
        gp_Ax2 axis(gp_Pnt(m_resizeCylAxisOX, m_resizeCylAxisOY, m_resizeCylAxisOZ),
                    gp_Dir(m_resizeCylAxisDX, m_resizeCylAxisDY, m_resizeCylAxisDZ),
                    gp_Dir(m_resizeCylAxisXX, m_resizeCylAxisXY, m_resizeCylAxisXZ));
        auto op = std::make_unique<ResizeCylindricalOp>();
        op->setBody(m_resizeCylBodyId);
        op->setAxis(axis);
        op->setHeight(m_resizeCylHeight);
        op->setOldRadii(m_resizeCylOriginalBottomR, m_resizeCylOriginalTopR);
        op->setNewRadii(newBot, newTop);
        op->setIsHole(m_resizeCylIsHole);
        if (op->execute(*m_document)) {
            m_resizeCylPreviewFailed = false;
            m_meshesDirty = true;
        } else {
            // Invalid diameter (e.g. a hole grown past the outer wall) —
            // keep the body at its original shape and flag the panel.
            m_document->updateBody(m_resizeCylBodyId, m_resizeCylPreviousShape);
            m_resizeCylPreviewFailed = true;
        }
    } catch (...) {
        m_document->updateBody(m_resizeCylBodyId, m_resizeCylPreviousShape);
        m_resizeCylPreviewFailed = true;
    }
}

void Application::commitResizeCylindrical() {
    if (!m_resizeCylActive) return;
    m_document->updateBody(m_resizeCylBodyId, m_resizeCylPreviousShape);

    double newBot = m_resizeCylEditBottom ? m_resizeCylNewBottomDiameter * 0.5
                                          : m_resizeCylOriginalBottomR;
    double newTop = m_resizeCylEditTop    ? m_resizeCylNewTopDiameter    * 0.5
                                          : m_resizeCylOriginalTopR;
    bool unchanged = std::abs(newBot - m_resizeCylOriginalBottomR) < 1e-5 &&
                     std::abs(newTop - m_resizeCylOriginalTopR)    < 1e-5;
    if (newBot < 1e-4 || newTop < 1e-4 || unchanged) {
        cancelResizeCylindrical();
        return;
    }

    gp_Ax2 axis(gp_Pnt(m_resizeCylAxisOX, m_resizeCylAxisOY, m_resizeCylAxisOZ),
                gp_Dir(m_resizeCylAxisDX, m_resizeCylAxisDY, m_resizeCylAxisDZ),
                gp_Dir(m_resizeCylAxisXX, m_resizeCylAxisXY, m_resizeCylAxisXZ));
    auto op = std::make_unique<ResizeCylindricalOp>();
    op->setBody(m_resizeCylBodyId);
    op->setAxis(axis);
    op->setHeight(m_resizeCylHeight);
    op->setOldRadii(m_resizeCylOriginalBottomR, m_resizeCylOriginalTopR);
    op->setNewRadii(newBot, newTop);
    op->setIsHole(m_resizeCylIsHole);
    m_history->pushOperation(std::move(op), *m_document);

    m_resizeCylActive = false;
    m_resizeCylBodyId = -1;
    m_resizeCylPreviousShape.Nullify();
    m_selection->clear();
    m_meshesDirty = true;
}

void Application::cancelResizeCylindrical() {
    if (m_resizeCylBodyId >= 0 && !m_resizeCylPreviousShape.IsNull()) {
        m_document->updateBody(m_resizeCylBodyId, m_resizeCylPreviousShape);
    }
    m_resizeCylActive = false;
    m_resizeCylBodyId = -1;
    m_resizeCylPreviousShape.Nullify();
    m_meshesDirty = true;
}

// ─── Interactive Shell ──────────────────────────────────────────────────────
//
// User picks a face on a body, clicks Shell, the popup pops with a thickness
// field defaulting to 1.0 mm. Typing rebuilds via ShellOp::execute against
// the snapshot for a live preview; Apply commits, Esc reverts.

// ─── Interactive Extrude (drag-to-distance) ─────────────────────────────────

double Application::extrudeOpDistance() const {
    // The profile face normal points outward from the body. For a Subtract the
    // tool must go into the body, so negate the distance.
    return (m_extrudeMode == ExtrudeMode::Subtract)
        ? -static_cast<double>(m_extrudeDistance)
        : static_cast<double>(m_extrudeDistance);
}

void Application::beginInteractiveExtrude(const TopoDS_Shape& profile,
                                          ExtrudeMode mode, int targetBody,
                                          int sourceSketchId) {
    // Extrude sweeps a profile along its normal — only meaningful for a FLAT
    // profile. A single curved body face (cylinder / sphere / fillet) has no
    // single normal, so extruding it produced garbage geometry; refuse with
    // guidance instead (mirrors Sketch-on-Face). Checked before cancelActiveIops
    // so a bad attempt doesn't disturb any in-progress op. Sketch profiles are
    // planar by construction; wire / compound profiles aren't a single face and
    // skip this check.
    if (profile.ShapeType() == TopAbs_FACE) {
        Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(profile));
        if (s.IsNull() || !s->IsKind(STANDARD_TYPE(Geom_Plane))) {
            showToast("Can't extrude a curved face \xE2\x80\x94 extrude works on "
                      "flat faces only.");
            return;
        }
    }
    cancelActiveIops();
    // Subtract/Union into a threaded body would boolean against the
    // thread's thousands of faces every preview frame — refuse up front
    // (NewBody doesn't touch an existing body, so it's always fine).
    if (mode != ExtrudeMode::NewBody && targetBody >= 0 &&
        m_history->isBodyThreaded(targetBody)) {
        std::fprintf(stderr, "[Extrude] declined: target body has a Thread "
                             "step (threads must be applied last)\n");
        showThreadsLastToast();
        return;
    }
    m_extrudeProfile = profile;
    m_extruding = true;
    m_extrudeMode = mode;
    m_extrudeTargetBody = targetBody;
    m_extrudeSketchId = sourceSketchId;
    m_extrudeDistance = 5.0f;
    std::snprintf(m_extrudeInputBuf, sizeof(m_extrudeInputBuf), "%.1f", m_extrudeDistance);
    m_extrudeInputFocus = true;

    // Compute face normal and center. A compound profile (multi-region
    // extrude — several letters at once) uses its first face: all regions
    // of one sketch are coplanar, so any face gives the right normal.
    TopoDS_Shape normShape = profile;
    if (profile.ShapeType() != TopAbs_FACE) {
        TopExp_Explorer fx(profile, TopAbs_FACE);
        if (fx.More()) normShape = fx.Current();
    }
    if (normShape.ShapeType() == TopAbs_FACE) {
        BRepGProp_Face prop(TopoDS::Face(normShape));
        gp_Pnt center;
        gp_Vec norm;
        double u1, u2, v1, v2;
        prop.Bounds(u1, u2, v1, v2);
        prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, center, norm);
        if (norm.Magnitude() > 1e-10) {
            m_extrudeNormal = glm::vec3(norm.X(), norm.Y(), norm.Z());
            m_extrudeNormal = glm::normalize(m_extrudeNormal);
        }
        m_extrudeOrigin = glm::vec3(center.X(), center.Y(), center.Z());
    }
    // Point the on-screen arrow into the body for a Subtract so dragging toward
    // the material deepens the cut.
    if (mode == ExtrudeMode::Subtract) m_extrudeNormal = -m_extrudeNormal;

    // Create initial preview body. The preview is always a NewBody (the solid
    // tool volume) so the user sees the shape being swept; for a Subtract it is
    // tinted/outlined red and the actual boolean cut happens on commit.
    auto op = std::make_unique<ExtrudeOp>();
    op->setProfile(profile);
    op->setDistance(extrudeOpDistance());
    op->setMode(ExtrudeMode::NewBody);
    op->setSketchSource(m_extrudeSketchId);
    {
        const Operation* raw = op.get();
        if (m_history->pushOperation(std::move(op), *m_document)) {
            m_extrudePreviewOp = raw;
            auto ids = m_document->getAllBodyIds();
            m_extrudePreviewBodyId = ids.back();
            m_meshesDirty = true;
        }
    }
}

void Application::updateInteractiveExtrude() {
    if (!m_extruding || m_extrudePreviewBodyId < 0) return;
    if (!std::isfinite(m_extrudeDistance)) { m_extrudeDistance = 0.0f; return; }

    // Remove old preview and create new one at current distance. The undo
    // is VERIFIED against the recorded preview op so an outside history
    // touch can never make us pop a committed step (see updatePushPull).
    m_document->removeBody(m_extrudePreviewBodyId);
    if (m_history->canUndo() &&
        m_history->getStep(m_history->currentStep()) == m_extrudePreviewOp) {
        m_history->undo(*m_document);
    } else {
        std::fprintf(stderr, "[Extrude] preview op no longer on top of "
                             "history — resyncing without undo\n");
    }
    m_extrudePreviewOp = nullptr;

    auto op = std::make_unique<ExtrudeOp>();
    op->setProfile(m_extrudeProfile);
    op->setDistance(extrudeOpDistance());
    op->setMode(ExtrudeMode::NewBody);
    op->setSketchSource(m_extrudeSketchId);
    const Operation* raw = op.get();
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_extrudePreviewOp = raw;
        auto ids = m_document->getAllBodyIds();
        m_extrudePreviewBodyId = ids.back();
        m_meshesDirty = true;
    }
}

void Application::commitInteractiveExtrude() {
    if (m_extrudeMode == ExtrudeMode::Subtract && m_extrudeTargetBody >= 0) {
        // Discard the NewBody tool preview and replace it with the real boolean
        // cut against the body the sketch was drawn on.
        if (m_extrudePreviewBodyId >= 0) {
            m_document->removeBody(m_extrudePreviewBodyId);
            if (m_history->canUndo() &&
                m_history->getStep(m_history->currentStep()) ==
                    m_extrudePreviewOp) {
                m_history->undo(*m_document);
            }
            m_extrudePreviewOp = nullptr;
        }
        auto op = std::make_unique<ExtrudeOp>();
        op->setProfile(m_extrudeProfile);
        op->setDistance(extrudeOpDistance());
        op->setMode(ExtrudeMode::Subtract);
        op->setTargetBody(m_extrudeTargetBody);
        op->setSketchSource(m_extrudeSketchId);
        if (m_history->pushOperation(std::move(op), *m_document)) {
            markDirty();
            std::fprintf(stdout, "Subtracted %.1f mm from body %d\n",
                         std::abs(m_extrudeDistance), m_extrudeTargetBody);
        } else {
            std::fprintf(stderr, "Subtract failed\n");
        }
    } else {
        // NewBody: the preview op is already the result — just finalize.
        std::fprintf(stdout, "Extruded %.1f mm\n", m_extrudeDistance);
    }

    m_extruding = false;
    m_extrudeProfile.Nullify();
    m_extrudePreviewBodyId = -1;
    m_extrudeMode = ExtrudeMode::NewBody;
    m_extrudeTargetBody = -1;
    m_meshesDirty = true;
}

void Application::cancelInteractiveExtrude() {
    if (m_extrudePreviewBodyId >= 0) {
        m_document->removeBody(m_extrudePreviewBodyId);
        if (m_history->canUndo() &&
            m_history->getStep(m_history->currentStep()) ==
                m_extrudePreviewOp) {
            m_history->undo(*m_document);
        }
        m_extrudePreviewOp = nullptr;
    }
    m_extruding = false;
    m_extrudeProfile.Nullify();
    m_extrudePreviewBodyId = -1;
    m_extrudeMode = ExtrudeMode::NewBody;
    m_extrudeTargetBody = -1;
    m_meshesDirty = true;
}

// ─── Push / Pull ────────────────────────────────────────────────────────────
//
// Picks up either selected sketch regions or selected body faces and drives
// a PushPullOp at the typed/dragged distance. Live preview is implemented by
// pushing and undoing successive preview ops on the history stack.

Application::SketchRegionHit Application::pickSketchRegion(float screenX, float screenY,
                                                           float vpW, float vpH,
                                                           bool buildIfCold) const {
    SketchRegionHit hit;
    if (!m_document || !m_viewport) return hit;

    const Camera& cam = m_viewport->getCamera();
    glm::mat4 view = cam.getViewMatrix();
    glm::mat4 proj = cam.getProjectionMatrix();
    glm::mat4 invVP = glm::inverse(proj * view);

    // Build a world-space ray through a given pixel.
    auto rayAt = [&](float sx, float sy, glm::vec3& origin, glm::vec3& dir) {
        float ndcx = (sx / vpW) * 2.0f - 1.0f;
        float ndcy = 1.0f - (sy / vpH) * 2.0f;
        glm::vec4 n = invVP * glm::vec4(ndcx, ndcy, -1.0f, 1.0f);
        glm::vec4 f = invVP * glm::vec4(ndcx, ndcy, 1.0f, 1.0f);
        n /= n.w; f /= f.w;
        origin = glm::vec3(n);
        dir = glm::normalize(glm::vec3(f) - glm::vec3(n));
    };

    glm::vec3 rayOrigin, rayDir;
    rayAt(screenX, screenY, rayOrigin, rayDir);

    float bestT = std::numeric_limits<float>::infinity();

    auto testSketch = [&](int sketchId, const Sketch& sketch) {
        const gp_Pln& pln = sketch.getPlane();
        const gp_Ax3& ax = pln.Position();
        glm::vec3 planeOrigin(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
        glm::vec3 planeNormal(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
        glm::vec3 planeX(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
        glm::vec3 planeY(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());

        auto projectToPlane = [&](glm::vec3 o, glm::vec3 d, float& tOut, glm::vec2& p2dOut) -> bool {
            float denom = glm::dot(d, planeNormal);
            if (std::abs(denom) < 1e-8f) return false;
            float t = glm::dot(planeOrigin - o, planeNormal) / denom;
            if (t <= 0.0f) return false;
            glm::vec3 local = (o + d * t) - planeOrigin;
            tOut = t;
            p2dOut = glm::vec2(glm::dot(local, planeX), glm::dot(local, planeY));
            return true;
        };

        float t;
        glm::vec2 p2d;
        if (!projectToPlane(rayOrigin, rayDir, t, p2d)) return;
        if (t >= bestT) return;

        // Cold region cache: building it runs the OCCT general fuse — on a
        // heavy sketch (SVG import, text) that's a SECONDS-long stall. The
        // per-frame HOVER pick must never trigger it (unhiding a complex
        // sketch used to freeze the app on the very next mouse move); a
        // CLICK still builds (one user-initiated wait, exactly as before).
        if (!buildIfCold && !sketch.regionsCached()) return;

        // Screen-space pick tolerance: how far ~6px maps to on this plane, so the
        // boundary catch area is a consistent, comfortable width at any zoom.
        float tol = 0.0f;
        glm::vec3 o2, d2; float t2; glm::vec2 p2d2;
        rayAt(screenX + 6.0f, screenY, o2, d2);
        if (projectToPlane(o2, d2, t2, p2d2)) tol = glm::length(p2d2 - p2d);

        auto regions = sketch.buildRegions();
        // Resolve overlapping candidates by two ranked rules instead of
        // first-match (BOP region order is arbitrary):
        //   1. STRICT containment beats near-boundary proximity. A click
        //      inside a letter is also within `tol` of the surrounding
        //      region's hole edge; first-match let the big region steal
        //      every click that wasn't dead-centre in the stroke.
        //   2. Among matches of equal rank, the SMALLEST region wins —
        //      clicking inside a nested shape picks the shape, not the
        //      sea it sits in.
        int bestIdx = -1;
        bool bestInside = false;
        double bestArea = 0.0;
        for (size_t i = 0; i < regions.size(); ++i) {
            bool inside = sketch.isPointInRegion(regions[i], p2d);
            if (!inside &&
                !sketch.isPointInOrNearRegion(regions[i], p2d, tol))
                continue;
            double area = 0.0;
            try {
                GProp_GProps props;
                BRepGProp::SurfaceProperties(regions[i].face, props);
                area = std::abs(props.Mass());
            } catch (...) {}
            bool better =
                bestIdx < 0 ||
                (inside && !bestInside) ||
                (inside == bestInside && area < bestArea);
            if (better) {
                bestIdx = static_cast<int>(i);
                bestInside = inside;
                bestArea = area;
            }
        }
        if (bestIdx >= 0) {
            bestT = t;
            hit.sketchId = sketchId;
            hit.regionIndex = bestIdx;
            hit.worldPoint = rayOrigin + rayDir * t;
            return;
        }

        // Fallback: edge picking. Open profiles (an arc, an unclosed polyline,
        // a spline used as a loft rib, …) have no closed region, so the loop
        // above misses them entirely — which used to make such sketches
        // unselectable from the viewport. Test 2D distance from the click
        // point to each primitive; if it lands within `tol` of any line /
        // circle / arc / spline / polygon edge, treat that as a whole-
        // sketch hit (regionIndex stays -1) so the viewport input handler
        // can emit a SelectionType::Sketch.
        auto getPoint2D = [&](int ptId) -> glm::vec2 {
            const SketchPoint* sp = sketch.getPoint(ptId);
            return sp ? sp->pos : glm::vec2(0.0f);
        };
        auto distPointToSegment = [](glm::vec2 p, glm::vec2 a, glm::vec2 b) -> float {
            glm::vec2 ab = b - a;
            float len2 = glm::dot(ab, ab);
            if (len2 < 1e-12f) return glm::length(p - a);
            float u = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
            return glm::length(p - (a + u * ab));
        };

        bool nearEdge = false;
        for (const auto& ln : sketch.getLines()) {
            if (distPointToSegment(p2d, getPoint2D(ln.startPointId),
                                        getPoint2D(ln.endPointId)) <= tol) {
                nearEdge = true; break;
            }
        }
        if (!nearEdge) {
            for (const auto& c : sketch.getCircles()) {
                glm::vec2 ctr = getPoint2D(c.centerPointId);
                float r = static_cast<float>(c.radius);
                float d = std::abs(glm::length(p2d - ctr) - r);
                if (d <= tol) { nearEdge = true; break; }
            }
        }
        if (!nearEdge) {
            for (const auto& a : sketch.getArcs()) {
                glm::vec2 ctr = getPoint2D(a.centerPointId);
                float r = static_cast<float>(a.radius);
                glm::vec2 s = getPoint2D(a.startPointId);
                glm::vec2 e = getPoint2D(a.endPointId);
                float d = std::abs(glm::length(p2d - ctr) - r);
                if (d > tol) continue;
                // Angle-in-arc test: only count if p2d's angle from centre
                // falls within the CCW sweep from start to end.
                float a0 = std::atan2(s.y - ctr.y, s.x - ctr.x);
                float a1 = std::atan2(e.y - ctr.y, e.x - ctr.x);
                float ap = std::atan2(p2d.y - ctr.y, p2d.x - ctr.x);
                auto norm = [](float x) {
                    while (x < 0) x += 2.0f * static_cast<float>(M_PI);
                    while (x >= 2.0f * static_cast<float>(M_PI)) x -= 2.0f * static_cast<float>(M_PI);
                    return x;
                };
                float span = norm(a1 - a0);
                float here = norm(ap - a0);
                if (here <= span) { nearEdge = true; break; }
            }
        }
        if (!nearEdge) {
            // Splines + polygons: approximate by walking the control / vertex
            // chain as a polyline. Good enough for picking; the renderer
            // tessellates the same control sequence anyway.
            for (const auto& sp : sketch.getSplines()) {
                const auto& ids = sp.controlPointIds;
                for (size_t i = 1; i < ids.size(); ++i) {
                    if (distPointToSegment(p2d, getPoint2D(ids[i-1]),
                                                getPoint2D(ids[i])) <= tol) {
                        nearEdge = true; break;
                    }
                }
                if (nearEdge) break;
            }
        }
        if (!nearEdge) {
            for (const auto& poly : sketch.getPolygons()) {
                const auto& ids = poly.vertexPointIds;
                for (size_t i = 0; i < ids.size(); ++i) {
                    glm::vec2 a = getPoint2D(ids[i]);
                    glm::vec2 b = getPoint2D(ids[(i + 1) % ids.size()]);
                    if (distPointToSegment(p2d, a, b) <= tol) {
                        nearEdge = true; break;
                    }
                }
                if (nearEdge) break;
            }
        }

        if (nearEdge) {
            bestT = t;
            hit.sketchId = sketchId;
            hit.regionIndex = -1; // edge-only hit; caller emits SelectionType::Sketch
            hit.worldPoint = rayOrigin + rayDir * t;
        }
    };

    // Test the active sketch first (most relevant when in sketch mode)
    if (m_activeSketch) testSketch(m_activeSketchId, *m_activeSketch);
    // Then all stored sketches
    for (int sid : m_document->getAllSketchIds()) {
        if (!m_document->isSketchVisible(sid)) continue;
        if (sid == m_activeSketchId) continue;
        auto sk = m_document->getSketch(sid);
        if (sk) testSketch(sid, *sk);
    }

    return hit;
}

void Application::beginPushPull() {
    cancelActiveIops();
    m_pushPullTargets.clear();
    m_pushPullPreviewBodyIds.clear();
    m_pushPullPreviousBodies.clear();
    m_pushPullLiveOp.reset();
    m_pushPullPreviewApplied = false;
    m_pushPullSymmetric = false;
    m_pushPullDistanceRaw = 0.0f;

    // Gather all selected SketchRegion entries AND body face selections.
    for (const auto& e : m_selection->getSelection()) {
        if (e.type == SelectionType::SketchRegion) {
            // For sketches loaded from a previous session, the in-memory
            // sourceFace may not have been bound yet (it isn't serialised).
            // Refresh it now so buildRegions correctly subtracts any existing
            // hole / inner wire the user sketched around.
            ensureSketchSourceFace(e.sketchId);
            auto sketch = m_document->getSketch(e.sketchId);
            if (!sketch) continue;
            auto regions = sketch->buildRegions();
            if (e.subShapeIndex < 0 || e.subShapeIndex >= static_cast<int>(regions.size())) continue;
            PushPullTarget t;
            t.sketchId = e.sketchId;
            t.regionIndex = e.subShapeIndex;
            // A DETACHED sketch has been deliberately broken away from its
            // former host (moved independently in 3D). Keeping the stale
            // source-body id fused the new prism into a body that can be
            // hundreds of mm away — a push/pull on an unlinked sketch must
            // behave like a free-floating sketch and make its own body.
            t.sourceBodyId = sketch->isDetachedFromBody()
                                 ? -1
                                 : sketch->getSourceBody();
            t.profile = regions[e.subShapeIndex].face;
            if (t.profile.IsNull()) continue;
            // PUSH/PULL adopts the body the sketch sits flat ON. A sketch with
            // no body link (e.g. drawn on a construction plane and used to cut
            // a hole) that lies coplanar-and-over a visible body's face should
            // fuse/cut that body in place — not spawn a separate solid that
            // overlaps and z-fights it. (Extrude From keeps its always-new-
            // body semantics; a new body from this sketch is one Extrude
            // away.) A DETACHED sketch was deliberately unlinked, so it stays
            // free-floating; genuine free-space sketches (over no face) return
            // -1 and are unaffected.
            if (t.sourceBodyId < 0 && !sketch->isDetachedFromBody()) {
                int host = findBodyUnderRegion(t.profile, sketch->getPlane());
                if (host >= 0) t.sourceBodyId = host;
            }
            m_pushPullTargets.push_back(t);
        } else if (e.type == SelectionType::Face && !e.shape.IsNull()) {
            // Push/Pull on a body face: face is the profile, the owning body is the source.
            // Positive distance extrudes outward (Fuse), negative cuts inward (Cut).
            PushPullTarget t;
            t.sketchId = -1;
            t.regionIndex = -1;
            t.sourceBodyId = e.bodyId;
            t.profile = TopoDS::Face(e.shape);
            if (t.profile.IsNull()) continue;
            m_pushPullTargets.push_back(t);
        }
    }

    if (m_pushPullTargets.empty()) {
        std::fprintf(stderr, "Push/Pull: select a sketch region or a body face first\n");
        return;
    }

    // THREADS ARE A FINISHING PASS. A boolean push/pull against a threaded
    // body runs the cut over the thread's thousands of faces — and the
    // interactive preview would do that EVERY frame, freezing the app long
    // before it reached the commit-time refusal in History::pushOperation.
    // Refuse up front with guidance instead. (Steve: it "just went
    // unresponsive".)
    for (const auto& t : m_pushPullTargets) {
        if (t.sourceBodyId >= 0 && m_history->isBodyThreaded(t.sourceBodyId)) {
            std::fprintf(stderr, "[Push/Pull] declined: this body has a "
                                 "Thread step. Threads must be applied LAST "
                                 "— delete the Thread, make this change, "
                                 "then re-thread.\n");
            m_pushPullTargets.clear();
            showThreadsLastToast();
            return;
        }
    }

    // Arrow direction at the first target's centre.
    //
    // For a flat face the UV-midpoint surface normal IS the face normal, but
    // for a CURVED face (chamfer cone, fillet torus, side of a cylinder, etc.)
    // that normal is the surface tangent perpendicular at one specific point —
    // sloped for a cone, twisted for a torus. The push/pull arrow then looks
    // like it "follows the polygon clicked" instead of a stable axis.
    //
    // Fix: if the face's underlying surface has a natural rotation axis (cone,
    // torus, cylinder, surface of revolution), use that axis as the push/pull
    // direction. Sign-correct it so positive distance still points outward
    // (positive dot product with the UV-midpoint normal preserves the
    // "fuse for +, cut for −" convention the user expects).
    m_pushPullHasArrow = false;
    try {
        const auto& tgt0 = m_pushPullTargets.front();
        const TopoDS_Face& f = tgt0.profile;
        if (!f.IsNull()) {
            BRepGProp_Face prop(f);
            double u1, u2, v1, v2;
            prop.Bounds(u1, u2, v1, v2);
            gp_Pnt c; gp_Vec n;
            prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, c, n);
            if (n.Magnitude() > 1e-10) {
                // NO outward correction — BRepGProp_Face::Normal() already
                // applies face orientation; this IS outward. MUST mirror
                // PushPullOp::execute (see the war-story comment there).
                gp_Vec dir = n;
                Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
                gp_Dir axis;
                bool hasAxis = false;
                if (auto cone =
                        Handle(Geom_ConicalSurface)::DownCast(surf); !cone.IsNull()) {
                    axis = cone->Axis().Direction(); hasAxis = true;
                } else if (auto tor =
                        Handle(Geom_ToroidalSurface)::DownCast(surf); !tor.IsNull()) {
                    axis = tor->Axis().Direction(); hasAxis = true;
                } else if (auto cyl =
                        Handle(Geom_CylindricalSurface)::DownCast(surf); !cyl.IsNull()) {
                    axis = cyl->Axis().Direction(); hasAxis = true;
                } else if (auto rev =
                        Handle(Geom_SurfaceOfRevolution)::DownCast(surf); !rev.IsNull()) {
                    axis = rev->Axis().Direction(); hasAxis = true;
                }
                if (hasAxis) {
                    dir = gp_Vec(axis);
                    if (dir.Dot(n) < 0) dir.Reverse();
                } else if (tgt0.sourceBodyId >= 0) {
                    // Mirror PushPullOp::execute: correct a genuinely-inverted
                    // planar normal so the live arrow agrees with the executed
                    // direction (bug #5). Untouched for curved/axis faces and
                    // for every reading that isn't an unambiguous inverted pair.
                    dir = correctedOutwardNormal(
                        m_document->getBody(tgt0.sourceBodyId), f, c, dir);
                }
                m_pushPullNormal = glm::normalize(glm::vec3(dir.X(), dir.Y(), dir.Z()));
                m_pushPullOrigin = glm::vec3(c.X(), c.Y(), c.Z());
                m_pushPullHasArrow = true;
            }
        }
    } catch (...) {}

    m_pushPullActive = true;
    m_pushPullDistance = 0.0f; // start at no change; drag the arrow or type a value
    std::snprintf(m_pushPullInputBuf, sizeof(m_pushPullInputBuf), "%.1f", m_pushPullDistance);
    m_pushPullInputFocus = true;

    // Dense bodies (a threaded rod has hundreds of helical faces) cannot
    // afford a real boolean per drag frame — and since push/pull now
    // triggers the thread-last reflow, each preview frame would re-thread
    // the whole rod (Steve: drag "a no go, non-responsive ~10s"). Those
    // bodies get a GHOST preview (tinted tool volume) and run the real
    // boolean once, on commit.
    m_pushPullHeavyPreview = false;
    for (const auto& t : m_pushPullTargets) {
        if (t.sourceBodyId < 0) continue;
        try {
            int nf = 0;
            for (TopExp_Explorer fx(m_document->getBody(t.sourceBodyId),
                                    TopAbs_FACE);
                 fx.More() && nf <= 250; fx.Next()) ++nf;
            if (nf > 250) { m_pushPullHeavyPreview = true; break; }
        } catch (...) {}
    }

    updatePushPull();
}

void Application::updatePushPull(bool applySnap) {
    if (!m_pushPullActive) return;
    if (!std::isfinite(m_pushPullDistance)) { m_pushPullDistance = 0.0f; return; }

    // Snap the live distance to the corner-widget grid step before applying.
    // Mutating m_pushPullDistance itself (rather than just the value passed
    // to setDistance) means the dim-arrow readout, the InputText field, and
    // the slider all reflect the snapped value — there's no "type 5.3, see
    // 5.3 in the field, body extrudes to 5.0" discrepancy. Toggling snap off
    // mid-drag immediately frees the distance to fine values on the next
    // updatePushPull frame.
    if (applySnap && m_snapToGrid && m_sketchGridStep > 0.0f) {
        const float step = m_sketchGridStep;
        m_pushPullDistance = std::round(m_pushPullDistance / step) * step;
        std::snprintf(m_pushPullInputBuf, sizeof(m_pushPullInputBuf),
                      "%.1f", m_pushPullDistance);
    }

    // GHOST PREVIEW for dense bodies: render the tool volume tinted instead
    // of running the boolean (and the thread reflow behind it) per frame.
    if (m_pushPullHeavyPreview) {
        constexpr int kGhostId = -7777; // renderer-only slot, no Document body
        bool any = false;
        TopoDS_Compound comp;
        BRep_Builder bb;
        bb.MakeCompound(comp);
        if (std::abs(m_pushPullDistance) > 1e-6) {
            gp_Vec pv(m_pushPullNormal.x, m_pushPullNormal.y,
                      m_pushPullNormal.z);
            pv *= static_cast<double>(m_pushPullDistance);
            for (const auto& t : m_pushPullTargets) {
                if (t.profile.IsNull()) continue;
                try {
                    BRepPrimAPI_MakePrism mk(t.profile, pv);
                    mk.Build();
                    if (mk.IsDone()) { bb.Add(comp, mk.Shape()); any = true; }
                } catch (...) {}
            }
        }
        if (any) {
            int slot = m_shapeRenderer->setBodyMesh(kGhostId, comp);
            if (slot >= 0) {
                m_shapeRenderer->setSubtractPreview(slot,
                                                    m_pushPullDistance < 0.0f);
                m_shapeRenderer->setColor(slot, glm::vec3(0.55f, 0.75f, 1.0f));
            }
        } else {
            m_shapeRenderer->removeBody(kGhostId);
        }
        return;
    }

    // SNAPSHOT/RESTORE preview engine — history is NOT involved. One live
    // op instance is undone and re-executed directly against the document:
    // PushPullOp::undo() removes its created bodies into the id-reuse pool
    // (so re-execution keeps the SAME ids) and restores mutated sources.
    // History sees a single pushExecuted() at commit. This replaces the
    // per-frame history undo/push churn that produced an entire class of
    // bugs: body ids changing every frame, empty-document click windows,
    // and outside history touches corrupting the preview bookkeeping.
    if (!m_pushPullLiveOp) m_pushPullLiveOp = makePushPullOpFromState();
    if (m_pushPullPreviewApplied) {
        m_pushPullLiveOp->undo(*m_document);
        m_pushPullPreviewApplied = false;
    }
    if (std::abs(m_pushPullDistance) > 1e-6) {
        m_pushPullLiveOp->setDistance(
            static_cast<double>(m_pushPullDistance));
        m_pushPullLiveOp->setSymmetric(m_pushPullSymmetric);
        if (m_pushPullLiveOp->execute(*m_document))
            m_pushPullPreviewApplied = true;
    }
    // Mark only the bodies the push/pull actually touched as dirty. On a
    // 100+ body project this turns each preview frame from "re-tessellate
    // every visible body" into "re-tessellate 1-2 bodies", which is the
    // difference between unusable and smooth.
    for (const auto& t : m_pushPullTargets) {
        if (t.sourceBodyId >= 0) markBodyDirty(t.sourceBodyId);
    }
    // Free-floating push/pull creates new bodies — mark them too so they
    // appear / refresh. Restrict to VISIBLE bodies: invisible ones never get
    // a renderer slot (full-rebuild skips them), so without the visibility
    // check they'd be marked dirty every preview frame forever — pure waste.
    for (int id : m_document->getAllBodyIds()) {
        if (!m_document->isBodyVisible(id)) continue;
        if (m_shapeRenderer->findSlotByBody(id) < 0) markBodyDirty(id);
    }
}

// Build a PushPullOp from the current interactive state. Shared by the
// light path (op pushed per preview frame) and the heavy/ghost path (op
// pushed once, on commit).
std::unique_ptr<PushPullOp> Application::makePushPullOpFromState() const {
    auto op = std::make_unique<PushPullOp>();
    std::vector<PushPullOp::Target> targets;
    for (const auto& t : m_pushPullTargets) {
        PushPullOp::Target ot;
        ot.profile = t.profile;
        ot.sourceBodyId = t.sourceBodyId;
        targets.push_back(ot);
    }
    op->setTargets(std::move(targets));
    op->setDistance(static_cast<double>(m_pushPullDistance));
    op->setSymmetric(m_pushPullSymmetric);
    // Cut-intersecting: a free-space sketch (cut-or-new-body) OR any cut-
    // direction push/pull (also cut the other visible bodies in the path). An
    // extrude (positive) on a source/face body keeps it OFF → fuses its source
    // only, exactly as before.
    bool allFreeSketch = !m_pushPullTargets.empty();
    for (const auto& t : m_pushPullTargets)
        if (!(t.sourceBodyId < 0 && t.sketchId >= 0)) { allFreeSketch = false; break; }
    op->setCutIntersecting(allFreeSketch || m_pushPullDistance < 0.0f);
    // Cascade plumbing: stamp the originating sketch+region on every target.
    // setTargets() above pre-sizes the source arrays to all -1, so this
    // upgrades them where we actually have a sketch source. Free-face
    // pushpulls (sourceBodyId-driven, no sketch) keep -1.
    for (size_t i = 0; i < m_pushPullTargets.size(); ++i) {
        const auto& t = m_pushPullTargets[i];
        if (t.sketchId >= 0) {
            op->setSketchSource(static_cast<int>(i), t.sketchId, t.regionIndex);
        }
    }
    return op;
}

void Application::commitPushPull() {
    // Smart cut: a free-space sketch push/pull that runs into visible bodies
    // subtracts from them (each separately) instead of making an overlapping new
    // body. Only this exact case (every target a free-space sketch region) is
    // rerouted; everything else falls through to the unchanged paths below. The
    // preview always showed the new-body extrusion, so undo it and run one fresh
    // cut-enabled execute. The op itself falls back to a new body if it hits
    // nothing — so "no intersection" is still today's behaviour.
    bool allFreeSketch = !m_pushPullTargets.empty();
    for (const auto& t : m_pushPullTargets)
        if (!(t.sourceBodyId < 0 && t.sketchId >= 0)) { allFreeSketch = false; break; }
    // Reroute when the result cuts through multiple bodies: a free-space sketch
    // (cut-or-new-body), or ANY cut-direction push/pull (cuts the source body
    // AND every other visible body in its path). Extrude (add) and non-sketch
    // cases fall through to the unchanged paths below.
    bool smartCut = std::abs(m_pushPullDistance) > 1e-6 &&
                    !m_pushPullTargets.empty() &&
                    (allFreeSketch || m_pushPullDistance < 0.0f);
    if (smartCut) {
        m_shapeRenderer->removeBody(-7777);
        if (m_pushPullLiveOp && m_pushPullPreviewApplied) {
            m_pushPullLiveOp->undo(*m_document);   // remove the preview new body
            m_pushPullPreviewApplied = false;
        }
        if (!m_history->pushOperation(makePushPullOpFromState(), *m_document))
            std::fprintf(stderr, "Push/Pull (cut) failed to apply\n");
        m_pushPullLiveOp.reset();
        m_pushPullHeavyPreview = false;
        m_pushPullPreviewApplied = false;
        m_pushPullActive = false;
        m_pushPullSticky = false;
        m_pushPullTargets.clear();
        m_meshesDirty = true;
        m_selection->clear();
        std::fprintf(stdout, "Push/Pull (smart cut) committed at %.2f mm\n",
                     m_pushPullDistance);
        return;
    }

    if (m_pushPullHeavyPreview) {
        // Ghost path: drop the preview mesh and run the real boolean ONCE.
        // This is where the thread reflow runs for dense bodies — a single
        // synchronous recompute instead of one per drag frame.
        m_shapeRenderer->removeBody(-7777);
        if (std::abs(m_pushPullDistance) > 1e-6) {
            if (!m_history->pushOperation(makePushPullOpFromState(),
                                          *m_document)) {
                std::fprintf(stderr, "Push/Pull failed to apply\n");
            }
        }
        m_pushPullHeavyPreview = false;
    }
    // Light path: the preview already applied the final state directly —
    // append the live op to history WITHOUT re-executing it.
    if (m_pushPullLiveOp && m_pushPullPreviewApplied) {
        m_history->pushExecuted(std::move(m_pushPullLiveOp));
    }
    m_pushPullLiveOp.reset();
    m_pushPullPreviewApplied = false;
    m_pushPullActive = false;
    m_pushPullSticky = false;
    m_pushPullTargets.clear();
    m_meshesDirty = true;
    m_selection->clear();
    std::fprintf(stdout, "Push/Pull committed at %.2f mm\n", m_pushPullDistance);
}

void Application::cancelPushPull() {
    if (!m_pushPullActive) return;
    if (m_pushPullHeavyPreview) {
        m_shapeRenderer->removeBody(-7777); // ghost only — nothing was pushed
        m_pushPullHeavyPreview = false;
    }
    if (m_pushPullLiveOp && m_pushPullPreviewApplied) {
        m_pushPullLiveOp->undo(*m_document);
    }
    m_pushPullLiveOp.reset();
    m_pushPullPreviewApplied = false;
    m_pushPullActive = false;
    m_pushPullSticky = false;
    m_pushPullTargets.clear();
    m_meshesDirty = true;
}

// ─── Move Face (in-plane slide → whole-body shear) ──────────────────────────

void Application::beginMoveFace(FaceXform kind) {
    cancelAllInteractivePreviews();
    m_moveFaceActive = false;
    m_moveFaceBodyId = -1;
    m_moveFaceFace.Nullify();
    m_faceXformKind = kind;
    m_moveFaceVec = glm::vec3(0.0f);
    m_moveFaceBase = glm::vec3(0.0f);
    m_moveFaceAngle = m_moveFaceAngleBase = 0.0f;
    m_moveFaceRotAccum = glm::mat3(1.0f);
    m_moveFaceRotHasAccum = false;
    m_moveFaceTwist = m_moveFaceTwistBase = 0.0f;
    m_moveFaceIsTwist = false;
    m_moveFaceScale = m_moveFaceScaleBase = 1.0f;
    m_moveFaceScaleA = m_moveFaceScaleABase = 1.0f;
    m_moveFaceScaleB = m_moveFaceScaleBBase = 1.0f;
    m_moveFaceDragging = false;
    m_moveHoleMode = false;
    m_moveHoleWall.Nullify();

    // Hole move: if the Move selection is a recognizable THROUGH-HOLE wall, slide
    // the whole hole (MoveHoleOp) instead of shearing a face. buildVoid succeeds
    // only on a real hole wall (an outer face / block side fails it), so this
    // doesn't hijack ordinary Move Face. Translate only; pockets are refused.
    if (kind == FaceXform::Translate && m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type != SelectionType::Face || e.shape.IsNull()) continue;
            TopoDS_Shape body;
            try { body = m_document->getBody(e.bodyId); } catch (...) { continue; }
            if (body.IsNull()) continue;
            TopoDS_Face wall = TopoDS::Face(e.shape);
            TopoDS_Shape voidSolid; gp_Vec entryN; bool pocket = false;
            TopoDS_Wire rim;
            if (MoveHoleOp::buildVoid(body, wall, voidSolid, entryN, pocket, &rim)) {
                // Gizmo set-up at the hole: plane = entry face, translate only.
                m_moveHoleMode = true;
                m_moveHoleWall = wall;
                m_moveFaceBodyId = e.bodyId;
                m_moveFacePreviousShape = body;
                m_moveFaceN = glm::normalize(glm::vec3(entryN.X(), entryN.Y(), entryN.Z()));
                try {
                    GProp_GProps gp; BRepGProp::SurfaceProperties(wall, gp);
                    gp_Pnt c = gp.CentreOfMass();
                    m_moveFaceP0 = m_moveFacePivot = glm::vec3(c.X(), c.Y(), c.Z());
                } catch (...) { m_moveFaceP0 = m_moveFacePivot = glm::vec3(0.0f); }
                glm::vec3 N = m_moveFaceN;
                glm::vec3 ref = (std::abs(N.x) < 0.9f) ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
                glm::vec3 A = ref - glm::dot(ref, N) * N;
                if (glm::length(A) < 1e-5f) { ref = glm::vec3(0,0,1); A = ref - glm::dot(ref, N) * N; }
                m_moveFaceAxisA = glm::normalize(A);
                m_moveFaceAxisB = glm::normalize(glm::cross(N, m_moveFaceAxisA));
                m_moveFaceGrab = -1;
                m_moveFaceHalfExtent = 1.0f;
                // Move highlight: the hole's top rim, sampled as a world-space
                // polyline in loop[0] so the existing yellow-silhouette renderer
                // draws it following the drag (m_moveFaceMoveOuter → loop[0]
                // translates by the move vector). No hole sub-loops.
                m_moveFaceSilhouetteLoops.clear();
                m_moveFaceHoleSlant.clear();
                m_moveFaceHoleVertical.clear();
                m_moveFaceMoveOuter = true;
                m_moveFacePendingRebuild = false;
                if (!rim.IsNull()) {
                    std::vector<glm::vec3> pts;
                    for (BRepTools_WireExplorer we(rim); we.More(); we.Next()) {
                        BRepAdaptor_Curve crv(we.Current());
                        double f = crv.FirstParameter(), l = crv.LastParameter();
                        if (we.Current().Orientation() == TopAbs_REVERSED) std::swap(f, l);
                        const int Nseg = 16;
                        for (int i = 0; i < Nseg; ++i) {
                            gp_Pnt p = crv.Value(f + (l - f) * (double(i) / Nseg));
                            pts.emplace_back(p.X(), p.Y(), p.Z());
                        }
                    }
                    if (!pts.empty()) m_moveFaceSilhouetteLoops.push_back(pts);
                }
                m_moveFaceActive = true;
                return;
            }
            if (pocket) {
                showToast("Only simple through-holes can be moved for now "
                          "\xE2\x80\x94 not pockets, countersunk, or stepped holes.");
                return;
            }
        }
    }

    // Sort the selection: the first PLANAR face slides (the moving face); every
    // OTHER selected face is a candidate hole WALL (move that hole as a straight
    // tube); selected EDGES are hole top rings (slant). Walls are matched to
    // hole loops by shared edges below — NOT by surface type, because after any
    // face op the wall is a ruled loft surface, not an analytic cylinder.
    std::vector<TopoDS_Face> selectedFaces;
    std::vector<TopoDS_Edge> selectedEdges;
    for (const auto& e : m_selection->getSelection()) {
        if (e.shape.IsNull()) continue;
        if (e.type == SelectionType::Face) {
            TopoDS_Face f = TopoDS::Face(e.shape);
            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            if (m_moveFaceFace.IsNull() && !s.IsNull() &&
                s->IsKind(STANDARD_TYPE(Geom_Plane))) {
                m_moveFaceBodyId = e.bodyId;
                m_moveFaceFace = f;
            } else {
                selectedFaces.push_back(f); // potential hole wall
            }
        } else if (e.type == SelectionType::Edge) {
            selectedEdges.push_back(TopoDS::Edge(e.shape));
        }
    }
    if (m_moveFaceBodyId < 0 || m_moveFaceFace.IsNull()) return;
    // Drop the chosen moving face from the wall candidates if it slipped in.
    std::vector<TopoDS_Face> selectedCylinders;
    for (const auto& f : selectedFaces)
        if (!f.IsSame(m_moveFaceFace)) selectedCylinders.push_back(f);

    // Move Face only makes sense on a FLAT face (the shear pins one plane and
    // slides another). A curved face (cylinder side, fillet, sphere) has no
    // single plane to slide, so refuse with guidance instead of shearing junk.
    {
        Handle(Geom_Surface) surf = BRep_Tool::Surface(m_moveFaceFace);
        if (surf.IsNull() || !surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
            std::fprintf(stderr, "[MoveFace] declined: select a FLAT face\n");
            showToast("Move Face needs a flat face - pick a planar face.");
            return;
        }
    }

    try { m_moveFacePreviousShape = m_document->getBody(m_moveFaceBodyId); }
    catch (...) { return; }

    // (The loft rebuild now lofts the outer loop AND subtracts a loft of each
    // hole loop, so holed faces are allowed. Freeform / boolean bodies that
    // crashed the old shear are handled safely too: the op only lofts local
    // wires and refuses gracefully on release if the body isn't a clean prism —
    // no crash.)

    // Face plane (orientation-corrected outward normal + a point on it).
    try {
        BRepGProp_Face prop(m_moveFaceFace);
        double u1, u2, v1, v2;
        prop.Bounds(u1, u2, v1, v2);
        gp_Pnt c; gp_Vec n;
        prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, c, n);
        if (n.Magnitude() < 1e-9) return;
        n.Normalize();
        m_moveFaceP0 = glm::vec3(c.X(), c.Y(), c.Z());
        m_moveFaceN  = glm::vec3(n.X(), n.Y(), n.Z());
        // Pivot for Rotate/Scale = the face's area centroid (its "middle").
        GProp_GProps gp; BRepGProp::SurfaceProperties(m_moveFaceFace, gp);
        gp_Pnt ctr = gp.CentreOfMass();
        m_moveFacePivot = glm::vec3(ctr.X(), ctr.Y(), ctr.Z());
    } catch (...) { return; }

    // Two in-plane arrow axes: project the world axis least aligned with N into
    // the face plane → A; B = N × A. A box top gets clean world-aligned arrows.
    {
        glm::vec3 N = m_moveFaceN;
        glm::vec3 ref = (std::abs(N.x) < 0.9f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 A = ref - glm::dot(ref, N) * N;
        if (glm::length(A) < 1e-5f) {
            ref = glm::vec3(0, 0, 1);
            A = ref - glm::dot(ref, N) * N;
        }
        m_moveFaceAxisA = glm::normalize(A);
        m_moveFaceAxisB = glm::normalize(glm::cross(N, m_moveFaceAxisA));
    }
    m_moveFaceGrab = -1;
    m_moveFaceRotAxis = m_moveFaceAxisB; // default tilt axis until a ring is grabbed

    // Sketches sitting ON this face slide along with it. Coincident = plane
    // parallel to the face AND lying on it (same offset). Snapshot their planes
    // so the live preview / cancel can restore them.
    m_moveFaceSketchIds.clear();
    m_moveFaceSketchPlanes0.clear();
    {
        gp_Vec fN(m_moveFaceN.x, m_moveFaceN.y, m_moveFaceN.z);
        gp_Pnt fP(m_moveFaceP0.x, m_moveFaceP0.y, m_moveFaceP0.z);
        for (int sid : m_document->getAllSketchIds()) {
            auto sk = m_document->getSketch(sid);
            if (!sk) continue;
            const gp_Pln& sp = sk->getPlane();
            gp_Vec sN(sp.Axis().Direction());
            if (std::abs(sN.Dot(fN)) < 0.999) continue; // not parallel
            gp_Vec d(sp.Location().X() - fP.X(), sp.Location().Y() - fP.Y(),
                     sp.Location().Z() - fP.Z());
            if (std::abs(d.Dot(fN)) > 0.05) continue;   // not on the face plane
            m_moveFaceSketchIds.push_back(sid);
            m_moveFaceSketchPlanes0.push_back(sp);
        }
    }

    // Each loop of the face (outer outline first, then hole loops) captured as a
    // world-space polyline for the drag-time ghost, plus a per-hole "vertical"
    // flag (default false = slants). Loop order MUST match the op's enumeration
    // (OuterWire, then TopExp wires) so flags + ghost line up.
    m_moveFaceSilhouetteLoops.clear();
    m_moveFaceHoleSlant.clear();
    m_moveFaceHoleVertical.clear();
    m_moveFaceMoveOuter = true; // a planar face is selected → the outline slides
    m_moveFacePendingRebuild = false;
    std::vector<TopoDS_Wire> innerWires;
    try {
        // Walk edges in CONNECTED order (WireExplorer) so the polyline doesn't
        // zig-zag into a bowtie the way TopExp_Explorer's arbitrary order did.
        auto sampleWire = [](const TopoDS_Wire& w) {
            std::vector<glm::vec3> pts;
            for (BRepTools_WireExplorer we(w); we.More(); we.Next()) {
                const TopoDS_Edge& e = we.Current();
                BRepAdaptor_Curve crv(e);
                double f = crv.FirstParameter(), l = crv.LastParameter();
                if (e.Orientation() == TopAbs_REVERSED) std::swap(f, l);
                const int Nseg = 12;
                for (int i = 0; i < Nseg; ++i) {
                    gp_Pnt p = crv.Value(f + (l - f) * (double(i) / Nseg));
                    pts.emplace_back(p.X(), p.Y(), p.Z());
                }
            }
            return pts;
        };
        TopoDS_Wire outer = BRepTools::OuterWire(m_moveFaceFace);
        if (!outer.IsNull())
            m_moveFaceSilhouetteLoops.push_back(sampleWire(outer));
        for (TopExp_Explorer wx(m_moveFaceFace, TopAbs_WIRE); wx.More(); wx.Next()) {
            TopoDS_Wire w = TopoDS::Wire(wx.Current());
            if (w.IsSame(outer)) continue;
            m_moveFaceSilhouetteLoops.push_back(sampleWire(w));
            m_moveFaceHoleSlant.push_back(false);    // stays put until opted in
            m_moveFaceHoleVertical.push_back(false);
            innerWires.push_back(w);
        }

        // hole index whose inner wire contains the given edge, else -1.
        auto holeOfEdge = [&](const TopoDS_Edge& edge) -> int {
            for (size_t hi = 0; hi < innerWires.size(); ++hi)
                for (TopExp_Explorer we(innerWires[hi], TopAbs_EDGE); we.More(); we.Next())
                    if (edge.IsSame(we.Current())) return static_cast<int>(hi);
            return -1;
        };
        // Cylinder wall picked → that hole moves as a straight tube (vertical).
        for (const TopoDS_Face& cyl : selectedCylinders) {
            for (TopExp_Explorer ce(cyl, TopAbs_EDGE); ce.More(); ce.Next()) {
                int hi = holeOfEdge(TopoDS::Edge(ce.Current()));
                if (hi >= 0) { m_moveFaceHoleVertical[hi] = true; break; }
            }
        }
        // Hole top edge picked → that hole slants (top ring follows).
        for (const TopoDS_Edge& edge : selectedEdges) {
            int hi = holeOfEdge(edge);
            if (hi >= 0) m_moveFaceHoleSlant[hi] = true;
        }
    } catch (...) {
        m_moveFaceSilhouetteLoops.clear();
        m_moveFaceHoleSlant.clear();
        m_moveFaceHoleVertical.clear();
    }

    // Face half-extent (max distance pivot→outline) so a drag of ~that length
    // maps to ≈1 rad of tilt / a unit of scale — a size-independent feel.
    m_moveFaceHalfExtent = 1.0f;
    if (!m_moveFaceSilhouetteLoops.empty()) {
        float mx = 0.0f;
        for (const auto& p : m_moveFaceSilhouetteLoops[0])
            mx = std::max(mx, glm::length(p - m_moveFacePivot));
        if (mx > 1e-3f) m_moveFaceHalfExtent = mx;
    }

    // Hollow (shelled) body: the per-frame preview refuses (the loft engine
    // can't shear a cavity), so the body won't follow the drag — but the
    // commit reflows beneath the Shell and lands correctly. Say so up front
    // instead of looking broken.
    if (m_history && m_history->isBodyShelled(m_moveFaceBodyId))
        showToast("Hollow body: the preview stays put \xE2\x80\x94 the change "
                  "applies when you release (re-shelled automatically).");

    m_moveFaceActive = true;
}

// Restore the on-face sketches to their snapshot planes, then slide them by
// `v` (so the live preview never compounds). v = (0,0,0) just restores.
void Application::moveFaceSlideSketches(const glm::vec3& v) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(v.x, v.y, v.z));
    for (size_t i = 0; i < m_moveFaceSketchIds.size(); ++i) {
        if (auto sk = m_document->getSketch(m_moveFaceSketchIds[i])) {
            gp_Pln p = m_moveFaceSketchPlanes0[i];
            if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f) p.Transform(t);
            sk->setPlane(p);
        }
    }
}

namespace {
// Rotation matrix (column-major glm) about a unit-ish axis by `angle` radians.
glm::mat3 rodrigues(const glm::vec3& axisIn, float angle) {
    glm::vec3 a = glm::normalize(axisIn);
    float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    float ax = a.x, ay = a.y, az = a.z;
    return glm::mat3(
        glm::vec3(c + ax*ax*t,      ay*ax*t + az*s,  az*ax*t - ay*s),   // col 0
        glm::vec3(ax*ay*t - az*s,   c + ay*ay*t,     az*ay*t + ax*s),   // col 1
        glm::vec3(ax*az*t + ay*s,   ay*az*t - ax*s,  c + az*az*t));     // col 2
}
} // namespace

glm::mat3 Application::faceRotTotal() const {
    return rodrigues(m_moveFaceRotAxis, m_moveFaceAngle) * m_moveFaceRotAccum;
}

// Bake the just-released ring drag into the accumulated tilt (so the next ring
// drag stacks on top), then reset the live angle.
void Application::bakeFaceRotationDrag() {
    // Twist isn't a tilt-matrix accumulation — nothing to bake for it.
    if (m_moveFaceIsTwist) return;
    if (m_faceXformKind != FaceXform::Rotate || std::abs(m_moveFaceAngle) < 1e-5f)
        return;
    m_moveFaceRotAccum = rodrigues(m_moveFaceRotAxis, m_moveFaceAngle) * m_moveFaceRotAccum;
    m_moveFaceRotHasAccum = true;
    m_moveFaceAngle = 0.0f;
    m_moveFaceAngleBase = 0.0f;
}

// Configure an op with the current gesture (Move / Rotate / Scale) + hole flags.
void Application::configureFaceOp(MoveFaceOp& op) const {
    switch (m_faceXformKind) {
        case FaceXform::Translate:
            op.setKind(MoveFaceOp::Kind::Translate);
            op.setMoveVector(gp_Vec(m_moveFaceVec.x, m_moveFaceVec.y, m_moveFaceVec.z));
            break;
        case FaceXform::Rotate: {
            if (m_moveFaceIsTwist) { // third ring = twist about the normal
                op.setKind(MoveFaceOp::Kind::Twist);
                op.setTwist(m_moveFaceTwist);
                break;
            }
            op.setKind(MoveFaceOp::Kind::Rotate);
            // Composed rotation (live drag ∘ accumulated tilts) as a gp_Trsf
            // about the pivot, so stacked tilts about both axes apply at once.
            glm::mat3 R = faceRotTotal();
            glm::vec3 Tt = m_moveFacePivot - R * m_moveFacePivot;
            gp_Trsf trsf;
            trsf.SetValues(R[0][0], R[1][0], R[2][0], Tt.x,
                           R[0][1], R[1][1], R[2][1], Tt.y,
                           R[0][2], R[1][2], R[2][2], Tt.z);
            op.setRotationExplicit(trsf);
            break;
        }
        case FaceXform::Scale:
            op.setKind(MoveFaceOp::Kind::Scale);
            if (m_moveFaceScaleUniform) {
                op.setScaleFactor(m_moveFaceScale);
            } else {
                op.setScaleNonUniform(
                    gp_Dir(m_moveFaceAxisA.x, m_moveFaceAxisA.y, m_moveFaceAxisA.z),
                    gp_Dir(m_moveFaceAxisB.x, m_moveFaceAxisB.y, m_moveFaceAxisB.z),
                    m_moveFaceScaleA, m_moveFaceScaleB);
            }
            break;
    }
    op.setLoopMotion(m_moveFaceMoveOuter, m_moveFaceHoleSlant, m_moveFaceHoleVertical);
}

bool Application::faceXformNontrivial() const {
    switch (m_faceXformKind) {
        case FaceXform::Translate: return glm::length(m_moveFaceVec) > 1e-4f;
        case FaceXform::Rotate:
            return m_moveFaceIsTwist
                ? std::abs(m_moveFaceTwist) > 1e-4f
                : (std::abs(m_moveFaceAngle) > 1e-4f || m_moveFaceRotHasAccum);
        case FaceXform::Scale:
            return m_moveFaceScaleUniform
                ? std::abs(m_moveFaceScale - 1.0f) > 1e-4f
                : (std::abs(m_moveFaceScaleA - 1.0f) > 1e-4f ||
                   std::abs(m_moveFaceScaleB - 1.0f) > 1e-4f);
    }
    return false;
}

void Application::updateMoveFace() {
    if (!m_moveFaceActive || m_moveFaceBodyId < 0) return;

    // Hole-move preview: re-cut the hole at the dragged position each frame.
    if (m_moveHoleMode) {
        m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
        m_meshesDirty = true;
        gp_Vec mv(m_moveFaceVec.x, m_moveFaceVec.y, m_moveFaceVec.z);
        if (mv.Magnitude() < 1e-9) return;
        try {
            MoveHoleOp op;
            op.setBody(m_moveFaceBodyId);
            op.setSeedWall(m_moveHoleWall);
            op.setMoveVector(mv);
            if (!op.execute(*m_document))
                m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
            m_meshesDirty = true;
        } catch (...) {
            m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
        }
        return;
    }

    // Always preview from the original snapshot so transforms don't compound.
    m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
    m_meshesDirty = true;
    if (!faceXformNontrivial()) { moveFaceSlideSketches(glm::vec3(0.0f)); return; }
    try {
        auto op = std::make_unique<MoveFaceOp>();
        op->setBody(m_moveFaceBodyId);
        op->setFace(m_moveFaceFace);
        configureFaceOp(*op);
        if (!op->execute(*m_document))
            m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
        // Sketch follow in the preview is translate-only for now (rotate/scale
        // sketches still follow on commit via the op's own transform).
        if (m_faceXformKind == FaceXform::Translate) moveFaceSlideSketches(m_moveFaceVec);
        m_meshesDirty = true;
    } catch (...) {
        m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
    }
}

void Application::commitMoveFace() {
    if (!m_moveFaceActive) { return; }

    // Hole-move commit: restore the snapshot, then push one MoveHoleOp.
    if (m_moveHoleMode) {
        if (m_moveFaceBodyId >= 0 && !m_moveFacePreviousShape.IsNull())
            m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
        gp_Vec mv(m_moveFaceVec.x, m_moveFaceVec.y, m_moveFaceVec.z);
        if (mv.Magnitude() > 1e-9 && m_moveFaceBodyId >= 0 && !m_moveHoleWall.IsNull()) {
            auto op = std::make_unique<MoveHoleOp>();
            op->setBody(m_moveFaceBodyId);
            op->setSeedWall(m_moveHoleWall);
            op->setMoveVector(mv);
            if (m_history->pushOperation(std::move(op), *m_document))
                std::fprintf(stdout, "Hole move committed\n");
        }
        // The wall face identity changed; clear selection rather than chase it.
        if (m_selection) m_selection->clear();
        m_moveHoleMode = false;
        m_moveFaceActive = false;
        m_moveHoleWall.Nullify();
        m_meshesDirty = true;
        return;
    }

    // Restore the original body + sketch planes before the real op runs (it
    // snapshots the body from the doc and re-applies the slide atomically).
    if (m_moveFaceBodyId >= 0 && !m_moveFacePreviousShape.IsNull())
        m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
    moveFaceSlideSketches(glm::vec3(0.0f)); // restore sketches to snapshot

    bool committed = false;
    if (faceXformNontrivial() && m_moveFaceBodyId >= 0 && !m_moveFaceFace.IsNull()) {
        auto op = std::make_unique<MoveFaceOp>();
        op->setBody(m_moveFaceBodyId);
        op->setFace(m_moveFaceFace);
        configureFaceOp(*op);
        op->setSketchIds(m_moveFaceSketchIds); // on-face sketches ride along
        committed = m_history->pushOperation(std::move(op), *m_document);
        if (committed)
            std::fprintf(stdout, "Face %s committed\n",
                         (m_faceXformKind == FaceXform::Rotate && m_moveFaceIsTwist) ? "twist"
                         : m_faceXformKind == FaceXform::Rotate ? "tilt"
                         : m_faceXformKind == FaceXform::Scale ? "scale" : "move");
    }

    // Re-select the moved face in the REBUILT body, so the highlight + the next
    // op use live geometry. Without this the selection keeps the stale old-
    // position face: the highlight lingers there, and a chained op lofts from
    // that old wire (lands the body back where it started = "the op got undone").
    if (committed && m_selection) {
        glm::vec3 want = m_moveFacePivot; // where the face centre ends up
        if (m_faceXformKind == FaceXform::Translate) want += m_moveFaceVec;
        TopoDS_Shape nb = m_document->getBody(m_moveFaceBodyId);
        TopoDS_Face best; double bestD = 1e300;
        for (TopExp_Explorer fx(nb, TopAbs_FACE); fx.More(); fx.Next()) {
            TopoDS_Face f = TopoDS::Face(fx.Current());
            Handle(Geom_Surface) s = BRep_Tool::Surface(f);
            if (s.IsNull() || !s->IsKind(STANDARD_TYPE(Geom_Plane))) continue;
            try {
                GProp_GProps gp; BRepGProp::SurfaceProperties(f, gp);
                gp_Pnt c = gp.CentreOfMass();
                double d = glm::length(glm::vec3(c.X(), c.Y(), c.Z()) - want);
                if (d < bestD) { bestD = d; best = f; }
            } catch (...) {}
        }
        if (!best.IsNull()) {
            SelectionEntry entry;
            entry.type = SelectionType::Face;
            entry.bodyId = m_moveFaceBodyId;
            entry.shape = best;
            m_selection->select(entry);
        } else {
            m_selection->clear(); // fall back to clearing if we can't re-find it
        }
    }
    m_moveFaceSketchIds.clear();
    m_moveFaceSketchPlanes0.clear();
    m_moveFaceActive = false;
    m_moveHoleMode = false;
    m_moveHoleWall.Nullify();
    m_moveFaceBodyId = -1;
    m_moveFaceFace.Nullify();
    m_moveFacePreviousShape.Nullify();
    m_moveFaceVec = glm::vec3(0.0f);
    m_moveFaceBase = glm::vec3(0.0f);
    m_moveFaceDragging = false;
    m_moveFaceSilhouetteLoops.clear();
    m_moveFaceHoleSlant.clear();
    m_moveFaceHoleVertical.clear();
    m_moveFaceMoveOuter = true;
    m_moveFacePendingRebuild = false;
    m_meshesDirty = true;
}

void Application::cancelMoveFace() {
    if (!m_moveFaceActive) return;
    if (m_moveFaceBodyId >= 0 && !m_moveFacePreviousShape.IsNull())
        m_document->updateBody(m_moveFaceBodyId, m_moveFacePreviousShape);
    moveFaceSlideSketches(glm::vec3(0.0f)); // restore sketches to snapshot
    m_moveFaceSketchIds.clear();
    m_moveFaceSketchPlanes0.clear();
    m_moveFaceActive = false;
    m_moveHoleMode = false;
    m_moveHoleWall.Nullify();
    m_moveFaceBodyId = -1;
    m_moveFaceFace.Nullify();
    m_moveFacePreviousShape.Nullify();
    m_moveFaceVec = glm::vec3(0.0f);
    m_moveFaceBase = glm::vec3(0.0f);
    m_moveFaceDragging = false;
    m_moveFaceSilhouetteLoops.clear();
    m_moveFaceHoleSlant.clear();
    m_moveFaceHoleVertical.clear();
    m_moveFaceMoveOuter = true;
    m_moveFacePendingRebuild = false;
    m_meshesDirty = true;
}

// ─── User Z-up axis convention helpers ─────────────────────────────────────
//
// The world stays Y-up internally (camera, transforms, viewcube), but the
// user-facing axis radios in tools like Pattern and Split read with 3D-
// printer convention: X = left/right, Y = forward/back, Z = up. So when
// the user picks "Z" we drive the rotation around the world Y axis, etc.
// Keeping the mapping in one place means the toolbar can label its radios
// in user terms while the modeling ops keep getting world-space vectors.

glm::vec3 Application::userAxisToWorldVec(int userIdx) {
    return materializr::userAxisToWorldVec(userIdx); // shared: UserAxes.h
}

int Application::userAxisToWorldIdx(int userIdx) {
    return materializr::userAxisToWorldIdx(userIdx);
}

// ─── Interactive Pattern (Linear / Radial) ─────────────────────────────────
//
// User clicks Linear Pattern or Radial Pattern in the toolbar (PatternPlugin
// fires a requestInteractiveOp via PluginContext, which the main frame loop
// picks up and dispatches to beginPattern). The popup lets the user adjust
// axis / count / spacing (linear) or axis / count / angle / origin (radial)
// with a live preview. Each parameter change re-pushes a PatternOp onto
// history via updatePattern; commit leaves the op there, cancel undoes it.

void Application::beginPattern(PatternKind kind) {
    cancelActiveIops();
    // Find the first selected body. PatternOp clones one source body.
    int bodyId = -1;
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Body && e.bodyId >= 0) {
                bodyId = e.bodyId; break;
            }
        }
    }
    if (bodyId < 0) return;

    m_patternKind   = kind;
    m_patternBodyId = bodyId;
    m_patternAxisIdx = (kind == PatternKind::Linear) ? 0 : 2; // default X for linear, Z for radial
    m_patternAxisId = -1; // start on a world axis; user can pick a construction axis
    m_patternCount    = (kind == PatternKind::Linear) ? 3   : 6;
    m_patternDistance = 5.0f;
    m_patternAngle    = 360.0f;
    // Default radial origin = body's bbox centre — that puts the rotation
    // axis through the body the first time so the user sees a sensible
    // ring immediately and can re-pick the origin via the viewport.
    m_patternOriginX = m_patternOriginY = m_patternOriginZ = 0.0f;
    try {
        Bnd_Box bb;
        BRepBndLib::Add(m_document->getBody(bodyId), bb);
        if (!bb.IsVoid()) {
            double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
            m_patternOriginX = static_cast<float>((x0 + x1) * 0.5);
            m_patternOriginY = static_cast<float>((y0 + y1) * 0.5);
            m_patternOriginZ = static_cast<float>((z0 + z1) * 0.5);
        }
    } catch (...) {}
    m_patternPickingOrigin = false;
    m_patternPreviewPushed = false;
    m_patternInputFocus    = true;
    std::snprintf(m_patternCountBuf,    sizeof(m_patternCountBuf),    "%d", m_patternCount);
    std::snprintf(m_patternDistanceBuf, sizeof(m_patternDistanceBuf), "%.2f", m_patternDistance);
    std::snprintf(m_patternAngleBuf,    sizeof(m_patternAngleBuf),    "%.1f", m_patternAngle);
    m_patternActive = true;

    updatePattern(); // initial preview
}

void Application::updatePattern() {
    if (!m_patternActive || m_patternBodyId < 0) return;

    // Undo the previous preview op (if any) before pushing the new one — keeps
    // history clean so the eventual commit leaves exactly one PatternOp.
    if (m_patternPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_patternPreviewPushed = false;
    }
    if (m_patternCount < 2) {
        // Nothing to preview at count=1 (a pattern of 1 is just the source).
        m_meshesDirty = true;
        return;
    }

    auto op = std::make_unique<PatternOp>();
    op->setBody(m_patternBodyId);

    // Axis direction comes from the chosen world axis, or — when the user
    // picked a construction axis from the dropdown — from that axis's own
    // direction (and, for radial, its origin too, since the axis defines the
    // full rotation line).
    glm::vec3 axisDir = userAxisToWorldVec(m_patternAxisIdx);
    float originX = m_patternOriginX, originY = m_patternOriginY, originZ = m_patternOriginZ;
    if (m_patternAxisId >= 0) {
        if (const auto* a = m_document->getAxis(m_patternAxisId)) {
            axisDir = glm::vec3((float)a->direction.X(),
                                (float)a->direction.Y(),
                                (float)a->direction.Z());
            originX = (float)a->origin.X();
            originY = (float)a->origin.Y();
            originZ = (float)a->origin.Z();
        }
    }

    if (m_patternKind == PatternKind::Linear) {
        op->setType(PatternType::Linear);
        op->setCount(m_patternCount);
        op->setLinearSpacing(axisDir.x * m_patternDistance,
                             axisDir.y * m_patternDistance,
                             axisDir.z * m_patternDistance);
    } else {
        op->setType(PatternType::Radial);
        op->setCount(m_patternCount);
        op->setRadialAxis(axisDir.x, axisDir.y, axisDir.z);
        op->setRadialOrigin(originX, originY, originZ);
        op->setTotalAngle(m_patternAngle);
    }
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_patternPreviewPushed = true;
    }
    m_meshesDirty = true;
}

void Application::commitPattern() {
    // The preview op IS the final op — leave it on history at the current
    // values. Just clear the popup state.
    m_patternActive        = false;
    m_patternPickingOrigin = false;
    m_patternPreviewPushed = false;
    m_patternBodyId        = -1;
    m_meshesDirty = true;
}

void Application::cancelPattern() {
    if (m_patternPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_patternPreviewPushed = false;
    }
    m_patternActive        = false;
    m_patternPickingOrigin = false;
    m_patternBodyId        = -1;
    m_meshesDirty = true;
}

// ─── Loft (interactive popup) ──────────────────────────────────────────────
//
// LoftPlugin walks the selection and, when 2+ distinct sketches are present,
// fires requestInteractiveOp("Loft"). The main frame loop dispatches that to
// beginLoft(), which snapshots the two profile wires (the outer wire of each
// sketch's first region) and opens the popup. updateLoft re-pushes a preview
// LoftOp each frame the user changes a toggle, commitLoft leaves the final
// op on history, cancelLoft undoes the preview.

void Application::beginLoft() {
    if (!m_selection || !m_document) return;

    // Snapshot the first two distinct sketches in click order.
    std::vector<int> sketchIds;
    auto addId = [&](int id) {
        if (id < 0) return;
        for (int x : sketchIds) if (x == id) return;
        sketchIds.push_back(id);
    };
    for (const auto& e : m_selection->getSelection()) {
        if ((e.type == SelectionType::Sketch ||
             e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
            addId(e.sketchId);
        }
    }
    if (sketchIds.size() < 2) return;

    auto wireFromSketch = [&](int id, std::vector<TopoDS_Wire>& holesOut) -> TopoDS_Wire {
        holesOut.clear();
        auto sk = m_document->getSketch(id);
        if (!sk) return {};
        auto regions = sk->buildRegions();
        if (!regions.empty()) {
            // Concentric profiles decompose into MULTIPLE regions (the ring
            // AND the inner disk). Loft the outermost one — largest outer
            // bbox — so its holes become the tube channel; blindly taking
            // regions[0] could grab the inner disk and loft a solid cone.
            size_t best = 0;
            double bestDiag = -1.0;
            for (size_t i = 0; i < regions.size(); ++i) {
                if (regions[i].outerWire.IsNull()) continue;
                Bnd_Box bb;
                BRepBndLib::Add(regions[i].outerWire, bb);
                if (bb.IsVoid()) continue;
                double x0, y0, z0, x1, y1, z1;
                bb.Get(x0, y0, z0, x1, y1, z1);
                double dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
                double diag = dx * dx + dy * dy + dz * dz;
                if (diag > bestDiag) { bestDiag = diag; best = i; }
            }
            holesOut = regions[best].holeWires; // inner boundaries → tube channels
            return regions[best].outerWire;
        }
        auto wires = sk->buildWires();
        if (!wires.empty()) return wires[0];
        return {};
    };

    m_loftWireA = wireFromSketch(sketchIds[0], m_loftHolesA);
    m_loftWireB = wireFromSketch(sketchIds[1], m_loftHolesB);
    if (m_loftWireA.IsNull() || m_loftWireB.IsNull()) {
        std::fprintf(stderr,
            "[Loft] could not derive a closed wire from one of the "
            "selected sketches (need a closed region in each).\n");
        return;
    }

    m_loftSolid = true;
    m_loftRuled = false;
    m_loftReverseB = false;
    m_loftPreviewPushed = false;
    m_loftActive = true;

    updateLoft();
}

void Application::updateLoft() {
    if (!m_loftActive || !m_history || !m_document) return;
    if (m_loftWireA.IsNull() || m_loftWireB.IsNull()) return;

    // Undo previous preview so history accumulates exactly one LoftOp.
    if (m_loftPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_loftPreviewPushed = false;
    }

    auto op = std::make_unique<LoftOp>();
    op->addProfile(m_loftWireA, m_loftHolesA);
    // Reverse profile B's wire when requested: this re-orders B's vertices so
    // they pair differently against A's, which is the standard remedy for
    // the "apex pinch / pyramid" output when start vertices are misaligned.
    // Reverse B's hole wires to match, so inner channels pair consistently.
    if (m_loftReverseB) {
        std::vector<TopoDS_Wire> holesB;
        holesB.reserve(m_loftHolesB.size());
        for (const auto& h : m_loftHolesB) holesB.push_back(TopoDS::Wire(h.Reversed()));
        op->addProfile(TopoDS::Wire(m_loftWireB.Reversed()), holesB);
    } else {
        op->addProfile(m_loftWireB, m_loftHolesB);
    }
    op->setSolid(m_loftSolid);
    op->setRuled(m_loftRuled);
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_loftPreviewPushed = true;
    }
    m_meshesDirty = true;
}

void Application::commitLoft() {
    m_loftActive = false;
    m_loftPreviewPushed = false;
    m_loftWireA = TopoDS_Wire();
    m_loftWireB = TopoDS_Wire();
    m_meshesDirty = true;
}

// ─── Cascade: re-execute Extrudes that consumed a just-edited sketch ───────
//
// Triggered by SketchEditedEvent. Walks the live history forward and, for
// every enabled ExtrudeOp whose source sketch matches, rebuilds the profile
// from the current sketch state and re-runs execute() — which uses
// addOrPutBody under the hood, so the resulting body keeps the same id and
// the user sees its shape morph in place.
//
// We deliberately stop at Extrude. Downstream ops (Fillet, Chamfer, Pattern,
// Mirror, Push/Pull) reference faces / edges of the extruded body — when
// the body's topology shifts, those references go stale (the toponaming
// problem). Re-running them blindly would produce wrong-edge fillets or
// outright crashes. For the user's "edit a dimension and watch the prism
// follow" workflow this trade-off is fine: simple chains just work; chained
// workflows leave the downstream ops on the stale body and the user
// manually re-runs them.
const std::map<int, std::set<int>>& Application::sketchBodyLinks() const {
    // Memoized on the history revision — the Properties panel reads the link
    // hint every frame a body/sketch is selected, and this walk (dynamic_cast
    // + captureDiff per step, fresh map/set nodes) was running per frame.
    if (m_history && m_linkMapRevision == m_history->revision())
        return m_linkMapCache;
    std::map<int, std::set<int>>& links = m_linkMapCache;
    links.clear();
    if (!m_history) return links;
    m_linkMapRevision = m_history->revision();
    int n = m_history->stepCount();
    for (int i = 0; i < n; ++i) {
        const Operation* op = m_history->getStep(i);
        if (!op) continue;
        std::set<int> srcSketches;
        if (auto* ext = dynamic_cast<const ExtrudeOp*>(op)) {
            if (ext->getSketchId() >= 0) srcSketches.insert(ext->getSketchId());
        } else if (auto* pp = dynamic_cast<const PushPullOp*>(op)) {
            for (int t = 0; t < pp->targetCount(); ++t)
                if (pp->getSketchIdAt(t) >= 0) srcSketches.insert(pp->getSketchIdAt(t));
        }
        if (srcSketches.empty()) continue;
        // Bodies this step touched = created + modified.
        OperationDiff diff = op->captureDiff();
        std::set<int> bodies(diff.created.begin(), diff.created.end());
        for (const auto& [id, _] : diff.modifiedBefore) bodies.insert(id);
        for (int s : srcSketches)
            links[s].insert(bodies.begin(), bodies.end());
    }
    return links;
}

bool Application::bodySafelyRederivable(int bodyId, int viaSketchId) const {
    if (!m_history) return false;
    int n = m_history->stepCount();
    for (int i = 0; i < n; ++i) {
        const Operation* op = m_history->getStep(i);
        if (!op || !op->isEnabled()) continue;
        OperationDiff d = op->captureDiff();
        bool touches = false;
        for (int c : d.created) if (c == bodyId) { touches = true; break; }
        if (!touches)
            for (const auto& [id, _] : d.modifiedBefore)
                if (id == bodyId) { touches = true; break; }
        if (!touches) continue;
        // The only op allowed to touch this body is its own sketch's extrude /
        // push-pull. Anything else (fillet, chamfer, boolean, a second feature,
        // a transform) means re-deriving at a new position would break — not safe.
        if (auto* ext = dynamic_cast<const ExtrudeOp*>(op)) {
            if (ext->getSketchId() == viaSketchId) continue;
        } else if (auto* pp = dynamic_cast<const PushPullOp*>(op)) {
            bool fromSketch = false;
            for (int t = 0; t < pp->targetCount(); ++t)
                if (pp->getSketchIdAt(t) == viaSketchId) { fromSketch = true; break; }
            if (fromSketch) continue;
        }
        return false;
    }
    return true;
}

void Application::relinkSketch(bool isBody, int id) {
    if (!m_document) return;
    std::vector<int> sketches;
    if (isBody) {
        const auto& links = sketchBodyLinks();
        for (const auto& [sid, bodies] : links)
            if (bodies.count(id)) sketches.push_back(sid);
    } else {
        sketches.push_back(id);
    }
    bool changed = false;
    for (int sid : sketches)
        if (auto sk = m_document->getSketch(sid); sk && sk->isDetachedFromBody()) {
            sk->setDetachedFromBody(false);
            changed = true;
        }
    if (changed) {
        markDirty();
        m_meshesDirty = true;
        showToast("Sketch re-linked — editing it will drive the body again.");
    }
}

std::string Application::linkHintFor(bool isBody, int id) const {
    if (!m_document) return "";
    const auto& links = sketchBodyLinks();
    auto nameList = [&](const std::set<int>& ids, bool bodies) {
        std::string s;
        for (int v : ids) {
            if (!s.empty()) s += ", ";
            s += bodies ? m_document->getBodyName(v) : m_document->getSketchName(v);
        }
        return s;
    };
    if (isBody) {
        std::set<int> live, detached;
        for (const auto& [sid, bodyIds] : links) {
            if (!bodyIds.count(id)) continue;
            auto sk = m_document->getSketch(sid);
            (sk && sk->isDetachedFromBody() ? detached : live).insert(sid);
        }
        if (live.empty() && detached.empty()) return "";
        if (!live.empty())
            return "Built from " + nameList(live, false) +
                   " — editing it updates this body.";
        return "Detached from " + nameList(detached, false) +
               " — moved independently; sketch edits won't update this body.";
    }
    // Sketch: what body it drives + whether it's detached.
    auto it = links.find(id);
    if (it == links.end() || it->second.empty()) return "";
    auto sk = m_document->getSketch(id);
    std::string bodies = nameList(it->second, true);
    if (sk && sk->isDetachedFromBody())
        return "Detached — moved independently; edits won't update " + bodies + ".";
    return "Drives " + bodies + " — editing this sketch updates it.";
}

void Application::cascadeFromSketchEdit(int sketchId) {
    if (sketchId < 0 || !m_history || !m_document) return;
    // A detached sketch has been deliberately broken out of unison with its
    // body (moved on its own in 3D) — editing it must NOT retro-drive the body.
    if (auto sk = m_document->getSketch(sketchId); sk && sk->isDetachedFromBody())
        return;
    int n = m_history->stepCount();

    // Re-derive the profile of every sketch-sourced extrude / push-pull that
    // references the edited sketch, and remember the EARLIEST such step. We do
    // NOT execute them in isolation here — that used to overwrite the body with
    // just the bare extrude, discarding everything downstream (hollows,
    // fillets) and leaving a "cube with N holes". Instead we replay the whole
    // chain from the earliest affected step below.
    int earliest = -1, matched = 0;
    for (int i = 0; i < n; ++i) {
        Operation* op = const_cast<Operation*>(m_history->getStep(i));
        if (!op || !op->isEnabled()) continue;
        if (auto* ext = dynamic_cast<ExtrudeOp*>(op)) {
            if (ext->getSketchId() != sketchId) continue;
            ++matched;
            if (ext->rebuildProfileFromSketch(*m_document) && earliest < 0) earliest = i;
        } else if (auto* pp = dynamic_cast<PushPullOp*>(op)) {
            bool refs = false;
            int tc = pp->targetCount();
            for (int t = 0; t < tc; ++t)
                if (pp->getSketchIdAt(t) == sketchId) { refs = true; break; }
            if (!refs) continue;
            ++matched;
            if (pp->rebuildProfileFromSketch(*m_document, sketchId) && earliest < 0)
                earliest = i;
        }
    }
    if (earliest < 0) {
        std::fprintf(stderr, "[Cascade] sketchId=%d: %d matched, none re-derivable\n",
                     sketchId, matched);
        if (matched > 0) {
            // A body IS driven by this sketch, but its profile couldn't be
            // re-derived from the new geometry — tell the user instead of
            // silently leaving the sketch changed and the body stale.
            showToast("Updated the sketch, but the body built from it couldn't "
                      "rebuild from the new shape \xE2\x80\x94 the model is unchanged.");
        }
        // matched == 0: nothing in the model is built from this sketch (e.g.
        // editing a freshly-duplicated sketch before it's extruded). That's the
        // normal case while sketching — stay silent, just refresh.
        m_meshesDirty = true;
        return;
    }

    // Replay the chain from the earliest re-derived op forward, TRANSACTIONALLY:
    // the new profiles take effect and ALL downstream ops re-run on the updated
    // geometry. If any can't follow (e.g. a fillet whose edge no longer exists
    // after the change), the entire model is restored — never half-built.
    //
    // Snapshot every body's shape BEFORE the replay so we can re-tessellate
    // ONLY the bodies it actually changes — not the whole scene. On a multi-
    // body project a sketch edit touching one body would otherwise re-mesh
    // every body, which is the dominant cost on a tablet. A re-executed op
    // hands its bodies a fresh TShape, so IsEqual cleanly separates changed
    // bodies from untouched ones; created/removed ids are covered below.
    std::map<int, TopoDS_Shape> beforeBodies;
    for (int id : m_document->getAllBodyIds())
        beforeBodies[id] = m_document->getBody(id);

    // Pin the edited sketch's FINAL state for the replay: re-executing the
    // chain rolls the live sketch back through its SketchEditOp snapshots, so
    // mid-replay it holds a STALE state — while the extrude below was rebuilt
    // from the final one. A fillet/chamfer re-finding its edges from "the
    // sketch the user just edited" (generative anchors) must read the final
    // state or it looks for the old geometry and fails every time.
    if (auto sk = m_document->getSketch(sketchId))
        m_document->setCascadeSketchOverride(
            sketchId, std::make_shared<materializr::Sketch>(*sk));
    bool ok = m_history->editStep(earliest, *m_document, /*transactional=*/true);
    m_document->clearCascadeSketchOverrides();
    std::fprintf(stderr, "[Cascade] sketchId=%d replay from step %d: %s\n",
                 sketchId, earliest, ok ? "applied" : "reverted");
    if (!ok) {
        showToast("Couldn't update the model for that sketch change \xE2\x80\x94 a "
                  "downstream feature (e.g. a fillet) couldn't follow it, so the "
                  "model was left unchanged.");
    }

    // Partial remesh: mark only bodies whose shape changed, plus any that were
    // created or removed. On a failed (reverted) replay every body is restored
    // to its snapshot TShape, so nothing is marked — no needless remesh at all.
    std::set<int> nowIds;
    for (int id : m_document->getAllBodyIds()) {
        nowIds.insert(id);
        auto it = beforeBodies.find(id);
        if (it == beforeBodies.end() ||
            !it->second.IsEqual(m_document->getBody(id)))
            m_dirtyBodyIds.insert(id);            // changed or newly created
    }
    for (const auto& [id, shp] : beforeBodies)
        if (!nowIds.count(id)) m_dirtyBodyIds.insert(id); // removed → mesh cleared
}

void Application::cancelLoft() {
    if (m_loftPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_loftPreviewPushed = false;
    }
    m_loftActive = false;
    m_loftWireA = TopoDS_Wire();
    m_loftWireB = TopoDS_Wire();
    m_meshesDirty = true;
}

// ─── Construction Plane (interactive popup) ────────────────────────────────
//
// Same architecture as Loft: ConstructionPlanePlugin fires a plain
// requestInteractiveOp("ConstructionPlane"). Application reads the current
// selection (a planar face unlocks the "Parallel to face" option), opens a
// live-previewed popup with XY/XZ/YZ + offset, then commits a single
// ConstructionPlaneOp on Apply or undoes the preview on Cancel.

void Application::beginConstructionPlane() {
    if (!m_history || !m_document) return;

    m_planeOpHaveFace = false;
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Face && !e.shape.IsNull() &&
                e.shape.ShapeType() == TopAbs_FACE) {
                try {
                    TopoDS_Face f = TopoDS::Face(e.shape);
                    Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
                    if (!surf.IsNull() && surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
                        m_planeOpBaseFace = Handle(Geom_Plane)::DownCast(surf)->Pln();
                        m_planeOpHaveFace = true;
                        break;
                    }
                } catch (...) {}
            }
        }
    }

    // Gather inputs for the derived modes (Midplane / Normal-to-Axis /
    // Tangent-to-Cylinder) from the selection in one pass.
    m_planeOpHaveTwoPlanes = false;
    m_planeOpHaveAxis = false;
    m_planeOpHaveCylinder = false;
    {
        std::vector<gp_Pln> planarPlanes;        // planar faces + construction planes
        std::vector<gp_Ax1> axes;                // construction axes + straight edges
        struct CylInfo { gp_Ax1 axis; double radius; };
        std::vector<CylInfo> cylinders;
        std::vector<gp_Pnt> vertices;
        if (m_selection) {
            for (const auto& e : m_selection->getSelection()) {
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    if (const auto* pe = m_document->getPlane(e.planeId))
                        planarPlanes.push_back(pe->plane);
                    continue;
                }
                if (e.type == SelectionType::Axis && e.axisId >= 0) {
                    if (const auto* a = m_document->getAxis(e.axisId))
                        axes.emplace_back(a->origin, a->direction);
                    continue;
                }
                if (e.shape.IsNull()) continue;
                try {
                    if (e.type == SelectionType::Face && e.shape.ShapeType() == TopAbs_FACE) {
                        Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(e.shape));
                        if (!s.IsNull() && s->IsKind(STANDARD_TYPE(Geom_Plane))) {
                            planarPlanes.push_back(Handle(Geom_Plane)::DownCast(s)->Pln());
                        } else {
                            Handle(Geom_CylindricalSurface) cyl =
                                Handle(Geom_CylindricalSurface)::DownCast(s);
                            if (!cyl.IsNull())
                                cylinders.push_back({gp_Ax1(cyl->Cylinder().Position().Location(),
                                                            cyl->Cylinder().Position().Direction()),
                                                     cyl->Cylinder().Radius()});
                        }
                    } else if (e.type == SelectionType::Edge) {
                        BRepAdaptor_Curve adaptor(TopoDS::Edge(e.shape));
                        if (adaptor.GetType() == GeomAbs_Line)
                            axes.push_back(adaptor.Line().Position());
                    } else if (e.type == SelectionType::Vertex) {
                        vertices.push_back(BRep_Tool::Pnt(TopoDS::Vertex(e.shape)));
                    }
                } catch (...) {}
            }
        }

        if (planarPlanes.size() >= 2) {
            m_planeOpHaveTwoPlanes = true;
            m_planeOpPlaneA = planarPlanes[0];
            m_planeOpPlaneB = planarPlanes[1];
        }
        if (!axes.empty()) {
            m_planeOpHaveAxis = true;
            m_planeOpAxis = axes[0];
            m_planeOpAxisPoint = vertices.empty() ? axes[0].Location() : vertices[0];
        }
        if (!cylinders.empty()) {
            m_planeOpHaveCylinder = true;
            m_planeOpCylAxis = cylinders[0].axis;
            m_planeOpCylRadius = cylinders[0].radius;
            // Side reference for the tangent: radial toward a selected vertex,
            // else a second plane's normal, else world +X.
            if (!vertices.empty()) {
                gp_Vec v(m_planeOpCylAxis.Location(), vertices[0]);
                m_planeOpCylRefDir = (v.Magnitude() > 1e-9) ? gp_Dir(v) : gp_Dir(1, 0, 0);
            } else if (!planarPlanes.empty()) {
                m_planeOpCylRefDir = planarPlanes[0].Axis().Direction();
            } else {
                m_planeOpCylRefDir = gp_Dir(1, 0, 0);
            }
        }
    }

    // A cylindrical face also yields an axis (its centreline), so the
    // Normal-to-Axis mode can build a cross-section plane perpendicular to
    // the cylinder without a separate construction axis. Only fill it in when
    // no explicit axis/edge was selected.
    if (!m_planeOpHaveAxis && m_planeOpHaveCylinder) {
        m_planeOpHaveAxis = true;
        m_planeOpAxis = m_planeOpCylAxis;
        m_planeOpAxisPoint = m_planeOpCylAxis.Location();
    }

    // Pick the most likely mode from what's selected so the common "select
    // geometry, click the button" flow lands ready-to-go: two planes →
    // Midplane; a planar face → Parallel-to-face; a cylinder → Tangent;
    // an axis/edge → Normal-to-axis; nothing relevant → XY.
    if      (m_planeOpHaveTwoPlanes) m_planeOpKindIdx = 4;  // 2 faces → midplane intent
    else if (m_planeOpHaveFace)      m_planeOpKindIdx = 3;
    else if (m_planeOpHaveCylinder)  m_planeOpKindIdx = 6;  // cylinder → tangent default
    else if (m_planeOpHaveAxis)      m_planeOpKindIdx = 5;
    else                             m_planeOpKindIdx = 0;
    m_planeOpOffset = 0.0;
    std::snprintf(m_planeOpOffsetBuf, sizeof(m_planeOpOffsetBuf), "%.2f",
                  m_planeOpOffset);
    m_planeOpPreviewPushed = false;
    m_planeOpActive = true;

    updateConstructionPlane();
}

void Application::beginConstructionPlaneMode(int kindIdx) {
    // Open the plane popup (captures selection, pushes a default preview) then
    // force the explicitly-requested mode. The sidebar only offers a mode when
    // its inputs are present, so the override is always valid.
    beginConstructionPlane();
    if (!m_planeOpActive) return;
    m_planeOpKindIdx = kindIdx;
    updateConstructionPlane();
}

void Application::updateConstructionPlane() {
    if (!m_planeOpActive || !m_history || !m_document) return;

    if (m_planeOpPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_planeOpPreviewPushed = false;
    }

    auto op = std::make_unique<ConstructionPlaneOp>();
    switch (m_planeOpKindIdx) {
        case 0: op->setType(PlaneCreationType::XY);
                op->setOffset(m_planeOpOffset);
                op->setName("XY Plane"); break;
        case 1: op->setType(PlaneCreationType::XZ);
                op->setOffset(m_planeOpOffset);
                op->setName("XZ Plane"); break;
        case 2: op->setType(PlaneCreationType::YZ);
                op->setOffset(m_planeOpOffset);
                op->setName("YZ Plane"); break;
        case 3:
            if (m_planeOpHaveFace) {
                // ParallelToFace places the plane at p1 with the base plane's
                // normal — push p1 along that normal by the offset so the
                // slider drives it away from the face like the user expects.
                gp_Dir n = m_planeOpBaseFace.Axis().Direction();
                gp_Pnt o = m_planeOpBaseFace.Axis().Location();
                gp_Pnt p(o.X() + n.X() * m_planeOpOffset,
                         o.Y() + n.Y() * m_planeOpOffset,
                         o.Z() + n.Z() * m_planeOpOffset);
                op->setBasePlane(m_planeOpBaseFace);
                op->setPoints(p, gp_Pnt(0,0,0), gp_Pnt(0,0,0));
                op->setType(PlaneCreationType::ParallelToFace);
                op->setName("Face Plane");
            } else {
                op->setType(PlaneCreationType::XY);
                op->setName("XY Plane");
            }
            break;
        case 4:   // Midplane
        case 5:   // Normal to axis / edge
        case 6:   // Tangent to cylinder
        case 7: { // Through (containing) the cylinder axis
            gp_Dir N; gp_Pnt P0;
            if (computeDerivedPlaneNP(m_planeOpKindIdx, N, P0)) {
                // Push the through-point along N by the offset so the slider
                // shifts the result off the derived position, reusing the
                // ParallelToFace (normal + point) form.
                gp_Pnt p(P0.X() + N.X() * m_planeOpOffset,
                         P0.Y() + N.Y() * m_planeOpOffset,
                         P0.Z() + N.Z() * m_planeOpOffset);
                op->setBasePlane(gp_Pln(P0, N));
                op->setPoints(p, gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 0));
                if (m_planeOpKindIdx == 4) {
                    op->setType(PlaneCreationType::Midplane);     op->setName("Midplane");
                } else if (m_planeOpKindIdx == 5) {
                    op->setType(PlaneCreationType::NormalToAxis); op->setName("Normal-to-Axis Plane");
                } else if (m_planeOpKindIdx == 6) {
                    op->setType(PlaneCreationType::TangentToCylinder); op->setName("Tangent Plane");
                } else {
                    op->setType(PlaneCreationType::ThroughAxis);  op->setName("Through-Axis Plane");
                }
            } else {
                op->setType(PlaneCreationType::XY);
                op->setName("XY Plane");
            }
            break;
        }
    }
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_planeOpPreviewPushed = true;
        // Auto-select the freshly-pushed plane so the move/rotate gizmo
        // appears on it. ConstructionPlaneOp::execute push_backs to
        // Document, so the just-added id is the back of getAllPlaneIds().
        auto ids = m_document->getAllPlaneIds();
        if (!ids.empty()) {
            SelectionEntry e;
            e.type = SelectionType::Plane;
            e.planeId = ids.back();
            m_selection->select(e);
        }
    }
    m_meshesDirty = true;
}

void Application::commitConstructionPlane() {
    m_planeOpActive = false;
    m_planeOpPreviewPushed = false;
    m_meshesDirty = true;
}

void Application::cancelConstructionPlane() {
    if (m_planeOpPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_planeOpPreviewPushed = false;
    }
    m_planeOpActive = false;
    m_meshesDirty = true;
}

void Application::beginPrimitivePopup(int kindIdx) {
    cancelAllInteractivePreviews();
    m_primitivePopupActive = true;
    m_primitivePopupKind   = kindIdx;
    // Per-kind defaults so opening a fresh popup always starts at a sensible
    // size (kindIdx switches via the toolbar buttons; we re-seed the kind-
    // specific fields without touching the others, which keeps any custom
    // origin / extents the user last typed when they reopen the same kind).
    m_primitivePopupOrigin[0] = 0.0;
    m_primitivePopupOrigin[1] = 0.0;
    m_primitivePopupOrigin[2] = 0.0;
    switch (kindIdx) {
    case 0: // Box
        m_primitivePopupExtents[0] = 10.0;
        m_primitivePopupExtents[1] = 10.0;
        m_primitivePopupExtents[2] = 10.0;
        break;
    case 1: // Cylinder
        m_primitivePopupRadius = 5.0;
        m_primitivePopupHeight = 10.0;
        break;
    case 2: // Sphere
        m_primitivePopupRadius = 5.0;
        break;
    case 3: // Cone
        m_primitivePopupRadius      = 5.0;
        m_primitivePopupTopRadius   = 0.0;
        m_primitivePopupHeight      = 10.0;
        break;
    case 4: // Torus
        m_primitivePopupRadius      = 5.0;
        m_primitivePopupMinorRadius = 2.0;
        break;
    }
}

void Application::commitPrimitivePopup() {
    using K = materializr::PrimitiveOp::Kind;
    auto op = std::make_unique<materializr::PrimitiveOp>();
    K kind = K::Box;
    switch (m_primitivePopupKind) {
        case 0: kind = K::Box;      break;
        case 1: kind = K::Cylinder; break;
        case 2: kind = K::Sphere;   break;
        case 3: kind = K::Cone;     break;
        case 4: kind = K::Torus;    break;
    }
    op->setKind(kind);
    op->setOrigin(m_primitivePopupOrigin[0],
                  m_primitivePopupOrigin[1],
                  m_primitivePopupOrigin[2]);
    op->setBoxExtents(m_primitivePopupExtents[0],
                      m_primitivePopupExtents[1],
                      m_primitivePopupExtents[2]);
    op->setRadius(m_primitivePopupRadius);
    op->setHeight(m_primitivePopupHeight);
    op->setTopRadius(m_primitivePopupTopRadius);
    op->setMinorRadius(m_primitivePopupMinorRadius);
    if (m_history->pushOperation(std::move(op), *m_document)) {
        auto ids = m_document->getAllBodyIds();
        if (!ids.empty()) {
            SelectionEntry e;
            e.type = SelectionType::Body;
            e.bodyId = ids.back();
            m_selection->select(e);
        }
        m_meshesDirty = true;
    }
    m_primitivePopupActive = false;
}

void Application::cancelPrimitivePopup() {
    m_primitivePopupActive = false;
}

bool Application::computeDerivedPlaneNP(int kindIdx, gp_Dir& outNormal,
                                        gp_Pnt& outPoint) const {
    if (kindIdx == 4) { // Midplane — centred between the two captured planes
        if (!m_planeOpHaveTwoPlanes) return false;
        gp_Dir nA = m_planeOpPlaneA.Axis().Direction();
        gp_Dir nB = m_planeOpPlaneB.Axis().Direction();
        // Align B's normal with A before averaging so two faces pointing at
        // each other (antiparallel) don't cancel to a zero vector.
        gp_Vec sum(nA);
        if (nA.Dot(nB) < 0.0) sum -= gp_Vec(nB); else sum += gp_Vec(nB);
        if (sum.Magnitude() < 1e-9) return false;
        outNormal = gp_Dir(sum);
        gp_Pnt a = m_planeOpPlaneA.Axis().Location();
        gp_Pnt b = m_planeOpPlaneB.Axis().Location();
        // Midpoint of the two plane origins sits exactly on the perpendicular
        // midplane (its component along N is the average of the two offsets);
        // the in-plane component is irrelevant to the resulting plane.
        outPoint = gp_Pnt((a.X() + b.X()) * 0.5,
                          (a.Y() + b.Y()) * 0.5,
                          (a.Z() + b.Z()) * 0.5);
        return true;
    }
    if (kindIdx == 5) { // Normal to axis/edge through the captured point
        if (!m_planeOpHaveAxis) return false;
        outNormal = m_planeOpAxis.Direction();
        outPoint  = m_planeOpAxisPoint;
        return true;
    }
    if (kindIdx == 6) { // Tangent to cylinder, on the reference-direction side
        if (!m_planeOpHaveCylinder) return false;
        gp_Vec axv(m_planeOpCylAxis.Direction());
        gp_Vec ref(m_planeOpCylRefDir);
        gp_Vec radial = ref - axv * ref.Dot(axv);   // perp-to-axis component
        if (radial.Magnitude() < 1e-9) {
            // Reference is parallel to the axis — fall back to any perpendicular.
            gp_Ax2 tmp(m_planeOpCylAxis.Location(), m_planeOpCylAxis.Direction());
            radial = gp_Vec(tmp.XDirection());
        }
        outNormal = gp_Dir(radial);
        gp_Pnt c = m_planeOpCylAxis.Location();
        outPoint = gp_Pnt(c.X() + outNormal.X() * m_planeOpCylRadius,
                          c.Y() + outNormal.Y() * m_planeOpCylRadius,
                          c.Z() + outNormal.Z() * m_planeOpCylRadius);
        return true;
    }
    if (kindIdx == 7) { // Plane CONTAINING the cylinder axis (longitudinal)
        if (!m_planeOpHaveCylinder) return false;
        gp_Vec axv(m_planeOpCylAxis.Direction());
        gp_Vec ref(m_planeOpCylRefDir);
        gp_Vec radial = ref - axv * ref.Dot(axv);     // perp-to-axis component
        if (radial.Magnitude() < 1e-9) {
            gp_Ax2 tmp(m_planeOpCylAxis.Location(), m_planeOpCylAxis.Direction());
            radial = gp_Vec(tmp.XDirection());
        }
        // The plane contains both the axis and the radial reference, so its
        // normal is perpendicular to both. Offset (applied by the caller along
        // this normal) slides it to a parallel chord cut.
        gp_Vec nrm = axv.Crossed(radial);
        if (nrm.Magnitude() < 1e-9) return false;
        outNormal = gp_Dir(nrm);
        outPoint  = m_planeOpCylAxis.Location();
        return true;
    }
    return false;
}

// ─── Construction Axis interactive popup ───────────────────────────────────
//
// Same skeleton as the plane popup: begin seeds defaults, update rebuilds
// the ConstructionAxisOp on every radio/value change (preview-undo first),
// commit leaves the last preview in place, cancel undoes the preview.
// Auto-selects the just-pushed axis so the user can immediately pivot to
// the Move gizmo / Revolve later.

void Application::beginConstructionAxis() {
    if (m_axisOpActive) return; // already open
    m_axisOpActive = true;
    m_axisOpOrigin[0] = m_axisOpOrigin[1] = m_axisOpOrigin[2] = 0.0;
    for (int i = 0; i < 3; ++i) {
        std::snprintf(m_axisOpOriginBuf[i], sizeof(m_axisOpOriginBuf[i]),
                      "%.2f", m_axisOpOrigin[i]);
    }

    // Gather selection-derived inputs (cylinder centreline, straight edge,
    // two vertices, a planar face's normal, two planes' intersection).
    m_axisOpHaveCylinder = m_axisOpHaveEdge = m_axisOpHaveTwoVerts = false;
    m_axisOpHaveFaceNormal = m_axisOpHaveTwoPlanes = false;
    {
        std::vector<gp_Pln> planarPlanes;
        std::vector<gp_Pnt> vertices;
        if (m_selection) {
            for (const auto& e : m_selection->getSelection()) {
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    if (const auto* pe = m_document->getPlane(e.planeId))
                        planarPlanes.push_back(pe->plane);
                    continue;
                }
                if (e.shape.IsNull()) continue;
                try {
                    if (e.type == SelectionType::Face && e.shape.ShapeType() == TopAbs_FACE) {
                        Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(e.shape));
                        if (s.IsNull()) continue;
                        Handle(Geom_CylindricalSurface) cyl =
                            Handle(Geom_CylindricalSurface)::DownCast(s);
                        if (!cyl.IsNull()) {
                            if (!m_axisOpHaveCylinder) {
                                m_axisOpCylAxis = gp_Ax1(cyl->Cylinder().Position().Location(),
                                                         cyl->Cylinder().Position().Direction());
                                m_axisOpHaveCylinder = true;
                            }
                        } else if (s->IsKind(STANDARD_TYPE(Geom_Plane))) {
                            gp_Pln pln = Handle(Geom_Plane)::DownCast(s)->Pln();
                            planarPlanes.push_back(pln);
                            if (!m_axisOpHaveFaceNormal) {
                                // Anchor at the FACE CENTROID — the plane's
                                // parametric origin is wherever the surface
                                // happens to start, often a corner ("axis on
                                // lower corner instead of straight out of
                                // the face").
                                gp_Pnt anchor = pln.Axis().Location();
                                try {
                                    GProp_GProps pr;
                                    BRepGProp::SurfaceProperties(
                                        TopoDS::Face(e.shape), pr);
                                    anchor = pr.CentreOfMass();
                                } catch (...) {}
                                m_axisOpFacePt = anchor;
                                m_axisOpFaceNormal = pln.Axis().Direction();
                                m_axisOpHaveFaceNormal = true;
                            }
                        }
                    } else if (e.type == SelectionType::Edge) {
                        BRepAdaptor_Curve ad(TopoDS::Edge(e.shape));
                        if (ad.GetType() == GeomAbs_Line && !m_axisOpHaveEdge) {
                            m_axisOpEdgeAxis = ad.Line().Position();
                            m_axisOpHaveEdge = true;
                        }
                    } else if (e.type == SelectionType::Vertex) {
                        vertices.push_back(BRep_Tool::Pnt(TopoDS::Vertex(e.shape)));
                    }
                } catch (...) {}
            }
        }
        if (vertices.size() >= 2) {
            m_axisOpHaveTwoVerts = true;
            m_axisOpV1 = vertices[0]; m_axisOpV2 = vertices[1];
        }
        if (planarPlanes.size() >= 2) {
            m_axisOpHaveTwoPlanes = true;
            m_axisOpPlaneA = planarPlanes[0]; m_axisOpPlaneB = planarPlanes[1];
        }
    }

    // Default to the most likely mode for the selection; else user-Z (up).
    if      (m_axisOpHaveCylinder)   m_axisOpKindIdx = 3;
    else if (m_axisOpHaveEdge)       m_axisOpKindIdx = 4;
    else if (m_axisOpHaveTwoVerts)   m_axisOpKindIdx = 5;
    else if (m_axisOpHaveTwoPlanes)  m_axisOpKindIdx = 7;
    else if (m_axisOpHaveFaceNormal) m_axisOpKindIdx = 6;
    else                             m_axisOpKindIdx = 2;

    m_axisOpPreviewPushed = false;
    updateConstructionAxis();
}

void Application::beginConstructionAxisMode(int kindIdx) {
    beginConstructionAxis();
    if (!m_axisOpActive) return;
    m_axisOpKindIdx = kindIdx;   // override the auto-default
    updateConstructionAxis();
}

bool Application::computeDerivedAxisOD(int kindIdx, gp_Pnt& outOrigin,
                                       gp_Dir& outDir) const {
    switch (kindIdx) {
        case 3: // cylinder centreline
            if (!m_axisOpHaveCylinder) return false;
            outOrigin = m_axisOpCylAxis.Location();
            outDir    = m_axisOpCylAxis.Direction();
            return true;
        case 4: // along straight edge
            if (!m_axisOpHaveEdge) return false;
            outOrigin = m_axisOpEdgeAxis.Location();
            outDir    = m_axisOpEdgeAxis.Direction();
            return true;
        case 5: { // through two vertices
            if (!m_axisOpHaveTwoVerts) return false;
            gp_Vec v(m_axisOpV1, m_axisOpV2);
            if (v.Magnitude() < 1e-9) return false;
            outOrigin = m_axisOpV1;
            outDir    = gp_Dir(v);
            return true;
        }
        case 6: // normal to a planar face, through its point
            if (!m_axisOpHaveFaceNormal) return false;
            outOrigin = m_axisOpFacePt;
            outDir    = m_axisOpFaceNormal;
            return true;
        case 7: { // intersection line of two planes
            if (!m_axisOpHaveTwoPlanes) return false;
            IntAna_QuadQuadGeo inter(m_axisOpPlaneA, m_axisOpPlaneB,
                                     Precision::Angular(), Precision::Confusion());
            if (!inter.IsDone() || inter.NbSolutions() < 1) return false;
            gp_Lin line = inter.Line(1);
            outOrigin = line.Location();
            outDir    = line.Direction();
            return true;
        }
        default: return false;
    }
}

void Application::updateConstructionAxis() {
    if (!m_axisOpActive || !m_history || !m_document) return;

    // Undo the previous preview so we don't stack axes on every radio /
    // origin tweak (same dance the plane popup uses).
    if (m_axisOpPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_axisOpPreviewPushed = false;
    }

    auto op = std::make_unique<ConstructionAxisOp>();
    if (m_axisOpKindIdx <= 2) {
        // World axes through a typed origin. User-Z-up remap (same as the
        // plane popup): the popup's X / Y / Z labels are in the user's Z-up
        // convention, so user-Y → world-Z (depth) and user-Z → world-Y (up).
        AxisCreationType kind = AxisCreationType::WorldY;
        const char* nm = "Axis";
        switch (m_axisOpKindIdx) {
            case 0: kind = AxisCreationType::WorldX; nm = "X Axis"; break;
            case 1: kind = AxisCreationType::WorldZ; nm = "Y Axis"; break;
            case 2: kind = AxisCreationType::WorldY; nm = "Z Axis"; break;
        }
        op->setType(kind);
        op->setOrigin(gp_Pnt(m_axisOpOrigin[0], m_axisOpOrigin[1], m_axisOpOrigin[2]));
        op->setName(nm);
    } else if (m_axisOpKindIdx == 5) {
        // Two points → TwoPoints (its computeAxis derives direction from p1→p2).
        if (m_axisOpHaveTwoVerts) {
            op->setPoints(m_axisOpV1, m_axisOpV2);
            op->setType(AxisCreationType::TwoPoints);
            op->setName("Axis (2 points)");
        } else {
            op->setType(AxisCreationType::WorldY); op->setName("Z Axis");
        }
    } else {
        // Derived (cylinder / edge / face-normal / plane-intersection): the
        // host resolves (origin, direction); the op passes them through.
        gp_Pnt o; gp_Dir d;
        if (computeDerivedAxisOD(m_axisOpKindIdx, o, d)) {
            op->setOrigin(o);
            op->setDirection(d);
            switch (m_axisOpKindIdx) {
                case 3: op->setType(AxisCreationType::FromCylinderAxis);
                        op->setName("Cylinder Axis"); break;
                case 4: op->setType(AxisCreationType::AlongEdge);
                        op->setName("Edge Axis"); break;
                case 6: op->setType(AxisCreationType::ThroughFaceNormal);
                        op->setName("Face-Normal Axis"); break;
                case 7: op->setType(AxisCreationType::TwoPlanesIntersection);
                        op->setName("Plane-Intersection Axis"); break;
            }
        } else {
            op->setType(AxisCreationType::WorldY); op->setName("Z Axis");
        }
    }

    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_axisOpPreviewPushed = true;
        // Auto-select the freshly-pushed axis so click-Move workflows pick
        // it up immediately. Mirrors the construction-plane popup.
        auto ids = m_document->getAllAxisIds();
        if (!ids.empty()) {
            SelectionEntry e;
            e.type = SelectionType::Axis;
            e.axisId = ids.back();
            m_selection->select(e);
        }
    }
    m_meshesDirty = true;
}

void Application::commitConstructionAxis() {
    m_axisOpActive = false;
    m_axisOpPreviewPushed = false;
    m_meshesDirty = true;
}

void Application::cancelConstructionAxis() {
    if (m_axisOpPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_axisOpPreviewPushed = false;
    }
    m_axisOpActive = false;
    m_meshesDirty = true;
}

// ─── Sketch-mode Pattern (Linear / Radial) ─────────────────────────────────
//
// Simpler than body patterns: no live preview, just a modal popup that takes
// count + spacing (linear) or count + angle + origin (radial), then runs an
// inline geometry copy similar to SketchCopy and pushes a SketchEditOp.

void Application::beginSketchPattern(PatternKind kind) {
    if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) return;
    m_sketchPatternKind = kind;
    m_sketchPatternCount    = 3;
    m_sketchPatternDistance = 5.0f;
    m_sketchPatternAngle    = 360.0f;
    m_sketchPatternOriginX  = 0.0f;
    m_sketchPatternOriginY  = 0.0f;
    std::snprintf(m_sketchPatternCountBuf,    sizeof(m_sketchPatternCountBuf),    "%d", m_sketchPatternCount);
    std::snprintf(m_sketchPatternDistanceBuf, sizeof(m_sketchPatternDistanceBuf), "%.2f", m_sketchPatternDistance);
    std::snprintf(m_sketchPatternAngleBuf,    sizeof(m_sketchPatternAngleBuf),    "%.1f", m_sketchPatternAngle);
    std::snprintf(m_sketchPatternOXBuf, sizeof(m_sketchPatternOXBuf), "%.2f", m_sketchPatternOriginX);
    std::snprintf(m_sketchPatternOYBuf, sizeof(m_sketchPatternOYBuf), "%.2f", m_sketchPatternOriginY);
    m_sketchPatternFocusInput = true;
    m_sketchPatternActive = true;

    // Capture the selection model NOW (before previewing). On each preview
    // frame we restore from `before` and re-transform these ids; if we
    // re-read the selection from SketchTool after restoring, ids would
    // reference (now-vanished) preview copies.
    m_sketchPatternPts.clear();
    m_sketchPatternLines.clear();
    m_sketchPatternSelectAll = !m_sketchTool->hasElementSelection();
    if (!m_sketchPatternSelectAll) {
        m_sketchPatternPts.insert(m_sketchTool->getSelectedPoints().begin(),
                                  m_sketchTool->getSelectedPoints().end());
        m_sketchPatternLines = m_sketchTool->getSelectedLines();
        for (int lid : m_sketchPatternLines) {
            for (const auto& l : m_activeSketch->getLines()) {
                if (l.id == lid) {
                    m_sketchPatternPts.insert(l.startPointId);
                    m_sketchPatternPts.insert(l.endPointId);
                    break;
                }
            }
        }
    } else {
        for (const auto& p : m_activeSketch->getPoints()) m_sketchPatternPts.insert(p.id);
        for (const auto& l : m_activeSketch->getLines())  m_sketchPatternLines.insert(l.id);
        // Circles + arcs only included via the "selectAll" path (they have no
        // first-class selection state in SketchTool).
        for (const auto& c : m_activeSketch->getCircles())
            m_sketchPatternPts.insert(c.centerPointId);
        for (const auto& a : m_activeSketch->getArcs()) {
            m_sketchPatternPts.insert(a.centerPointId);
            m_sketchPatternPts.insert(a.startPointId);
            m_sketchPatternPts.insert(a.endPointId);
        }
    }

    // Snapshot the sketch for preview rollback / commit diff.
    m_sketchPatternBefore = std::make_shared<materializr::Sketch>(*m_activeSketch);
    updateSketchPattern();
}

void Application::updateSketchPattern() {
    if (!m_sketchPatternActive || !m_activeSketch || !m_sketchPatternBefore) return;
    // Restore the pre-preview state, then re-apply the transform from
    // current parameters. This is how every preview frame stays clean —
    // no leftover copies from earlier preview iterations.
    *m_activeSketch = *m_sketchPatternBefore;
    if (m_sketchPatternCount < 2 || m_sketchPatternPts.empty()) return;

    for (int step = 1; step < m_sketchPatternCount; ++step) {
        std::unordered_map<int,int> remap;
        auto xform = [&](glm::vec2 p) -> glm::vec2 {
            if (m_sketchPatternKind == PatternKind::Linear) {
                return p + glm::vec2(m_sketchPatternDistance * step, 0.0f);
            }
            float stepDeg = m_sketchPatternAngle / m_sketchPatternCount;
            float angRad = (stepDeg * step) * static_cast<float>(M_PI) / 180.0f;
            float dx = p.x - m_sketchPatternOriginX;
            float dy = p.y - m_sketchPatternOriginY;
            float ca = std::cos(angRad), sa = std::sin(angRad);
            return glm::vec2(m_sketchPatternOriginX + dx * ca - dy * sa,
                             m_sketchPatternOriginY + dx * sa + dy * ca);
        };
        for (int oldId : m_sketchPatternPts) {
            auto* p = m_activeSketch->getPoint(oldId);
            if (!p) continue;
            remap[oldId] = m_activeSketch->addPoint(xform(p->pos));
        }
        for (int lid : m_sketchPatternLines) {
            for (const auto& l : m_activeSketch->getLines()) {
                if (l.id != lid) continue;
                auto sIt = remap.find(l.startPointId);
                auto eIt = remap.find(l.endPointId);
                if (sIt != remap.end() && eIt != remap.end())
                    m_activeSketch->addLine(sIt->second, eIt->second);
                break;
            }
        }
        if (m_sketchPatternSelectAll) {
            auto circles = m_activeSketch->getCircles();
            for (const auto& c : circles) {
                auto it = remap.find(c.centerPointId);
                if (it != remap.end()) m_activeSketch->addCircle(it->second, c.radius);
            }
            auto arcs = m_activeSketch->getArcs();
            for (const auto& a : arcs) {
                auto ic = remap.find(a.centerPointId);
                auto is = remap.find(a.startPointId);
                auto ie = remap.find(a.endPointId);
                if (ic != remap.end() && is != remap.end() && ie != remap.end())
                    m_activeSketch->addArc(ic->second, is->second, ie->second, a.radius);
            }
        }
    }
}

void Application::commitSketchPattern() {
    if (!m_sketchPatternActive) return;
    updateSketchPattern(); // make sure the in-doc state reflects current params
    auto after = std::make_shared<materializr::Sketch>(*m_activeSketch);
    if (m_sketchPatternBefore &&
        (m_sketchPatternBefore->getPoints().size()  != after->getPoints().size() ||
         m_sketchPatternBefore->getLines().size()   != after->getLines().size() ||
         m_sketchPatternBefore->getCircles().size() != after->getCircles().size() ||
         m_sketchPatternBefore->getArcs().size()    != after->getArcs().size())) {
        auto op = std::make_unique<SketchEditOp>(m_activeSketch,
                                                  m_sketchPatternBefore, after);
        m_history->pushExecuted(std::move(op));
    }
    m_sketchPatternActive = false;
    m_sketchPatternPickingOrigin = false;
    m_sketchPatternBefore.reset();
    m_sketchPatternPts.clear();
    m_sketchPatternLines.clear();
}

void Application::cancelSketchPattern() {
    if (m_sketchPatternBefore && m_activeSketch) {
        *m_activeSketch = *m_sketchPatternBefore;
    }
    m_sketchPatternActive = false;
    m_sketchPatternPickingOrigin = false;
    m_sketchPatternBefore.reset();
    m_sketchPatternPts.clear();
    m_sketchPatternLines.clear();
}

// ── Rotate Plane About Axis ─────────────────────────────────────────────
// Build the list of candidate hinge lines for the target plane and open the
// popup. Each entry is a gp_Ax1 resolved up-front (transient — we never touch
// the document's axis list). Order: the plane's own U / V axes (tilt in
// place), then every construction axis, then a selected straight edge / a
// selected cylindrical face's centreline if either is in the selection.
void Application::beginRotatePlaneAboutAxis(int planeId) {
    if (!m_document) return;
    const auto* pe = m_document->getPlane(planeId);
    if (!pe) return;

    m_rotPlaneId = planeId;
    m_rotPlaneOriginal = pe->plane;
    m_rotPlaneAngle = 0.0f;
    std::snprintf(m_rotPlaneAngleBuf, sizeof(m_rotPlaneAngleBuf), "0.0");
    m_rotPlaneHinges.clear();
    m_rotPlaneHingeLabels.clear();

    // Tilt-in-place options: the plane's own X / Y directions through its
    // centre, so rotation reorients without translating the plane.
    const gp_Ax3& ax = m_rotPlaneOriginal.Position();
    gp_Pnt center = ax.Location();
    m_rotPlaneHinges.emplace_back(center, ax.XDirection());
    m_rotPlaneHingeLabels.emplace_back("Tilt about plane U (in-plane X)");
    m_rotPlaneHinges.emplace_back(center, ax.YDirection());
    m_rotPlaneHingeLabels.emplace_back("Tilt about plane V (in-plane Y)");

    int defaultIdx = 0;

    // Every construction axis in the document.
    for (int aid : m_document->getAllAxisIds()) {
        const auto* a = m_document->getAxis(aid);
        if (!a) continue;
        m_rotPlaneHinges.emplace_back(a->origin, a->direction);
        m_rotPlaneHingeLabels.emplace_back("Axis: " + a->name);
    }

    // Hinge about real geometry: a co-selected straight edge or cylindrical
    // face. Default to it when present — a co-selected hinge is almost
    // certainly why the user opened this rather than an in-place tilt.
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.shape.IsNull()) continue;
            if (e.type == SelectionType::Edge) {
                BRepAdaptor_Curve adaptor(TopoDS::Edge(e.shape));
                if (adaptor.GetType() == GeomAbs_Line) {
                    defaultIdx = static_cast<int>(m_rotPlaneHinges.size());
                    m_rotPlaneHinges.push_back(adaptor.Line().Position());
                    m_rotPlaneHingeLabels.emplace_back("Selected edge");
                }
            } else if (e.type == SelectionType::Face) {
                Handle(Geom_CylindricalSurface) cyl =
                    Handle(Geom_CylindricalSurface)::DownCast(
                        BRep_Tool::Surface(TopoDS::Face(e.shape)));
                if (!cyl.IsNull()) {
                    defaultIdx = static_cast<int>(m_rotPlaneHinges.size());
                    m_rotPlaneHinges.emplace_back(
                        cyl->Cylinder().Position().Location(),
                        cyl->Cylinder().Position().Direction());
                    m_rotPlaneHingeLabels.emplace_back("Selected face centreline");
                }
            }
        }
    }

    m_rotPlaneHingeIdx = defaultIdx;
    m_rotPlaneActive = true;
}

// Live preview / commit core: rotate the snapshot plane about the chosen
// hinge by the current angle and write it through Document::setPlane. Always
// re-bases from m_rotPlaneOriginal so dragging the angle doesn't accumulate.
void Application::applyRotatePlanePreview() {
    if (!m_document || m_rotPlaneId < 0) return;
    if (m_rotPlaneHingeIdx < 0 ||
        m_rotPlaneHingeIdx >= static_cast<int>(m_rotPlaneHinges.size())) return;
    gp_Trsf t;
    t.SetRotation(m_rotPlaneHinges[m_rotPlaneHingeIdx],
                  m_rotPlaneAngle * M_PI / 180.0);
    gp_Pln rotated = m_rotPlaneOriginal;
    rotated.Transform(t);
    m_document->setPlane(m_rotPlaneId, rotated);
}

void Application::cancelRotatePlaneAboutAxis() {
    if (m_document && m_rotPlaneId >= 0)
        m_document->setPlane(m_rotPlaneId, m_rotPlaneOriginal);
    m_rotPlaneActive = false;
    m_rotPlaneId = -1;
    m_rotPlaneHinges.clear();
    m_rotPlaneHingeLabels.clear();
}

// ─── Async thread re-cut (cascade/editStep recompute path) ──────────────────
// A sketch edit cascading through a Thread step used to re-run the multi-
// second helix sweep + boolean synchronously on the UI thread ("not
// responding"). ThreadOp::execute now hands the recompute here: the body is
// left at its pre-thread state (visually unthreaded for a moment) and the
// re-cut runs on a worker; pollThreadRecuts applies the result when it lands.

bool Application::launchThreadRecut(ThreadOp& op, int attempts) {
    if (!m_document) return false;
    TopoDS_Shape live;
    try { live = m_document->getBody(op.getBodyId()); } catch (...) {}
    if (live.IsNull()) return false;
    // DEEP-COPY for the worker (same reasoning as commitThread: the live
    // TShape's lazy caches are touched by the render thread every frame).
    TopoDS_Shape body = BRepBuilderAPI_Copy(live).Shape();
    if (body.IsNull()) return false;
    auto worker = std::make_shared<ThreadOp>(op); // params copy; buildResult const
    PendingThreadRecut p;
    p.op = &op;
    p.bodyId = op.getBodyId();
    p.launchedFrom = live;
    p.attempts = attempts;
    // Pre-mesh at the CURRENT quality so the renderer reuses the cache
    // instead of freezing the main thread on the helicoid faces; finer
    // angular pass (0.3 rad shows facets on threads).
    float rdefl, rang;
    meshQualityParams(rdefl, rang);
    const float recutAng = std::min(rang, 0.15f);
    p.fut = std::async(std::launch::async,
                       [worker, body, rdefl, recutAng]() {
                           TopoDS_Shape r = worker->buildResult(body);
                           if (!r.IsNull()) {
                               try {
                                   BRepMesh_IncrementalMesh mesh(
                                       r, rdefl, Standard_False,
                                       recutAng, Standard_True);
                                   mesh.Perform();
                               } catch (...) {}
                           }
                           return r;
                       });
    m_threadRecuts.push_back(std::move(p));
    return true;
}

void Application::installThreadRecutHook() {
    ThreadOp::setAsyncRecutHook([this](ThreadOp& op, Document& doc) -> bool {
        // Only the live document (headless/temp docs keep the sync path).
        if (!m_document || &doc != m_document.get()) return false;
        // Single-flight per op: a request while one is pending stays pending —
        // the landing check sees the body changed since launch and RELAUNCHES
        // against the current state, so the newest edit always wins.
        for (auto& p : m_threadRecuts)
            if (p.op == &op) { p.attempts = 1; return true; } // re-arm budget
        if (!launchThreadRecut(op, 1)) return false;
        showToast("Re-cutting thread in the background\xE2\x80\xA6");
        return true;
    });
}

void Application::pollThreadRecuts() {
    for (size_t i = 0; i < m_threadRecuts.size();) {
        auto& p = m_threadRecuts[i];
        if (p.fut.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) { ++i; continue; }
        TopoDS_Shape result = p.fut.get();

        // The op must still be an applied history step.
        int stepIdx = -1;
        for (int k = 0; k <= m_history->currentStep(); ++k)
            if (m_history->getStep(k) == p.op) { stepIdx = k; break; }
        TopoDS_Shape cur;
        try { cur = m_document->getBody(p.bodyId); } catch (...) {}

        if (stepIdx < 0 || cur.IsNull()) {
            // Step deleted / body gone — drop.
            m_threadRecuts.erase(m_threadRecuts.begin() + i);
            continue;
        }
        if (!cur.IsSame(p.launchedFrom)) {
            // The body changed while the worker ran (a second cascade re-ran
            // the chain and committed a NEW TShape — e.g. the sketch edit
            // fired two cascades). This result is stale: RELAUNCH against the
            // current body instead of silently dropping it, or the thread
            // never lands ("it said background but nothing happened").
            ThreadOp* op = p.op;
            int attempts = p.attempts;
            m_threadRecuts.erase(m_threadRecuts.begin() + i);
            if (attempts < 3) launchThreadRecut(*op, attempts + 1);
            else std::fprintf(stderr, "[Thread] recut gave up after %d stale "
                                      "attempts\n", attempts);
            continue;
        }
        if (result.IsNull()) {
            // New geometry can't take the thread — suspend the step with the
            // standard explainer banner instead of silently no-opping.
            m_history->suspendStep(stepIdx);
            showToast("Thread couldn't re-cut on the new geometry \xE2\x80\x94 "
                      "check the Thread step.");
        } else {
            m_document->updateBody(p.bodyId, result);
            m_meshesDirty = true;
        }
        m_threadRecuts.erase(m_threadRecuts.begin() + i);
    }
}

void Application::flushThreadRecuts() {
    // Save path: a snapshot taken mid-recut would bake the unthreaded body
    // under a Thread step. Block the (rare, few-second) remainder instead.
    while (!m_threadRecuts.empty()) {
        m_threadRecuts.front().fut.wait();
        pollThreadRecuts();
    }
}

} // namespace materializr
