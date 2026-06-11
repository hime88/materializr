#include "Sketch.h"

#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BOPAlgo_Builder.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Face.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Curve.hxx>
#include <GC_MakeCircle.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <Geom_BSplineCurve.hxx>
#include <ElCLib.hxx>
#include <gp_Pnt.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt2d.hxx>
#include <TopoDS.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

namespace materializr {

Sketch::Sketch()
    : m_plane(gp_Pln()) // default XY plane at origin
{
}

void Sketch::setPlane(const gp_Pln& plane) {
    m_plane = plane;
}

const gp_Pln& Sketch::getPlane() const {
    return m_plane;
}

gp_Pnt Sketch::sketchToWorld(glm::vec2 pt2d) const {
    const gp_Ax3& ax = m_plane.Position();
    gp_Pnt origin = ax.Location();
    gp_Dir xDir = ax.XDirection();
    gp_Dir yDir = ax.YDirection();
    return gp_Pnt(
        origin.X() + pt2d.x * xDir.X() + pt2d.y * yDir.X(),
        origin.Y() + pt2d.x * xDir.Y() + pt2d.y * yDir.Y(),
        origin.Z() + pt2d.x * xDir.Z() + pt2d.y * yDir.Z()
    );
}

// Point management

int Sketch::addPoint(glm::vec2 pos, bool fromText) {
    SketchPoint pt;
    pt.id = nextId();
    pt.pos = pos;
    pt.isConstruction = false;
    pt.fromText = fromText;
    m_points.push_back(pt);
    return pt.id;
}

void Sketch::movePoint(int id, glm::vec2 pos) {
    SketchPoint* pt = findPoint(id);
    if (pt) {
        pt->pos = pos;
    }
}

const SketchPoint* Sketch::getPoint(int id) const {
    for (const auto& pt : m_points) {
        if (pt.id == id) return &pt;
    }
    return nullptr;
}

const std::vector<SketchPoint>& Sketch::getPoints() const {
    return m_points;
}

SketchPoint* Sketch::findPoint(int id) {
    for (auto& pt : m_points) {
        if (pt.id == id) return &pt;
    }
    return nullptr;
}

// Element creation

int Sketch::addLine(int startPtId, int endPtId, bool fromText) {
    SketchLine line;
    line.id = nextId();
    line.startPointId = startPtId;
    line.endPointId = endPtId;
    line.isConstruction = false;
    line.fromText = fromText;
    m_lines.push_back(line);
    return line.id;
}

int Sketch::addCircle(int centerPtId, double radius) {
    SketchCircle circle;
    circle.id = nextId();
    circle.centerPointId = centerPtId;
    circle.radius = radius;
    circle.isConstruction = false;
    m_circles.push_back(circle);
    return circle.id;
}

void Sketch::setCircleRadius(int circleId, double r) {
    for (auto& c : m_circles) {
        if (c.id == circleId) { c.radius = std::max(r, 1e-6); return; }
    }
}

void Sketch::setArcRadius(int arcId, double r) {
    for (auto& a : m_arcs) {
        if (a.id == arcId) { a.radius = std::max(r, 1e-6); return; }
    }
}

int Sketch::addArc(int centerPtId, int startPtId, int endPtId, double radius) {
    SketchArc arc;
    arc.id = nextId();
    arc.centerPointId = centerPtId;
    arc.startPointId = startPtId;
    arc.endPointId = endPtId;
    arc.radius = radius;
    arc.isConstruction = false;
    m_arcs.push_back(arc);
    return arc.id;
}

int Sketch::addSpline(const std::vector<int>& controlPointIds) {
    SketchSpline spline;
    spline.id = nextId();
    spline.controlPointIds = controlPointIds;
    spline.isConstruction = false;
    m_splines.push_back(spline);
    return spline.id;
}

int Sketch::addPolygon(int centerPtId, double radius, int sides, double rotationRad) {
    SketchPolygon polygon;
    polygon.id = nextId();
    polygon.centerPointId = centerPtId;
    polygon.radius = radius;
    polygon.sides = sides;

    const SketchPoint* center = getPoint(centerPtId);
    if (!center) return -1;
    // COPY the centre position — `center` points INTO m_points, and the
    // addPoint calls below reallocate that vector. The dangling pointer
    // produced vertices missing the centre offset or at ±1e26 ("many
    // meters long"), and only on the FIRST polygon per session: the first
    // commit grows the vector past capacity; after an undo the capacity
    // remains, the retry doesn't reallocate, and the same clicks succeed.
    const glm::vec2 centerPos = center->pos;

    // Create N vertex points evenly spaced around center. The first vertex
    // lands at angle `rotationRad` so the caller can align it with the cursor
    // direction — used by SketchTool to snap a corner of the polygon to the
    // grid (the first vertex is exactly under the snapped cursor).
    for (int i = 0; i < sides; ++i) {
        double angle = rotationRad + 2.0 * M_PI * i / sides;
        float vx = centerPos.x + static_cast<float>(radius * std::cos(angle));
        float vy = centerPos.y + static_cast<float>(radius * std::sin(angle));
        int ptId = addPoint(glm::vec2(vx, vy));
        polygon.vertexPointIds.push_back(ptId);
    }

    // Create N lines connecting consecutive vertices (closing the loop)
    for (int i = 0; i < sides; ++i) {
        int startPtId = polygon.vertexPointIds[i];
        int endPtId = polygon.vertexPointIds[(i + 1) % sides];
        int lineId = addLine(startPtId, endPtId);
        polygon.lineIds.push_back(lineId);
    }

    polygon.isConstruction = false;
    m_polygons.push_back(polygon);
    return polygon.id;
}

int Sketch::addRectangle(glm::vec2 corner1, glm::vec2 corner2) {
    // Create 4 corner points
    glm::vec2 c1 = corner1;
    glm::vec2 c2 = glm::vec2(corner2.x, corner1.y);
    glm::vec2 c3 = corner2;
    glm::vec2 c4 = glm::vec2(corner1.x, corner2.y);

    int p1 = addPoint(c1);
    int p2 = addPoint(c2);
    int p3 = addPoint(c3);
    int p4 = addPoint(c4);

    // Create 4 lines forming a closed rectangle
    int firstLineId = addLine(p1, p2);
    addLine(p2, p3);
    addLine(p3, p4);
    addLine(p4, p1);

    return firstLineId;
}

// Element access

const std::vector<SketchLine>& Sketch::getLines() const {
    return m_lines;
}

const std::vector<SketchCircle>& Sketch::getCircles() const {
    return m_circles;
}

const std::vector<SketchArc>& Sketch::getArcs() const {
    return m_arcs;
}

const std::vector<SketchSpline>& Sketch::getSplines() const {
    return m_splines;
}

namespace {
// Centripetal Catmull-Rom point on the span p1->p2 at local t in [0,1].
// LOCAL control: each span only sees its two neighbours, so the curve
// hugs the clicked points instead of ballooning the way a global C2
// interpolation does around sparse points and closed seams (Steve's
// "the math is just a little off / it ends weird" vase).
glm::vec2 catmullRomPoint(glm::vec2 p0, glm::vec2 p1, glm::vec2 p2,
                          glm::vec2 p3, float t) {
    auto tj = [](float ti, glm::vec2 a, glm::vec2 b) {
        float d = glm::length(b - a);
        return ti + std::sqrt(std::max(d, 1e-6f));
    };
    float t0 = 0.0f;
    float t1 = tj(t0, p0, p1);
    float t2 = tj(t1, p1, p2);
    float t3 = tj(t2, p2, p3);
    float tt = t1 + (t2 - t1) * t;
    auto lp = [](glm::vec2 a, glm::vec2 b, float u) {
        return a * (1.0f - u) + b * u;
    };
    glm::vec2 A1 = lp(p0, p1, (tt - t0) / std::max(t1 - t0, 1e-6f));
    glm::vec2 A2 = lp(p1, p2, (tt - t1) / std::max(t2 - t1, 1e-6f));
    glm::vec2 A3 = lp(p2, p3, (tt - t2) / std::max(t3 - t2, 1e-6f));
    glm::vec2 B1 = lp(A1, A2, (tt - t0) / std::max(t2 - t0, 1e-6f));
    glm::vec2 B2 = lp(A2, A3, (tt - t1) / std::max(t3 - t1, 1e-6f));
    return lp(B1, B2, (tt - t1) / std::max(t2 - t1, 1e-6f));
}
} // anonymous

std::vector<glm::vec2> Sketch::interpolate2D(const std::vector<glm::vec2>& ctrl,
                                             int segsPerSpan, bool closed) {
    int n = static_cast<int>(ctrl.size());
    if (n < 2) return ctrl;
    auto at = [&](int i) -> glm::vec2 {
        if (closed) return ctrl[((i % n) + n) % n];
        // Open ends: reflect the end point through its neighbour for a
        // natural phantom tangent.
        if (i < 0) return ctrl[0] * 2.0f - ctrl[1];
        if (i >= n) return ctrl[n - 1] * 2.0f - ctrl[n - 2];
        return ctrl[i];
    };
    int spans = closed ? n : n - 1;
    std::vector<glm::vec2> out;
    out.reserve(spans * segsPerSpan + 1);
    for (int s = 0; s < spans; ++s) {
        for (int i = 0; i < segsPerSpan; ++i) {
            float t = static_cast<float>(i) / segsPerSpan;
            out.push_back(catmullRomPoint(at(s - 1), at(s), at(s + 1),
                                          at(s + 2), t));
        }
    }
    out.push_back(closed ? ctrl[0] : ctrl[n - 1]);
    return out;
}

std::vector<glm::vec2> Sketch::sampleSpline2D(const SketchSpline& sp,
                                              int segsPerSpan) const {
    const auto& ids = sp.controlPointIds;
    bool closedSp = ids.size() > 2 && ids.front() == ids.back();
    std::vector<glm::vec2> ctrl;
    ctrl.reserve(ids.size());
    size_t n = ids.size() - (closedSp ? 1 : 0);
    for (size_t k = 0; k < n; ++k) {
        const SketchPoint* p = getPoint(ids[k]);
        if (!p) return ctrl;
        ctrl.push_back(p->pos);
    }
    return interpolate2D(ctrl, segsPerSpan, closedSp);
}

const std::vector<SketchPolygon>& Sketch::getPolygons() const {
    return m_polygons;
}

// Element removal

void Sketch::removeElement(int id) {
    m_lines.erase(
        std::remove_if(m_lines.begin(), m_lines.end(),
            [id](const SketchLine& l) { return l.id == id; }),
        m_lines.end());

    m_circles.erase(
        std::remove_if(m_circles.begin(), m_circles.end(),
            [id](const SketchCircle& c) { return c.id == id; }),
        m_circles.end());

    m_arcs.erase(
        std::remove_if(m_arcs.begin(), m_arcs.end(),
            [id](const SketchArc& a) { return a.id == id; }),
        m_arcs.end());

    m_splines.erase(
        std::remove_if(m_splines.begin(), m_splines.end(),
            [id](const SketchSpline& s) { return s.id == id; }),
        m_splines.end());

    m_polygons.erase(
        std::remove_if(m_polygons.begin(), m_polygons.end(),
            [id](const SketchPolygon& p) { return p.id == id; }),
        m_polygons.end());

    m_points.erase(
        std::remove_if(m_points.begin(), m_points.end(),
            [id](const SketchPoint& p) { return p.id == id; }),
        m_points.end());
}

int Sketch::pruneOrphanPoints() {
    // Every point id still referenced by some geometry element.
    std::unordered_set<int> used;
    for (const auto& l : m_lines) { used.insert(l.startPointId); used.insert(l.endPointId); }
    for (const auto& c : m_circles) used.insert(c.centerPointId);
    for (const auto& a : m_arcs) {
        used.insert(a.centerPointId); used.insert(a.startPointId); used.insert(a.endPointId);
    }
    for (const auto& s : m_splines)
        for (int id : s.controlPointIds) used.insert(id);
    for (const auto& g : m_polygons) {
        used.insert(g.centerPointId);
        for (int id : g.vertexPointIds) used.insert(id);
    }

    // Drop points referenced by nothing (a deleted line's stranded endpoints).
    size_t before = m_points.size();
    m_points.erase(
        std::remove_if(m_points.begin(), m_points.end(),
            [&](const SketchPoint& p) { return used.find(p.id) == used.end(); }),
        m_points.end());
    int pruned = static_cast<int>(before - m_points.size());

    // Drop constraints whose referenced entity (a point or an element) no longer
    // exists — deleting geometry would otherwise leave the solver chasing ghosts.
    std::unordered_set<int> valid;
    for (const auto& p : m_points)   valid.insert(p.id);
    for (const auto& l : m_lines)    valid.insert(l.id);
    for (const auto& c : m_circles)  valid.insert(c.id);
    for (const auto& a : m_arcs)     valid.insert(a.id);
    for (const auto& s : m_splines)  valid.insert(s.id);
    for (const auto& g : m_polygons) valid.insert(g.id);
    m_constraints.erase(
        std::remove_if(m_constraints.begin(), m_constraints.end(),
            [&](const Constraint& k) {
                return (k.entityA >= 0 && valid.find(k.entityA) == valid.end()) ||
                       (k.entityB >= 0 && valid.find(k.entityB) == valid.end());
            }),
        m_constraints.end());

    return pruned;
}

void Sketch::clear() {
    m_points.clear();
    m_lines.clear();
    m_circles.clear();
    m_arcs.clear();
    m_splines.clear();
    m_polygons.clear();
    m_constraints.clear();
    m_nextId = 1;
    m_nextConstraintId = 1;
}

int Sketch::addConstraint(const Constraint& c) {
    Constraint copy = c;
    copy.id = m_nextConstraintId++;
    m_constraints.push_back(copy);
    return copy.id;
}

void Sketch::removeConstraint(int id) {
    m_constraints.erase(
        std::remove_if(m_constraints.begin(), m_constraints.end(),
            [id](const Constraint& c) { return c.id == id; }),
        m_constraints.end());
}

// Build OCCT wires from closed profiles.
//
// Adjacency graph spans lines AND arc-segments-of-circles. A circle that has no
// sketch points on its perimeter contributes a standalone full-circle wire. A
// circle that *does* have points on it gets split into arc segments at those
// points so the DFS can find closed loops mixing straight and curved edges.

std::vector<TopoDS_Wire> Sketch::buildWires() const {
    std::vector<TopoDS_Wire> wires;

    // Helper edge spec used by the adjacency walker
    struct EdgeSpec {
        bool isArc = false;
        int startPtId = -1;
        int endPtId = -1;
        // Arc-only fields:
        int circleId = -1;           // index into m_circles
        float startAngle = 0.0f;     // radians, around circle center
        float endAngle = 0.0f;       // radians, CCW from startAngle (may exceed 2pi)
        // Spline-only: index into m_splines. A spline participates in
        // adjacency as a single edge between its first and last control
        // points; emitOcctEdge interpolates a smooth B-spline through ALL
        // its control points.
        int splineIdx = -1;
    };
    std::vector<EdgeSpec> edges;

    // Coordinate table for every adjacency node: real sketch points plus the
    // synthetic crossing points generated just below. Keyed by id. Synthetic
    // ids start well past the live id counter so they can't collide with real
    // ones. emitOcctEdge() and the splitting loops below resolve coordinates
    // through this table rather than getPoint(), so synthetic nodes work too.
    std::unordered_map<int, glm::vec2> coord;
    for (const auto& pt : m_points) coord[pt.id] = pt.pos;
    int nextSynthId = m_nextId + 100000;

    const float onLineTol = 1e-2f;

    // Register a synthetic point at each proper line-line crossing (an
    // intersection strictly interior to BOTH segments). This lets closed shapes
    // formed by *crossing* lines be detected even when the user never placed a
    // vertex at the crossing — matching what the trim tool produces, but without
    // requiring a trim first. Crossings at shared endpoints are excluded (the
    // interior-parameter test rejects them), so already-working profiles like a
    // plain rectangle are untouched.
    {
        auto cross2 = [](glm::vec2 p, glm::vec2 q) { return p.x * q.y - p.y * q.x; };
        const int nL = static_cast<int>(m_lines.size());
        std::vector<glm::vec2> A(nL), B(nL);
        std::vector<bool> ok(nL, false);
        for (int i = 0; i < nL; ++i) {
            auto ia = coord.find(m_lines[i].startPointId);
            auto ib = coord.find(m_lines[i].endPointId);
            if (ia != coord.end() && ib != coord.end()) {
                A[i] = ia->second; B[i] = ib->second; ok[i] = true;
            }
        }
        const float pe = 1e-4f; // keep crossings off the segment endpoints
        for (int i = 0; i < nL; ++i) {
            if (!ok[i]) continue;
            for (int j = i + 1; j < nL; ++j) {
                if (!ok[j]) continue;
                glm::vec2 di = B[i] - A[i];
                glm::vec2 dj = B[j] - A[j];
                float den = cross2(di, dj);
                if (std::abs(den) < 1e-9f) continue; // parallel / collinear
                glm::vec2 off = A[j] - A[i];
                float t = cross2(off, dj) / den;
                float u = cross2(off, di) / den;
                if (t < pe || t > 1.0f - pe || u < pe || u > 1.0f - pe) continue;
                glm::vec2 ip = A[i] + t * di;
                // Reuse a nearby existing node (real or synthetic) so three lines
                // meeting at one point share a single graph node.
                bool dup = false;
                for (const auto& kv : coord) {
                    if (glm::length(kv.second - ip) < onLineTol) { dup = true; break; }
                }
                if (!dup) coord[nextSynthId++] = ip;
            }
        }
    }

    // Add line edges. If any point (real sketch point OR synthetic crossing)
    // lies on the interior of a line, emit sub-edges so adjacency can route
    // through that point. Without this, the line stays one edge from corner to
    // corner and any element ending mid-line can never form a closed region.
    for (const auto& line : m_lines) {
        auto ai = coord.find(line.startPointId);
        auto bi = coord.find(line.endPointId);
        if (ai == coord.end() || bi == coord.end()) {
            edges.push_back({false, line.startPointId, line.endPointId, -1, 0, 0});
            continue;
        }
        glm::vec2 aPos = ai->second;
        glm::vec2 dir = bi->second - aPos;
        float len2 = glm::dot(dir, dir);
        if (len2 < 1e-12f) {
            edges.push_back({false, line.startPointId, line.endPointId, -1, 0, 0});
            continue;
        }

        struct Onseg { float t; int id; };
        std::vector<Onseg> onseg;
        onseg.push_back({0.0f, line.startPointId});
        onseg.push_back({1.0f, line.endPointId});
        for (const auto& kv : coord) {
            if (kv.first == line.startPointId || kv.first == line.endPointId) continue;
            glm::vec2 v = kv.second - aPos;
            float t = glm::dot(v, dir) / len2;
            if (t < 1e-3f || t > 1.0f - 1e-3f) continue;
            glm::vec2 proj = aPos + t * dir;
            if (glm::length(kv.second - proj) > onLineTol) continue;
            onseg.push_back({t, kv.first});
        }
        std::sort(onseg.begin(), onseg.end(),
                  [](const Onseg& x, const Onseg& y) { return x.t < y.t; });
        for (size_t i = 0; i + 1 < onseg.size(); ++i) {
            edges.push_back({false, onseg[i].id, onseg[i + 1].id, -1, 0, 0});
        }
    }

    // For each circle, check for points lying on its perimeter
    const float perimeterEpsilon = 1e-3f;
    for (size_t ci = 0; ci < m_circles.size(); ++ci) {
        const auto& circle = m_circles[ci];
        const SketchPoint* center = getPoint(circle.centerPointId);
        if (!center) continue;

        struct OnPerim { int ptId; float angle; };
        std::vector<OnPerim> onPerim;
        for (const auto& pt : m_points) {
            if (pt.id == circle.centerPointId) continue;
            float d = glm::length(pt.pos - center->pos);
            if (std::abs(d - static_cast<float>(circle.radius)) < perimeterEpsilon) {
                float a = std::atan2(pt.pos.y - center->pos.y, pt.pos.x - center->pos.x);
                if (a < 0.0f) a += 2.0f * static_cast<float>(M_PI);
                onPerim.push_back({pt.id, a});
            }
        }

        if (onPerim.empty()) {
            // Standalone circle: emit a full-circle wire directly
            gp_Pnt center3d = sketchToWorld(center->pos);
            gp_Dir normal = m_plane.Position().Direction();
            gp_Circ gpCircle(gp_Ax2(center3d, normal), circle.radius);
            BRepBuilderAPI_MakeEdge edgeMaker(gpCircle);
            if (edgeMaker.IsDone()) {
                BRepBuilderAPI_MakeWire wireMaker(edgeMaker.Edge());
                if (wireMaker.IsDone()) wires.push_back(wireMaker.Wire());
            }
            continue;
        }

        // Split the circle into arc segments at perimeter points (CCW)
        std::sort(onPerim.begin(), onPerim.end(),
                  [](const OnPerim& a, const OnPerim& b){ return a.angle < b.angle; });
        for (size_t i = 0; i < onPerim.size(); ++i) {
            const OnPerim& cur = onPerim[i];
            const OnPerim& nxt = onPerim[(i + 1) % onPerim.size()];
            float endAng = nxt.angle;
            if (endAng <= cur.angle) endAng += 2.0f * static_cast<float>(M_PI);
            edges.push_back({true, cur.ptId, nxt.ptId, static_cast<int>(ci), cur.angle, endAng});
        }
    }

    // Existing arcs: include them in adjacency too. Same intermediate-point
    // split as for lines — any sketch point lying on the arc's perimeter
    // inside the arc's angle range becomes a virtual split, so adjacency can
    // route through it.
    for (const auto& arc : m_arcs) {
        const SketchPoint* center = getPoint(arc.centerPointId);
        const SketchPoint* s = getPoint(arc.startPointId);
        const SketchPoint* e = getPoint(arc.endPointId);
        if (!center || !s || !e) continue;
        float sA = std::atan2(s->pos.y - center->pos.y, s->pos.x - center->pos.x);
        float eA = std::atan2(e->pos.y - center->pos.y, e->pos.x - center->pos.x);
        if (sA < 0.0f) sA += 2.0f * static_cast<float>(M_PI);
        if (eA < 0.0f) eA += 2.0f * static_cast<float>(M_PI);
        if (eA <= sA) eA += 2.0f * static_cast<float>(M_PI);
        int arcEncoded = -2 - static_cast<int>(&arc - &m_arcs[0]);

        struct Onarc { float angle; int id; };
        std::vector<Onarc> onarc;
        onarc.push_back({sA, arc.startPointId});
        onarc.push_back({eA, arc.endPointId});
        for (const auto& pt : m_points) {
            if (pt.id == arc.startPointId || pt.id == arc.endPointId ||
                pt.id == arc.centerPointId) continue;
            float d = glm::length(pt.pos - center->pos);
            if (std::abs(d - static_cast<float>(arc.radius)) > perimeterEpsilon) continue;
            float a = std::atan2(pt.pos.y - center->pos.y, pt.pos.x - center->pos.x);
            if (a < 0.0f) a += 2.0f * static_cast<float>(M_PI);
            // Bring `a` into the [sA, sA + 2π) range so it can be compared against eA.
            while (a < sA - 1e-4f) a += 2.0f * static_cast<float>(M_PI);
            if (a < sA + 1e-3f || a > eA - 1e-3f) continue;
            onarc.push_back({a, pt.id});
        }
        std::sort(onarc.begin(), onarc.end(),
                  [](const Onarc& x, const Onarc& y) { return x.angle < y.angle; });
        for (size_t i = 0; i + 1 < onarc.size(); ++i) {
            edges.push_back({true, onarc[i].id, onarc[i + 1].id, arcEncoded,
                              onarc[i].angle, onarc[i + 1].angle});
        }
    }

    // Splines: one adjacency edge from first to last control point. A
    // CLOSED spline (first == last point, e.g. the user finished by
    // clicking the start point) forms a loop on its own and the walker
    // closes it immediately.
    for (size_t si = 0; si < m_splines.size(); ++si) {
        const auto& sp = m_splines[si];
        if (sp.isConstruction) continue;
        if (sp.controlPointIds.size() < 2) continue;
        EdgeSpec es;
        es.splineIdx = static_cast<int>(si);
        es.startPtId = sp.controlPointIds.front();
        es.endPtId = sp.controlPointIds.back();
        edges.push_back(es);
    }

    if (edges.empty()) return wires;

    // Prune dangling edges. An edge hanging off a free end — a vertex touched by
    // only one edge — can never be part of a closed loop, so iteratively drop
    // such edges until only the cycle core remains. This keeps the greedy walker
    // below from wandering down a tail, marking its edges used, failing to
    // close, and rolling back: e.g. a triangle drawn with three *crossing* lines
    // has six dangling tails past the corners that would otherwise trap it.
    // Profiles with no free ends (a plain rectangle, a circle) are unaffected.
    std::vector<bool> alive(edges.size(), true);
    {
        bool changed = true;
        while (changed) {
            changed = false;
            std::unordered_map<int, int> degree;
            for (size_t i = 0; i < edges.size(); ++i) {
                if (!alive[i]) continue;
                degree[edges[i].startPtId]++;
                degree[edges[i].endPtId]++;
            }
            for (size_t i = 0; i < edges.size(); ++i) {
                if (!alive[i]) continue;
                if (degree[edges[i].startPtId] < 2 || degree[edges[i].endPtId] < 2) {
                    alive[i] = false;
                    changed = true;
                }
            }
        }
    }

    // Adjacency: pointId -> list of surviving edge indices
    std::unordered_map<int, std::vector<int>> pointToEdges;
    for (size_t i = 0; i < edges.size(); ++i) {
        if (!alive[i]) continue;
        pointToEdges[edges[i].startPtId].push_back(static_cast<int>(i));
        pointToEdges[edges[i].endPtId].push_back(static_cast<int>(i));
    }

    std::unordered_set<int> usedEdges;

    auto otherEnd = [&](const EdgeSpec& e, int p) {
        return (e.startPtId == p) ? e.endPtId : e.startPtId;
    };

    auto emitOcctEdge = [&](const EdgeSpec& es, int fromPt, BRepBuilderAPI_MakeWire& wm) -> bool {
        // Resolve through the coord table so synthetic crossing nodes (which have
        // no SketchPoint) work alongside real points.
        auto sc = coord.find(es.startPtId);
        auto ec = coord.find(es.endPtId);
        if (sc == coord.end() || ec == coord.end()) return false;
        gp_Pnt p1 = sketchToWorld(sc->second);
        gp_Pnt p2 = sketchToWorld(ec->second);
        if (p1.Distance(p2) < 1e-10 && !es.isArc && es.splineIdx < 0)
            return false; // (a CLOSED spline legitimately starts where it ends)

        if (es.splineIdx >= 0) {
            // The SAME centripetal Catmull-Rom curve the renderer draws,
            // densely sampled and fitted with a B-spline — what you see is
            // what extrudes. Walked in the chain's direction.
            const SketchSpline& sp = m_splines[es.splineIdx];
            std::vector<glm::vec2> samp = sampleSpline2D(sp, 24);
            if (samp.size() < 2) return false;
            bool closedSp = sp.controlPointIds.size() > 2 &&
                            sp.controlPointIds.front() ==
                                sp.controlPointIds.back();
            if (fromPt == es.endPtId && !closedSp)
                std::reverse(samp.begin(), samp.end());
            try {
                TColgp_Array1OfPnt arr(1, static_cast<int>(samp.size()));
                for (size_t k = 0; k < samp.size(); ++k)
                    arr.SetValue(static_cast<int>(k) + 1,
                                 sketchToWorld(samp[k]));
                GeomAPI_PointsToBSpline fit(arr, 3, 8, GeomAbs_C2, 1.0e-3);
                if (!fit.IsDone()) return false;
                BRepBuilderAPI_MakeEdge mk(fit.Curve());
                if (!mk.IsDone()) return false;
                wm.Add(mk.Edge());
                return true;
            } catch (...) { return false; }
        }

        if (!es.isArc) {
            BRepBuilderAPI_MakeEdge mk(p1, p2);
            if (!mk.IsDone()) return false;
            wm.Add(mk.Edge());
            return true;
        }

        // Arc segment: figure out center/radius from either a circle index or an arc index
        glm::vec2 center2d(0); double radius = 0.0;
        if (es.circleId >= 0 && es.circleId < static_cast<int>(m_circles.size())) {
            const SketchCircle& c = m_circles[es.circleId];
            const SketchPoint* cp = getPoint(c.centerPointId);
            if (!cp) return false;
            center2d = cp->pos;
            radius = c.radius;
        } else {
            int arcIdx = -es.circleId - 2;
            if (arcIdx < 0 || arcIdx >= static_cast<int>(m_arcs.size())) return false;
            const SketchArc& a = m_arcs[arcIdx];
            const SketchPoint* cp = getPoint(a.centerPointId);
            if (!cp) return false;
            center2d = cp->pos;
            radius = a.radius;
        }

        // Walk direction: if we're entering from startPtId go CCW (start->end angles),
        // otherwise go the other way.
        float aStart = es.startAngle;
        float aEnd = es.endAngle;
        if (fromPt == es.endPtId) std::swap(aStart, aEnd);
        float aMid = 0.5f * (aStart + aEnd);
        glm::vec2 mid2d(center2d.x + std::cos(aMid) * static_cast<float>(radius),
                        center2d.y + std::sin(aMid) * static_cast<float>(radius));
        gp_Pnt mid3d = sketchToWorld(mid2d);

        // p1 must correspond to fromPt and p2 to the other end
        gp_Pnt fromPnt = (fromPt == es.startPtId) ? p1 : p2;
        gp_Pnt toPnt   = (fromPt == es.startPtId) ? p2 : p1;

        GC_MakeArcOfCircle arcMaker(fromPnt, mid3d, toPnt);
        if (!arcMaker.IsDone()) return false;
        BRepBuilderAPI_MakeEdge mk(arcMaker.Value());
        if (!mk.IsDone()) return false;
        wm.Add(mk.Edge());
        return true;
    };

    for (size_t startIdx = 0; startIdx < edges.size(); ++startIdx) {
        if (!alive[startIdx]) continue;
        if (usedEdges.count(static_cast<int>(startIdx))) continue;

        std::vector<int> chain;
        chain.push_back(static_cast<int>(startIdx));
        usedEdges.insert(static_cast<int>(startIdx));

        int firstPointId = edges[startIdx].startPtId;
        int currentPointId = edges[startIdx].endPtId;

        bool closed = false;
        bool extended = true;
        while (extended && !closed) {
            extended = false;
            if (currentPointId == firstPointId) { closed = true; break; }
            auto it = pointToEdges.find(currentPointId);
            if (it == pointToEdges.end()) break;
            for (int idx : it->second) {
                if (usedEdges.count(idx)) continue;
                int nextPt = otherEnd(edges[idx], currentPointId);
                if (nextPt < 0) continue;
                chain.push_back(idx);
                usedEdges.insert(idx);
                currentPointId = nextPt;
                extended = true;
                break;
            }
        }

        if (!closed) {
            for (int idx : chain) usedEdges.erase(idx);
            continue;
        }

        // Emit OCCT wire from the chain
        BRepBuilderAPI_MakeWire wireMaker;
        int curPt = firstPointId;
        bool valid = true;
        for (int idx : chain) {
            const EdgeSpec& es = edges[idx];
            // A zero-length line (degenerate input — duplicate consecutive
            // points) contributes no curve. SKIP it instead of failing:
            // failing here used to silently drop the ENTIRE closed wire,
            // which is how SVG circles with degenerate joint cubics
            // vanished from region detection.
            if (!es.isArc && es.splineIdx < 0) {
                auto sc = coord.find(es.startPtId);
                auto ec = coord.find(es.endPtId);
                if (sc != coord.end() && ec != coord.end() &&
                    glm::length(sc->second - ec->second) < 1e-6f) {
                    curPt = otherEnd(es, curPt);
                    continue;
                }
            }
            if (!emitOcctEdge(es, curPt, wireMaker)) { valid = false; break; }
            curPt = otherEnd(es, curPt);
        }

        if (valid && wireMaker.IsDone()) wires.push_back(wireMaker.Wire());
    }

    return wires;
}

namespace {

// Sample a 2D point on a wire (used as a "representative" for containment tests).
// We pick the midpoint of the first edge in sketch-plane coordinates.
glm::vec2 sampleWirePoint(const TopoDS_Wire& wire, const gp_Pln& plane) {
    gp_Ax3 ax = plane.Position();
    gp_Pnt origin = ax.Location();
    gp_Dir xd = ax.XDirection();
    gp_Dir yd = ax.YDirection();

    auto toSketch2D = [&](const gp_Pnt& w) {
        gp_Vec v(origin, w);
        return glm::vec2(static_cast<float>(v.Dot(gp_Vec(xd))),
                         static_cast<float>(v.Dot(gp_Vec(yd))));
    };

    BRepTools_WireExplorer ex(wire);
    if (!ex.More()) return glm::vec2(0);
    TopoDS_Edge edge = TopoDS::Edge(ex.Current());
    double f, l;
    Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
    if (curve.IsNull()) return glm::vec2(0);
    gp_Pnt mid;
    curve->D0((f + l) * 0.5, mid);
    return toSketch2D(mid);
}

// Densely sample a wire's perimeter into 2D sketch-plane points.
void densifyWire2D(const TopoDS_Wire& wire, const gp_Pln& plane,
                   std::vector<glm::vec2>& out, int samplesPerEdge = 32) {
    gp_Ax3 ax = plane.Position();
    gp_Pnt origin = ax.Location();
    gp_Dir xd = ax.XDirection();
    gp_Dir yd = ax.YDirection();
    auto toSketch2D = [&](const gp_Pnt& w) {
        gp_Vec v(origin, w);
        return glm::vec2(static_cast<float>(v.Dot(gp_Vec(xd))),
                         static_cast<float>(v.Dot(gp_Vec(yd))));
    };

    for (BRepTools_WireExplorer ex(wire); ex.More(); ex.Next()) {
        TopoDS_Edge edge = TopoDS::Edge(ex.Current());
        double f, l;
        Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
        if (curve.IsNull()) continue;
        for (int i = 0; i < samplesPerEdge; ++i) {
            double t = f + (l - f) * (double(i) / samplesPerEdge);
            gp_Pnt p;
            curve->D0(t, p);
            out.push_back(toSketch2D(p));
        }
    }
}

// Standard ray-cast point-in-polygon (winding-independent, counts crossings).
bool pointInPolygon2D(const std::vector<glm::vec2>& poly, glm::vec2 p) {
    bool inside = false;
    size_t n = poly.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto& a = poly[i];
        const auto& b = poly[j];
        bool intersect = ((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y + 1e-30f) + a.x);
        if (intersect) inside = !inside;
    }
    return inside;
}

} // anonymous

