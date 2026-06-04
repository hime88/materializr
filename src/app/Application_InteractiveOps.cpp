#include "gl_common.h"

#include <cstdio>
#include <cmath>
#include <limits>
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
#include "modeling/FilletOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/ShellOp.h"
#include "modeling/ResizeCylindricalOp.h"
#include "modeling/PatternOp.h"
#include "modeling/LoftOp.h"
#include "modeling/ConstructionPlaneOp.h"
#include "modeling/ConstructionAxisOp.h"
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>

#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp_Face.hxx>
#include <Bnd_Box.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <Geom_SurfaceOfRevolution.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
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
            // Outward handle direction: from the body centre to the edge, made
            // perpendicular to the edge, so the arrow faces straight out of the edge.
            Bnd_Box bb; BRepBndLib::Add(m_edgeOpPreviousShape, bb);
            if (!bb.IsVoid()) {
                double x1,y1,z1,x2,y2,z2; bb.Get(x1,y1,z1,x2,y2,z2);
                glm::vec3 c((x1+x2)*0.5f, (y1+y2)*0.5f, (z1+z2)*0.5f);
                glm::vec3 out = m_edgeOpMid - c;
                out -= glm::dot(out, m_edgeOpDir) * m_edgeOpDir; // perpendicular to edge
                if (glm::length(out) > 1e-5f) m_edgeOpOutDir = glm::normalize(out);
            }
            m_edgeOpHasHandle = true;
        }
    } catch (...) {}

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
        m_edgeOpPreviousShape = chamferOp->getPreviousShape();
    }
    if (m_edgeOpBodyId < 0 || m_edgeOpEdges.empty() ||
        m_edgeOpPreviousShape.IsNull()) return;

    m_edgeOpActive        = true;
    m_edgeOpEditingIndex  = historyIndex;
    m_edgeOpOrigValue     = m_edgeOpValue; // restored on cancel
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

    // Clear the face selection so the gizmo / overlay rendering doesn't fight
    // a stale "Face Operations" panel while editing.
    m_selection->clear();
    updateInteractiveEdgeOp();
}

namespace {
// Write the previewed radius/distance into the history op being re-edited.
void setEdgeOpParam(const Operation* opRaw, bool isFillet, float v) {
    if (!opRaw) return;
    if (isFillet) {
        if (auto* op = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(opRaw)))
            op->setRadius(static_cast<double>(v));
    } else {
        if (auto* op = const_cast<ChamferOp*>(dynamic_cast<const ChamferOp*>(opRaw)))
            op->setDistance(static_cast<double>(v));
    }
}
} // namespace

