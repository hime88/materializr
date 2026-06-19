#include "MoveHoleOp.h"

#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Pnt.hxx>
#include <cstdio>
#include <cmath>

namespace {

bool isPlanar(const TopoDS_Face& f) {
    Handle(Geom_Surface) s = BRep_Tool::Surface(f);
    return !s.IsNull() && s->IsKind(STANDARD_TYPE(Geom_Plane));
}

bool wireHasEdge(const TopoDS_Wire& w, const TopoDS_Edge& e) {
    for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next())
        if (ex.Current().IsSame(e)) return true;
    return false;
}

// Outward normal of a planar face at its parametric centre.
gp_Vec faceNormal(const TopoDS_Face& f) {
    BRepGProp_Face gf(f);
    double u1, u2, v1, v2;
    gf.Bounds(u1, u2, v1, v2);
    gp_Pnt p; gp_Vec n;
    gf.Normal(0.5 * (u1 + u2), 0.5 * (v1 + v2), p, n);
    if (n.Magnitude() > 1e-12) n.Normalize();
    return n;
}

} // namespace

bool MoveHoleOp::buildVoid(const TopoDS_Shape& body, const TopoDS_Face& seedWall,
                           TopoDS_Shape& voidOut, gp_Vec& entryNormal,
                           bool& isPocket) {
    isPocket = false;
    if (body.IsNull() || seedWall.IsNull()) return false;

    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    // The seed wall borders, along its edges: the hole's openings (planar CAP
    // faces, perpendicular to the bore), and — for a polygon hole — the ADJACENT
    // WALLS (planar, parallel to the bore). Collect every planar neighbour with
    // the shared edge, its normal, and whether that edge is on the neighbour's
    // inner wire (an open mouth) or outer wire (a wall edge, or a pocket floor).
    struct Nbr {
        TopoDS_Face face; TopoDS_Edge rim; gp_Vec n; bool rimInner; TopoDS_Wire opening;
    };
    std::vector<Nbr> nbrs;
    for (TopExp_Explorer ex(seedWall, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
        if (!edgeFaces.Contains(e)) continue;
        const TopTools_ListOfShape& fl = edgeFaces.FindFromKey(e);
        for (TopTools_ListIteratorOfListOfShape it(fl); it.More(); it.Next()) {
            TopoDS_Face f = TopoDS::Face(it.Value());
            if (f.IsSame(seedWall) || !isPlanar(f)) continue;
            Nbr nb; nb.face = f; nb.rim = e; nb.n = faceNormal(f);
            TopoDS_Wire outer = BRepTools::OuterWire(f);
            nb.rimInner = !wireHasEdge(outer, e);
            if (nb.rimInner) {
                for (TopoDS_Iterator wi(f); wi.More(); wi.Next()) {
                    if (wi.Value().ShapeType() != TopAbs_WIRE) continue;
                    TopoDS_Wire w = TopoDS::Wire(wi.Value());
                    if (w.IsSame(outer)) continue;
                    if (wireHasEdge(w, e)) { nb.opening = w; break; }
                }
            }
            nbrs.push_back(nb);
            break; // one neighbour per seed edge
        }
    }

    // An open mouth = a planar neighbour the bore opens THROUGH (rim is an inner
    // loop). Its normal IS the bore axis. Adjacent walls (rim on their outer
    // wire) have normals PERPENDICULAR to the axis — they're not caps.
    const Nbr* entry = nullptr;
    for (const auto& nb : nbrs)
        if (nb.rimInner && !nb.opening.IsNull()) { entry = &nb; break; }
    if (!entry) {
        std::fprintf(stderr, "[MoveHole] no open mouth on the wall (not a through-hole?)\n");
        return false;
    }
    gp_Vec N = entry->n;
    if (N.Magnitude() < 1e-9) return false;

    // Pocket = a cap on the bore axis whose rim is its OUTER wire (a floor that
    // closes the bore), as opposed to a second open mouth. Adjacent walls
    // (normal ⊥ axis) are excluded by the parallel test.
    for (const auto& nb : nbrs) {
        if (!nb.rimInner && std::abs(nb.n.Dot(N)) > 0.99) { isPocket = true; break; }
    }
    if (isPocket) {
        std::fprintf(stderr, "[MoveHole] refused: selection is a pocket (blind hole)\n");
        return false;
    }

    // Depth = the seed wall's extent along N (a prismatic hole's wall spans
    // exactly the hole depth). Robust for round/square/polygon alike.
    double lo = 1e300, hi = -1e300;
    for (TopExp_Explorer vx(seedWall, TopAbs_VERTEX); vx.More(); vx.Next()) {
        gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
        double d = gp_Vec(p.XYZ()).Dot(N);
        lo = std::min(lo, d); hi = std::max(hi, d);
    }
    double depth = hi - lo;
    if (depth < 1e-6) {
        std::fprintf(stderr, "[MoveHole] degenerate hole depth\n");
        return false;
    }

    // Build the opening face and extrude it INTO the body (−N) by EXACTLY the
    // hole depth, so the void's far cap lands on the exit face. No overshoot:
    // an overshoot would stick out past the slab and the fill-fuse would leave
    // nubs (it adds the union, not just the overlap).
    BRepBuilderAPI_MakeFace mf(entry->opening, Standard_True /*only plane*/);
    if (!mf.IsDone()) {
        std::fprintf(stderr, "[MoveHole] could not face the opening loop\n");
        return false;
    }
    BRepPrimAPI_MakePrism prism(mf.Face(), N * (-depth));
    prism.Build();
    if (!prism.IsDone() || prism.Shape().IsNull()) {
        std::fprintf(stderr, "[MoveHole] prism build failed\n");
        return false;
    }
    voidOut = prism.Shape();
    entryNormal = N;
    return true;
}

bool MoveHoleOp::execute(Document& doc) {
    m_wasPocket = false;
    TopoDS_Shape body = doc.getBody(m_bodyId);
    if (body.IsNull() || m_seedWall.IsNull()) return false;
    m_previousShape = body;

    TopoDS_Shape voidSolid;
    gp_Vec entryNormal;
    if (!buildVoid(body, m_seedWall, voidSolid, entryNormal, m_wasPocket))
        return false; // pocket or unrecognized → caller toasts

    // Project the requested move onto the entry plane (a hole slides ACROSS its
    // face, never along the bore — that would just deepen/shorten it).
    gp_Vec move = m_move;
    double along = move.Dot(entryNormal);
    move -= entryNormal * along;
    if (move.Magnitude() < 1e-9) return false; // no in-plane motion

    try {
        // Fill the old hole back to solid, then cut the same void at the new spot.
        BRepAlgoAPI_Fuse fuse(body, voidSolid);
        fuse.Build();
        if (!fuse.IsDone() || fuse.Shape().IsNull()) return false;

        gp_Trsf t; t.SetTranslation(move);
        TopoDS_Shape movedVoid = BRepBuilderAPI_Transform(voidSolid, t, true).Shape();

        BRepAlgoAPI_Cut cut(fuse.Shape(), movedVoid);
        cut.Build();
        if (!cut.IsDone() || cut.Shape().IsNull()) return false;

        TopoDS_Shape result = cut.Shape();
        // The fill-fuse leaves the patched disk as a separate face coplanar with
        // the original face, with a ghost circular edge between them. Merge same-
        // surface faces and drop the redundant seam so the old location looks
        // untouched (also tidies the new hole's edges).
        try {
            ShapeUpgrade_UnifySameDomain unify(result, Standard_True /*edges*/,
                                               Standard_True /*faces*/,
                                               Standard_False /*concat bsplines*/);
            unify.Build();
            if (!unify.Shape().IsNull()) result = unify.Shape();
        } catch (...) { /* keep the un-unified result rather than fail the move */ }

        if (!BRepCheck_Analyzer(result).IsValid()) {
            std::fprintf(stderr, "[MoveHole] result invalid\n");
            return false;
        }
        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        std::fprintf(stderr, "[MoveHole] OCCT exception\n");
        return false;
    }
}

bool MoveHoleOp::undo(Document& doc) {
    if (m_previousShape.IsNull()) return false;
    doc.updateBody(m_bodyId, m_previousShape);
    return true;
}

OperationDiff MoveHoleOp::captureDiff() const {
    // No params/rehydrate yet → reloads as a baked ReplayOp. Reporting the
    // pre-op body lets that replay restore the right shape, so the moved hole
    // survives save/reload (just not re-editable across sessions for now).
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string MoveHoleOp::description() const {
    double mag = m_move.Magnitude();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Move hole %.1f mm", mag);
    return buf;
}