std::vector<Sketch::Region> Sketch::buildRegions() const {
    const uint64_t h = geometryHash();
    if (m_regionCacheValid && h == m_regionHash) return m_regionCache;
    m_regionCache = buildRegionsUncached();
    m_regionHash = h;
    m_regionCacheValid = true;
    return m_regionCache;
}

// FNV-1a over everything region construction depends on. ~10 µs on a text
// sketch — noise next to the general fuse this guards (tens of ms).
uint64_t Sketch::geometryHash() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    auto mixI = [&](int v) { mix(&v, sizeof v); };
    auto mixF = [&](float v) { mix(&v, sizeof v); };
    auto mixD = [&](double v) { mix(&v, sizeof v); };

    const gp_Ax3& ax = m_plane.Position();
    mixD(ax.Location().X()); mixD(ax.Location().Y()); mixD(ax.Location().Z());
    mixD(ax.Direction().X()); mixD(ax.Direction().Y()); mixD(ax.Direction().Z());
    mixD(ax.XDirection().X()); mixD(ax.XDirection().Y()); mixD(ax.XDirection().Z());

    for (const auto& p : m_points) {
        mixI(p.id); mixF(p.pos.x); mixF(p.pos.y);
        mixI(p.isConstruction ? 1 : 0);
    }
    for (const auto& l : m_lines) {
        mixI(l.id); mixI(l.startPointId); mixI(l.endPointId);
        mixI(l.isConstruction ? 1 : 0);
    }
    for (const auto& c : m_circles) {
        mixI(c.id); mixI(c.centerPointId); mixD(c.radius);
        mixI(c.isConstruction ? 1 : 0);
    }
    for (const auto& a : m_arcs) {
        mixI(a.id); mixI(a.centerPointId); mixI(a.startPointId);
        mixI(a.endPointId); mixD(a.radius);
    }
    for (const auto& s : m_splines) {
        mixI(s.id);
        for (int id : s.controlPointIds) mixI(id);
    }
    for (const auto& g : m_polygons) {
        mixI(g.id); mixI(g.centerPointId); mixD(g.radius); mixI(g.sides);
    }
    mixI(m_sourceBodyId);
    // Source face identity: the TShape pointer changes whenever the host
    // body is regenerated, which is exactly when grafted holes can change.
    const void* tsh =
        m_sourceFace.IsNull() ? nullptr : m_sourceFace.TShape().get();
    mix(&tsh, sizeof tsh);
    // Plane pose: the 2D geometry projects to 3D through the plane, so the
    // cached 3D regions must rebuild when the sketch moves (e.g. Move Face
    // translates the plane) even though the 2D ids are unchanged.
    {
        gp_Pnt o = m_plane.Location();
        gp_Dir z = m_plane.Axis().Direction();
        gp_Dir x = m_plane.XAxis().Direction();
        double pv[9] = { o.X(), o.Y(), o.Z(), z.X(), z.Y(), z.Z(),
                         x.X(), x.Y(), x.Z() };
        mix(pv, sizeof pv);
    }
    return h;
}