void Application::updateInteractiveEdgeOp() {
    if (!m_edgeOpActive || m_edgeOpBodyId < 0) return;

    if (m_edgeOpEditingIndex >= 0) {
        // EDIT mode: preview through the real history replay so downstream
        // steps (a chamfer stacked on this fillet) stay visible during the
        // drag instead of flickering out. Geometrically impossible values
        // are rejected inside editStep (the op snaps back to its last good
        // parameters), so the preview can never strand the model.
        if (m_edgeOpValue < 0.01f) return; // don't preview "remove" mid-drag
        setEdgeOpParam(m_history->getStep(m_edgeOpEditingIndex),
                       m_edgeOpType == EdgeOpType::Fillet,
                       m_edgeOpValue);
        m_history->editStep(m_edgeOpEditingIndex, *m_document);
        m_meshesDirty = true;
        return;
    }

    // CREATE mode: transient op against the snapshotted pre-state.
    // Restore original first, so dragging back to ~0 shows no fillet/chamfer.
    m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    m_meshesDirty = true;
    if (m_edgeOpValue < 0.01f) return;

    try {
        if (m_edgeOpType == EdgeOpType::Fillet) {
            auto op = std::make_unique<FilletOp>();
            op->setBody(m_edgeOpBodyId);
            std::vector<TopoDS_Edge> typedEdges;
            for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
            op->setEdges(typedEdges);
            op->setRadius(static_cast<double>(m_edgeOpValue));
            if (op->execute(*m_document)) {
                m_meshesDirty = true;
            } else {
                // Failed — restore original
                m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
            }
        } else {
            auto op = std::make_unique<ChamferOp>();
            op->setBody(m_edgeOpBodyId);
            std::vector<TopoDS_Edge> typedEdges;
            for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
            op->setEdges(typedEdges);
            op->setDistance(static_cast<double>(m_edgeOpValue));
            if (op->execute(*m_document)) {
                m_meshesDirty = true;
            } else {
                m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
            }
        }
    } catch (...) {
        m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    }
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
                           m_edgeOpOrigValue);
            m_history->editStep(m_edgeOpEditingIndex, *m_document);
        }
        m_edgeOpActive = false;
        m_edgeOpEditingIndex = -1;
        m_edgeOpEdges.clear();
        m_edgeOpPreviousShape.Nullify();
        m_edgeOpType = EdgeOpType::None;
        m_meshesDirty = true;
        return;
    }

    if (m_edgeOpEditingIndex >= 0) {
        // Update the existing op's parameter and rerun from that point so any
        // downstream ops (cuts, fillets stacked on this one, …) recompute too.
        setEdgeOpParam(m_history->getStep(m_edgeOpEditingIndex),
                       m_edgeOpType == EdgeOpType::Fillet,
                       m_edgeOpValue);
        m_history->editStep(m_edgeOpEditingIndex, *m_document);
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
        m_history->pushOperation(std::move(op), *m_document);
    } else {
        auto op = std::make_unique<ChamferOp>();
        op->setBody(m_edgeOpBodyId);
        std::vector<TopoDS_Edge> typedEdges;
        for (const auto& e : m_edgeOpEdges) typedEdges.push_back(TopoDS::Edge(e));
        op->setEdges(typedEdges);
        op->setDistance(static_cast<double>(m_edgeOpValue));
        m_history->pushOperation(std::move(op), *m_document);
    }

    if (m_edgeOpEditingIndex < 0) {
        std::fprintf(stdout, "%s %.1f mm committed\n",
                     m_edgeOpType == EdgeOpType::Fillet ? "Fillet" : "Chamfer",
                     m_edgeOpValue);
    }

    m_edgeOpActive = false;
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
                       m_edgeOpOrigValue);
        m_history->editStep(m_edgeOpEditingIndex, *m_document);
    } else if (m_edgeOpBodyId >= 0 && !m_edgeOpPreviousShape.IsNull()) {
        m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    }
    m_edgeOpActive = false;
    m_edgeOpEditingIndex = -1;
    m_edgeOpEdges.clear();
    m_edgeOpPreviousShape.Nullify();
    m_edgeOpType = EdgeOpType::None;
    m_meshesDirty = true;
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

