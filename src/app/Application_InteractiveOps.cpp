#include "gl_common.h"

#include <cstdio>
#include <cmath>
#include <limits>

#include "app/Application.h"
#include "viewport/Viewport.h"
#include "viewport/Camera.h"
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

void Application::updateInteractiveEdgeOp() {
    if (!m_edgeOpActive || m_edgeOpBodyId < 0) return;

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
    // Restore original, then do it properly through history
    m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);

    // Confirming with no size set is a no-op — just cancel out. In edit mode
    // a zero value would be a "remove this fillet" — surprising semantics, so
    // we treat that as cancel too and leave the original op intact.
    if (m_edgeOpValue < 0.01f) {
        if (m_edgeOpEditingIndex >= 0)
            m_history->editStep(m_edgeOpEditingIndex, *m_document);
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
        const Operation* opRaw = m_history->getStep(m_edgeOpEditingIndex);
        if (m_edgeOpType == EdgeOpType::Fillet) {
            if (auto* op = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(opRaw))) {
                op->setRadius(static_cast<double>(m_edgeOpValue));
            }
        } else {
            if (auto* op = const_cast<ChamferOp*>(dynamic_cast<const ChamferOp*>(opRaw))) {
                op->setDistance(static_cast<double>(m_edgeOpValue));
            }
        }
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
    if (m_edgeOpBodyId >= 0 && !m_edgeOpPreviousShape.IsNull()) {
        m_document->updateBody(m_edgeOpBodyId, m_edgeOpPreviousShape);
    }
    // In edit mode, replay the existing op (unchanged) so the body returns to
    // its committed state including any downstream ops.
    if (m_edgeOpEditingIndex >= 0) {
        m_history->editStep(m_edgeOpEditingIndex, *m_document);
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
                                          ExtrudeMode mode, int targetBody) {
    m_extrudeProfile = profile;
    m_extruding = true;
    m_extrudeMode = mode;
    m_extrudeTargetBody = targetBody;
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
        for (size_t i = 0; i < regions.size(); ++i) {
            if (sketch.isPointInOrNearRegion(regions[i], p2d, tol)) {
                bestT = t;
                hit.sketchId = sketchId;
                hit.regionIndex = static_cast<int>(i);
                hit.worldPoint = rayOrigin + rayDir * t;
                break; // first match per sketch is fine; nesting handled by the test
            }
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
                            Bnd_Box bb;
                            BRepBndLib::Add(f, bb);
                            if (!bb.IsVoid()) {
                                double xmn,ymn,zmn,xmx,ymx,zmx;
                                bb.Get(xmn,ymn,zmn,xmx,ymx,zmx);
                                gp_Pnt fc((xmn+xmx)*0.5,(ymn+ymx)*0.5,(zmn+zmx)*0.5);
                                gp_Vec nu = n.Normalized();
                                const double eps = 1.0;
                                gp_Pnt fwd(fc.X() + nu.X() * eps,
                                           fc.Y() + nu.Y() * eps,
                                           fc.Z() + nu.Z() * eps);
                                gp_Pnt back(fc.X() - nu.X() * eps,
                                            fc.Y() - nu.Y() * eps,
                                            fc.Z() - nu.Z() * eps);
                                BRepClass3d_SolidClassifier fcl(body, fwd,  1e-6);
                                BRepClass3d_SolidClassifier bcl(body, back, 1e-6);
                                if (fcl.State() == TopAbs_IN &&
                                    bcl.State() == TopAbs_OUT) {
                                    n.Reverse();
                                }
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
    if (m_history->pushOperation(std::move(op), *m_document)) {
        m_pushPullPreviewPushed = true;
    }
    m_meshesDirty = true;
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
    glm::vec3 worldAxis = userAxisToWorldVec(m_patternAxisIdx);
    if (m_patternKind == PatternKind::Linear) {
        op->setType(PatternType::Linear);
        op->setCount(m_patternCount);
        op->setLinearSpacing(worldAxis.x * m_patternDistance,
                             worldAxis.y * m_patternDistance,
                             worldAxis.z * m_patternDistance);
    } else {
        op->setType(PatternType::Radial);
        op->setCount(m_patternCount);
        op->setRadialAxis(worldAxis.x, worldAxis.y, worldAxis.z);
        op->setRadialOrigin(m_patternOriginX, m_patternOriginY, m_patternOriginZ);
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

} // namespace materializr