TopoDS_Shape Sketch::buildProfileShape() const {
    auto wires = buildWires();
    if (wires.empty()) return TopoDS_Shape();
    const size_t n = wires.size();

    // Containment matrix from densified 2D polygons. n is small (a busy
    // SVG is a few dozen wires); the n² point-in-polygon pass is trivial
    // next to the boolean work below.
    std::vector<std::vector<glm::vec2>> polys(n);
    for (size_t i = 0; i < n; ++i)
        densifyWire2D(wires[i], m_plane, polys[i]);

    std::vector<std::vector<bool>> inside(n, std::vector<bool>(n, false));
    std::vector<int> depth(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (polys[i].size() < 3) continue;
        const size_t stepI = std::max<size_t>(1, polys[i].size() / 24);
        for (size_t j = 0; j < n; ++j) {
            if (i == j || polys[j].size() < 3) continue;
            // i ⊂ j  iff MOST of i's boundary points lie inside j. The old test
            // used a single vertex (polys[i][0]), which flips on float luck when
            // a counter grazes its glyph's edge — that's why A/R counters were
            // detected inconsistently. Sampling many points is robust, and keeps
            // concentric rings right (an outer ring's points sit OUTSIDE its
            // own counter, so it's never nested in its own hole).
            size_t in = 0, tested = 0;
            for (size_t k = 0; k < polys[i].size(); k += stepI) {
                ++tested;
                if (pointInPolygon2D(polys[j], polys[i][k])) ++in;
            }
            if (tested && in * 2 > tested) { inside[i][j] = true; depth[i]++; }
        }
    }
    // Direct parent of an odd-depth wire: its container one level up.
    std::vector<int> parent(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 0) continue;
        for (size_t j = 0; j < n; ++j) {
            if (inside[i][j] && depth[j] == depth[i] - 1) {
                parent[i] = static_cast<int>(j);
                break;
            }
        }
    }

    // One face per island; holes removed by boolean cut (no wire-winding
    // coordination — the lesson from the projection op's aperture bug).
    auto wireFace = [&](TopoDS_Wire w) -> TopoDS_Face {
        for (int attempt = 0; attempt < 2; ++attempt) {
            BRepBuilderAPI_MakeFace mf(m_plane, w);
            if (!mf.IsDone()) return TopoDS_Face();
            ShapeFix_Face fix(mf.Face());
            fix.FixOrientationMode() = 1;
            fix.FixWireMode() = 1;
            fix.Perform();
            TopoDS_Face cand = fix.Face();
            GProp_GProps g;
            BRepGProp::SurfaceProperties(cand, g);
            if (g.Mass() > 0.0 && BRepCheck_Analyzer(cand).IsValid())
                return cand;
            w.Reverse();
        }
        return TopoDS_Face();
    };

    TopoDS_Compound comp;
    BRep_Builder bb;
    bb.MakeCompound(comp);
    int islands = 0;
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 != 0) continue; // hole, consumed by its parent
        TopoDS_Face outer = wireFace(wires[i]);
        if (outer.IsNull()) continue;
        TopTools_ListOfShape holeFaces;
        for (size_t j = 0; j < n; ++j) {
            if (parent[j] != static_cast<int>(i)) continue;
            TopoDS_Face hf = wireFace(wires[j]);
            if (!hf.IsNull()) holeFaces.Append(hf);
        }
        TopoDS_Shape island = outer;
        if (!holeFaces.IsEmpty()) {
            try {
                BRepAlgoAPI_Cut cut;
                TopTools_ListOfShape args;
                args.Append(outer);
                cut.SetArguments(args);
                cut.SetTools(holeFaces);
                cut.Build();
                if (cut.IsDone() && !cut.Shape().IsNull())
                    island = cut.Shape();
            } catch (...) {}
        }
        for (TopExp_Explorer fx(island, TopAbs_FACE); fx.More(); fx.Next()) {
            bb.Add(comp, fx.Current());
            islands++;
        }
    }
    if (islands == 0) return TopoDS_Shape();
    return comp;
}