void Application::beginResizeCylindrical() {
    if (m_resizeCylBodyId < 0) return;
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
        if (op->execute(*m_document)) m_meshesDirty = true;
        else m_document->updateBody(m_resizeCylBodyId, m_resizeCylPreviousShape);
    } catch (...) {
        m_document->updateBody(m_resizeCylBodyId, m_resizeCylPreviousShape);
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

void Application::beginInteractiveShell() {
    if (m_shellBodyId < 0 || m_shellFace.IsNull()) return;
    try {
        m_shellPreviousShape = m_document->getBody(m_shellBodyId);
    } catch (...) { return; }
    m_shellThickness = 1.0f;
    std::snprintf(m_shellInputBuf, sizeof(m_shellInputBuf), "%.2f", m_shellThickness);
    m_shellInputFocus = true;
    m_shellActive = true;
    updateInteractiveShell();
}

void Application::updateInteractiveShell() {
    if (!m_shellActive || m_shellBodyId < 0) return;
    // Reset to the snapshot, then run a fresh ShellOp against it so live
    // preview tracks the typed value exactly without compounding edits.
    m_document->updateBody(m_shellBodyId, m_shellPreviousShape);
    m_meshesDirty = true;
    if (m_shellThickness <= 0.0f) return;
    try {
        auto op = std::make_unique<ShellOp>();
        op->setBody(m_shellBodyId);
        op->setThickness(static_cast<double>(m_shellThickness));
        op->addFaceToRemove(m_shellFace);
        if (op->execute(*m_document)) m_meshesDirty = true;
        else m_document->updateBody(m_shellBodyId, m_shellPreviousShape);
    } catch (...) {
        m_document->updateBody(m_shellBodyId, m_shellPreviousShape);
    }
}

void Application::commitInteractiveShell() {
    if (!m_shellActive) return;
    // Roll back the preview; History::pushOperation re-runs the op cleanly
    // against the snapshot.
    m_document->updateBody(m_shellBodyId, m_shellPreviousShape);
    if (m_shellThickness <= 0.0f) {
        cancelInteractiveShell();
        return;
    }
    auto op = std::make_unique<ShellOp>();
    op->setBody(m_shellBodyId);
    op->setThickness(static_cast<double>(m_shellThickness));
    op->addFaceToRemove(m_shellFace);
    m_history->pushOperation(std::move(op), *m_document);

    m_shellActive = false;
    m_shellBodyId = -1;
    m_shellFace.Nullify();
    m_shellPreviousShape.Nullify();
    m_selection->clear();
    m_meshesDirty = true;
}

void Application::cancelInteractiveShell() {
    if (m_shellBodyId >= 0 && !m_shellPreviousShape.IsNull()) {
        m_document->updateBody(m_shellBodyId, m_shellPreviousShape);
    }
    m_shellActive = false;
    m_shellBodyId = -1;
    m_shellFace.Nullify();
    m_shellPreviousShape.Nullify();
    m_meshesDirty = true;
}

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
    m_extrudeProfile = profile;
    m_extruding = true;
    m_extrudeMode = mode;
    m_extrudeTargetBody = targetBody;
    m_extrudeSketchId = sourceSketchId;
    m_extrudeDistance = 5.0f;
    std::snprintf(m_extrudeInputBuf, sizeof(m_extrudeInputBuf), "%.1f", m_extrudeDistance);
    m_extrudeInputFocus = true;

    // Compute face normal and center
    if (profile.ShapeType() == TopAbs_FACE) {
        BRepGProp_Face prop(TopoDS::Face(profile));
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
    if (m_history->pushOperation(std::move(op), *m_document)) {
        auto ids = m_document->getAllBodyIds();
        m_extrudePreviewBodyId = ids.back();
        m_meshesDirty = true;
    }
}

void Application::updateInteractiveExtrude() {
    if (!m_extruding || m_extrudePreviewBodyId < 0) return;
    if (!std::isfinite(m_extrudeDistance)) { m_extrudeDistance = 0.0f; return; }

    // Remove old preview and create new one at current distance
    m_document->removeBody(m_extrudePreviewBodyId);
    m_history->undo(*m_document); // undo the last extrude

    auto op = std::make_unique<ExtrudeOp>();
    op->setProfile(m_extrudeProfile);
    op->setDistance(extrudeOpDistance());
    op->setMode(ExtrudeMode::NewBody);
    op->setSketchSource(m_extrudeSketchId);
    if (m_history->pushOperation(std::move(op), *m_document)) {
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
            m_history->undo(*m_document);
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
        m_history->undo(*m_document);
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
                                                           float vpW, float vpH) const {
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

        // Screen-space pick tolerance: how far ~6px maps to on this plane, so the
        // boundary catch area is a consistent, comfortable width at any zoom.
        float tol = 0.0f;
        glm::vec3 o2, d2; float t2; glm::vec2 p2d2;
        rayAt(screenX + 6.0f, screenY, o2, d2);
        if (projectToPlane(o2, d2, t2, p2d2)) tol = glm::length(p2d2 - p2d);

        auto regions = sketch.buildRegions();
        bool matched = false;
        for (size_t i = 0; i < regions.size(); ++i) {
            if (sketch.isPointInOrNearRegion(regions[i], p2d, tol)) {
                bestT = t;
                hit.sketchId = sketchId;
                hit.regionIndex = static_cast<int>(i);
                hit.worldPoint = rayOrigin + rayDir * t;
                matched = true;
                break; // first match per sketch is fine; nesting handled by the test
            }
        }
        if (matched) return;

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
    m_pushPullTargets.clear();
    m_pushPullPreviewBodyIds.clear();
    m_pushPullPreviousBodies.clear();
    m_pushPullPreviewPushed = false;

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
            t.sourceBodyId = sketch->getSourceBody();
            t.profile = regions[e.subShapeIndex].face;
            if (t.profile.IsNull()) continue;
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
                // Classifier-based outward verification — mirror of the same
                // check in PushPullOp::execute so the live arrow and the
                // executed extrusion agree on direction even on STEP-imported
                // faces whose surface normal points the wrong way.
                if (tgt0.sourceBodyId >= 0) {
                    try {
                        const TopoDS_Shape& body = m_document->getBody(tgt0.sourceBodyId);
                        if (!body.IsNull()) {
                            // Geometric outward check — same as PushPullOp::
                            // execute. The body bbox centre is in the body's
                            // interior; if the face normal points TOWARD it,
                            // the normal is inward and gets flipped. Far
                            // more reliable than probe-classifier on thin
                            // bodies / edge-adjacent faces.
                            Bnd_Box bodyBB;
                            BRepBndLib::Add(body, bodyBB);
                            Bnd_Box faceBB;
                            BRepBndLib::Add(f, faceBB);
                            if (!bodyBB.IsVoid() && !faceBB.IsVoid()) {
                                double bxmn,bymn,bzmn,bxmx,bymx,bzmx;
                                double fxmn,fymn,fzmn,fxmx,fymx,fzmx;
                                bodyBB.Get(bxmn,bymn,bzmn,bxmx,bymx,bzmx);
                                faceBB.Get(fxmn,fymn,fzmn,fxmx,fymx,fzmx);
                                gp_Vec toBodyCentre(
                                    (bxmn+bxmx)*0.5 - (fxmn+fxmx)*0.5,
                                    (bymn+bymx)*0.5 - (fymn+fymx)*0.5,
                                    (bzmn+bzmx)*0.5 - (fzmn+fzmx)*0.5);
                                if (n.Dot(toBodyCentre) > 0) n.Reverse();
                            }
                        }
                    } catch (...) {}
                }
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

    updatePushPull();
}

void Application::updatePushPull() {
    if (!m_pushPullActive) return;
    if (!std::isfinite(m_pushPullDistance)) { m_pushPullDistance = 0.0f; return; }

    // Snap the live distance to the corner-widget grid step before applying.
    // Mutating m_pushPullDistance itself (rather than just the value passed
    // to setDistance) means the dim-arrow readout, the InputText field, and
    // the slider all reflect the snapped value — there's no "type 5.3, see
    // 5.3 in the field, body extrudes to 5.0" discrepancy. Toggling snap off
    // mid-drag immediately frees the distance to fine values on the next
    // updatePushPull frame.
    if (m_snapToGrid && m_sketchGridStep > 0.0f) {
        const float step = m_sketchGridStep;
        m_pushPullDistance = std::round(m_pushPullDistance / step) * step;
        std::snprintf(m_pushPullInputBuf, sizeof(m_pushPullInputBuf),
                      "%.1f", m_pushPullDistance);
    }

    // Only undo OUR previous preview — not any other pushpull that may already be
    // committed at the top of the history.
    if (m_pushPullPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_pushPullPreviewPushed = false;
    }

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
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_pushPullPreviewPushed = true;
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

void Application::commitPushPull() {
    // The last preview push IS the final state — just clean up
    m_pushPullActive = false;
    m_pushPullPreviewPushed = false;
    m_pushPullTargets.clear();
    m_meshesDirty = true;
    m_selection->clear();
    std::fprintf(stdout, "Push/Pull committed at %.2f mm\n", m_pushPullDistance);
}

void Application::cancelPushPull() {
    if (!m_pushPullActive) return;
    if (m_pushPullPreviewPushed && m_history->canUndo()) {
        m_history->undo(*m_document);
        m_pushPullPreviewPushed = false;
    }
    m_pushPullActive = false;
    m_pushPullTargets.clear();
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
    switch (userIdx) {
        case 0: return glm::vec3(1.0f, 0.0f, 0.0f); // user X → world X
        case 1: return glm::vec3(0.0f, 0.0f, 1.0f); // user Y → world Z (forward/back)
        case 2: return glm::vec3(0.0f, 1.0f, 0.0f); // user Z → world Y (up)
    }
    return glm::vec3(1.0f, 0.0f, 0.0f);
}

int Application::userAxisToWorldIdx(int userIdx) {
    switch (userIdx) { case 0: return 0; case 1: return 2; case 2: return 1; }
    return 0;
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
void Application::cascadeFromSketchEdit(int sketchId) {
    if (sketchId < 0 || !m_history || !m_document) return;
    int n = m_history->stepCount();
    int matched = 0, rebuilt = 0, executed = 0, anyOps = 0, reloadedNoCast = 0;
    bool anyChanged = false;

    // Diagnostic: snapshot body ids before cascade so we can see exactly
    // which bodies got added / replaced. If the count grows, we know a
    // duplicate body was created instead of the existing one being updated.
    auto bodyIdsBefore = m_document->getAllBodyIds();

    for (int i = 0; i < n; ++i) {
        Operation* op = const_cast<Operation*>(m_history->getStep(i));
        if (!op || !op->isEnabled()) continue;
        ++anyOps;
        // Diagnostic: a reloaded Extrude/PushPull shows up as ReplayOp, not
        // a real instance — that's the "post-load loses sketch link"
        // limitation we'll fix later via op-specific serialization.
        const std::string tid = op->typeId();
        if ((tid == "extrude" || tid == "pushpull") &&
            !dynamic_cast<ExtrudeOp*>(op) &&
            !dynamic_cast<PushPullOp*>(op)) ++reloadedNoCast;

        if (auto* ext = dynamic_cast<ExtrudeOp*>(op)) {
            if (ext->getSketchId() != sketchId) continue;
            ++matched;
            if (!ext->rebuildProfileFromSketch(*m_document)) continue;
            ++rebuilt;
            if (ext->execute(*m_document)) { ++executed; anyChanged = true; }
        } else if (auto* pp = dynamic_cast<PushPullOp*>(op)) {
            // PushPullOp can hold multiple targets, each with its own
            // sketch source — only re-execute if at least one references
            // the edited sketch.
            bool refs = false;
            int tc = pp->targetCount();
            for (int t = 0; t < tc; ++t) {
                if (pp->getSketchIdAt(t) == sketchId) { refs = true; break; }
            }
            if (!refs) continue;
            ++matched;
            if (!pp->rebuildProfileFromSketch(*m_document, sketchId)) continue;
            ++rebuilt;
            if (pp->execute(*m_document)) { ++executed; anyChanged = true; }
        }
    }
    auto bodyIdsAfter = m_document->getAllBodyIds();
    int added = 0;
    for (int id : bodyIdsAfter) {
        bool wasThere = false;
        for (int b : bodyIdsBefore) if (b == id) { wasThere = true; break; }
        if (!wasThere) ++added;
    }
    std::fprintf(stderr,
        "[Cascade] sketchId=%d  steps=%d enabled=%d  reloadedNoCast=%d  "
        "matched=%d  rebuilt=%d  executed=%d  bodies_before=%zu bodies_after=%zu added=%d\n",
        sketchId, n, anyOps, reloadedNoCast, matched, rebuilt, executed,
        bodyIdsBefore.size(), bodyIdsAfter.size(), added);
    if (anyChanged) m_meshesDirty = true;
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
                                m_axisOpFacePt = pln.Axis().Location();
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

} // namespace materializr
