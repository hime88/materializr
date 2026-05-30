#include "Sketch.h"

#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BOPAlgo_Builder.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
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

int Sketch::addPoint(glm::vec2 pos) {
    SketchPoint pt;
    pt.id = nextId();
    pt.pos = pos;
    pt.isConstruction = false;
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

int Sketch::addLine(int startPtId, int endPtId) {
    SketchLine line;
    line.id = nextId();
    line.startPointId = startPtId;
    line.endPointId = endPtId;
    line.isConstruction = false;
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

    // Create N vertex points evenly spaced around center. The first vertex
    // lands at angle `rotationRad` so the caller can align it with the cursor
    // direction — used by SketchTool to snap a corner of the polygon to the
    // grid (the first vertex is exactly under the snapped cursor).
    for (int i = 0; i < sides; ++i) {
        double angle = rotationRad + 2.0 * M_PI * i / sides;
        float vx = center->pos.x + static_cast<float>(radius * std::cos(angle));
        float vy = center->pos.y + static_cast<float>(radius * std::sin(angle));
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

void Sketch::clear() {
    m_points.clear();
    m_lines.clear();
    m_circles.clear();
    m_arcs.clear();
    m_splines.clear();
    m_polygons.clear();
    m_nextId = 1;
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
        if (p1.Distance(p2) < 1e-10 && !es.isArc) return false;

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
    std::vector<Region> regions;

    // Build a planar face from every closed wire the sketch forms.
    auto wires = buildWires();
    std::vector<TopoDS_Face> faces;
    for (const auto& w : wires) {
        BRepBuilderAPI_MakeFace mf(m_plane, w);
        if (mf.IsDone()) faces.push_back(mf.Face());
    }
    // If the sketch was started on an existing face, include it so the leftover
    // area outside the drawn shapes (but inside the host face) is also selectable.
    if (!m_sourceFace.IsNull()) faces.push_back(m_sourceFace);

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