std::vector<Sketch::Region> Sketch::buildRegionsUncached() const {
    std::vector<Region> regions;

    // Build a planar face from every closed wire the sketch forms. Each
    // sketch face is then augmented with any holes from the source face
    // that fall fully inside it — BOPAlgo_Builder doesn't split coplanar
    // faces when their edges don't intersect, so a sketch drawn AROUND an
    // existing hole would otherwise come out as a solid disk and a
    // push/pull would yield a solid bar instead of a tube.
    auto wires = buildWires();
    std::vector<TopoDS_Wire> sourceHoleWires;
    TopoDS_Wire sourceOuterWire;
    if (!m_sourceFace.IsNull()) {
        sourceOuterWire = BRepTools::OuterWire(m_sourceFace);
        for (TopExp_Explorer we(m_sourceFace, TopAbs_WIRE); we.More(); we.Next()) {
            TopoDS_Wire w = TopoDS::Wire(we.Current());
            if (!sourceOuterWire.IsNull() && !w.IsSame(sourceOuterWire)) {
                sourceHoleWires.push_back(w);
            }
        }
    }
    // Densify a wire to 2D sketch-plane points so we can do polygon-in-polygon tests.
    auto wireTo2D = [&](const TopoDS_Wire& w) -> std::vector<glm::vec2> {
        std::vector<glm::vec2> poly;
        densifyWire2D(w, m_plane, poly);
        return poly;
    };
    std::vector<std::vector<glm::vec2>> holePolys;
    holePolys.reserve(sourceHoleWires.size());
    for (const auto& hw : sourceHoleWires) holePolys.push_back(wireTo2D(hw));

    std::vector<TopoDS_Face> faces;
    for (const auto& w : wires) {
        BRepBuilderAPI_MakeFace mf(m_plane, w);
        if (!mf.IsDone()) continue;
        TopoDS_Face f = mf.Face();
        // Subtract any source-face holes that lie fully inside this sketch face.
        std::vector<glm::vec2> outerPoly = wireTo2D(w);
        int grafted = 0;
        for (size_t i = 0; i < sourceHoleWires.size(); ++i) {
            const auto& holePoly = holePolys[i];
            if (holePoly.empty()) continue;
            // Sample test: every densified point of the hole must lie inside
            // the sketch's outer polygon. (densifyWire2D produces enough
            // samples that a clean "all inside" check is reliable.)
            bool allInside = true;
            for (const auto& p : holePoly) {
                if (!pointInPolygon2D(outerPoly, p)) { allInside = false; break; }
            }
            if (!allInside) continue;
            // Graft this hole into the sketch face as an inner wire.
            BRepBuilderAPI_MakeFace addHole(f);
            addHole.Add(TopoDS::Wire(sourceHoleWires[i].Reversed()));
            if (addHole.IsDone()) {
                f = addHole.Face();
                ++grafted;
            }
        }
        faces.push_back(f);
        (void)grafted; // counter retained in case a debug log is re-added later
    }
    // Include the source face so leftover area outside the drawn shapes
    // (but inside the host face) is also selectable.
    if (!m_sourceFace.IsNull()) {
        faces.push_back(m_sourceFace);
    }

    if (faces.empty()) return regions;

    // Partition the union of all faces into atomic, non-overlapping regions.
    // General-fusing the faces splits every overlap into its own face — the lens
    // where two shapes cross, the surrounding crescents, an annulus where one
    // shape sits inside another — so each piece can be selected and push/pulled
    // independently. (This is what makes intersecting shapes selectable.) A lone
    // face has nothing to fuse, and a one-argument general fuse is invalid, so
    // use it directly in that case.
    std::vector<TopoDS_Face> atomic;
    if (faces.size() == 1) {
        atomic.push_back(faces.front());
    } else {
        try {
            BOPAlgo_Builder gf;
            for (const auto& f : faces) gf.AddArgument(f);
            gf.Perform();
            if (!gf.HasErrors()) {
                for (TopExp_Explorer ex(gf.Shape(), TopAbs_FACE); ex.More(); ex.Next())
                    atomic.push_back(TopoDS::Face(ex.Current()));
            }
        } catch (...) {}
        if (atomic.empty()) atomic = faces; // fall back to the un-fused faces
    }

    // Split those faces by every sketch line, so an open interior line that
    // divides a region (e.g. a wall splitting a room) yields separate selectable
    // cells. Lines that merely trace a face boundary are no-ops here; only lines
    // crossing a face's interior actually subdivide it. (GF above handles closed
    // overlaps/holes; this handles open dividing lines.)
    {
        TopTools_ListOfShape toolEdges;
        for (const auto& line : m_lines) {
            const SketchPoint* a = getPoint(line.startPointId);
            const SketchPoint* b = getPoint(line.endPointId);
            if (!a || !b) continue;
            gp_Pnt p1 = sketchToWorld(a->pos);
            gp_Pnt p2 = sketchToWorld(b->pos);
            if (p1.Distance(p2) < 1e-9) continue;
            BRepBuilderAPI_MakeEdge mk(p1, p2);
            if (mk.IsDone()) toolEdges.Append(mk.Edge());
        }
        if (!toolEdges.IsEmpty() && !atomic.empty()) {
            try {
                BRepAlgoAPI_Splitter sp;
                TopTools_ListOfShape args;
                for (const auto& f : atomic) args.Append(f);
                sp.SetArguments(args);
                sp.SetTools(toolEdges);
                sp.Build();
                if (!sp.HasErrors()) {
                    std::vector<TopoDS_Face> split;
                    for (TopExp_Explorer ex(sp.Shape(), TopAbs_FACE); ex.More(); ex.Next())
                        split.push_back(TopoDS::Face(ex.Current()));
                    if (!split.empty()) atomic.swap(split);
                }
            } catch (...) {}
        }
    }

    // Project a 3D point onto sketch-plane 2D coordinates.
    const gp_Ax3& ax = m_plane.Position();
    gp_Pnt planeOrigin = ax.Location();
    gp_Dir xd = ax.XDirection();
    gp_Dir yd = ax.YDirection();
    auto toSketch2D = [&](const gp_Pnt& w) {
        gp_Vec v(planeOrigin, w);
        return glm::vec2(static_cast<float>(v.Dot(gp_Vec(xd))),
                         static_cast<float>(v.Dot(gp_Vec(yd))));
    };

    for (const auto& f : atomic) {
        Region region;
        region.face = f;
        region.outerWire = BRepTools::OuterWire(f);
        for (TopExp_Explorer wexp(f, TopAbs_WIRE); wexp.More(); wexp.Next()) {
            TopoDS_Wire w = TopoDS::Wire(wexp.Current());
            if (!region.outerWire.IsNull() && !w.IsSame(region.outerWire))
                region.holeWires.push_back(w);
        }

        // Representative point: the face's area centroid in 2D. For a ring/annulus
        // the centroid can fall in the hole, so fall back to a point on the outer
        // wire when the centroid isn't actually inside the region.
        glm::vec2 rep(0);
        try {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f, props);
            rep = toSketch2D(props.CentreOfMass());
        } catch (...) {}
        if (region.outerWire.IsNull() || !isPointInRegion(region, rep)) {
            if (!region.outerWire.IsNull()) rep = sampleWirePoint(region.outerWire, m_plane);
        }
        region.representativePoint = rep;

        regions.push_back(std::move(region));
    }

    return regions;
}

bool Sketch::isPointInRegion(const Region& region, glm::vec2 p) const {
    std::vector<glm::vec2> outerPoly;
    densifyWire2D(region.outerWire, m_plane, outerPoly);
    if (!pointInPolygon2D(outerPoly, p)) return false;
    for (const auto& h : region.holeWires) {
        std::vector<glm::vec2> holePoly;
        densifyWire2D(h, m_plane, holePoly);
        if (pointInPolygon2D(holePoly, p)) return false;
    }
    return true;
}

bool Sketch::isPointInOrNearRegion(const Region& region, glm::vec2 p, float tol) const {
    if (isPointInRegion(region, p)) return true;
    if (tol <= 0.0f) return false;

    auto distToSeg = [](glm::vec2 q, glm::vec2 a, glm::vec2 b) {
        glm::vec2 ab = b - a;
        float len2 = glm::dot(ab, ab);
        float t = len2 > 1e-12f ? glm::clamp(glm::dot(q - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        return glm::length(q - (a + t * ab));
    };
    auto nearWire = [&](const TopoDS_Wire& w) {
        std::vector<glm::vec2> poly;
        densifyWire2D(w, m_plane, poly);
        for (size_t i = 0; i + 1 < poly.size(); ++i) {
            if (distToSeg(p, poly[i], poly[i + 1]) <= tol) return true;
        }
        return false;
    };
    if (nearWire(region.outerWire)) return true;
    for (const auto& h : region.holeWires) {
        if (nearWire(h)) return true;
    }
    return false;
}

bool Sketch::getSourceFaceCentroid(glm::vec2& out) const {
    if (m_sourceFace.IsNull()) return false;
    if (m_centroidValid) { out = m_centroid; return true; }

    // Let OCCT compute the true area centroid (centre of mass for a uniform
    // density surface). Doing this ourselves from densified polygon vertices is
    // fragile for faces whose outer wire has any reversed edges — the resulting
    // vertex sequence is scrambled and the polygon-area formula returns garbage.
    try {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(m_sourceFace, props);
        if (props.Mass() <= 0.0) return false;
        gp_Pnt c3d = props.CentreOfMass();
        // Project to sketch-plane 2D coordinates.
        const gp_Ax3& ax = m_plane.Position();
        gp_Pnt origin = ax.Location();
        gp_Dir xd = ax.XDirection();
        gp_Dir yd = ax.YDirection();
        gp_Vec v(origin, c3d);
        m_centroid = glm::vec2(static_cast<float>(v.Dot(gp_Vec(xd))),
                               static_cast<float>(v.Dot(gp_Vec(yd))));
        m_centroidValid = true;
        out = m_centroid;
        return true;
    } catch (...) { return false; }
}

int Sketch::elementCount() const {
    return static_cast<int>(m_lines.size() + m_circles.size() + m_arcs.size() +
                            m_splines.size() + m_polygons.size());
}

int Sketch::pointCount() const {
    return static_cast<int>(m_points.size());
}

} // namespace materializr
