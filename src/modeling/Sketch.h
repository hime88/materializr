#pragma once
#include "SketchConstraints.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

namespace materializr {

enum class SketchElementType { Point, Line, Circle, Arc, Rectangle, Spline, Polygon };

struct SketchPoint {
    int id;
    glm::vec2 pos;
    bool isConstruction = false;
    // Glyph-outline geometry from the Text tool. Still forms regions and is
    // still selectable, but hidden from vertex markers and snap/inference —
    // a five-letter word carries hundreds of vertices, and drawing anywhere
    // near it became impossible when every one was a snap target.
    bool fromText = false;
};

struct SketchLine {
    int id;
    int startPointId;
    int endPointId;
    bool isConstruction = false;
    bool fromText = false; // see SketchPoint::fromText
};

struct SketchCircle {
    int id;
    int centerPointId;
    double radius;
    bool isConstruction = false;
};

struct SketchArc {
    int id;
    int centerPointId;
    int startPointId;
    int endPointId;
    double radius;
    bool isConstruction = false;
};

struct SketchSpline {
    int id;
    std::vector<int> controlPointIds;
    bool isConstruction = false;
};

struct SketchPolygon {
    int id;
    int centerPointId;
    double radius;
    int sides;
    std::vector<int> vertexPointIds; // generated vertices
    std::vector<int> lineIds;        // generated lines
    bool isConstruction = false;
};

class Sketch {
public:
    Sketch();
    ~Sketch() = default;

    // Plane this sketch is on
    void setPlane(const gp_Pln& plane);
    const gp_Pln& getPlane() const;

    // Point management
    int addPoint(glm::vec2 pos, bool fromText = false);
    void movePoint(int id, glm::vec2 pos);
    const SketchPoint* getPoint(int id) const;
    const std::vector<SketchPoint>& getPoints() const;

    // Element creation
    int addLine(int startPtId, int endPtId, bool fromText = false);
    int addCircle(int centerPtId, double radius);
    int addArc(int centerPtId, int startPtId, int endPtId, double radius);
    int addRectangle(glm::vec2 corner1, glm::vec2 corner2); // returns first line id

    // Spline and polygon creation
    int addSpline(const std::vector<int>& controlPointIds);
    int addPolygon(int centerPtId, double radius, int sides, double rotationRad = 0.0);

    // Element access
    const std::vector<SketchLine>& getLines() const;
    const std::vector<SketchCircle>& getCircles() const;
    const std::vector<SketchArc>& getArcs() const;
    const std::vector<SketchSpline>& getSplines() const;
    const std::vector<SketchPolygon>& getPolygons() const;

    // Sample the smooth interpolated curve of a spline as sketch-2D points
    // (the same B-spline buildWires() emits, so what you see is what
    // extrudes). Falls back to the raw control polyline if interpolation
    // fails. `segsPerSpan` segments between consecutive control points.
    std::vector<glm::vec2> sampleSpline2D(const SketchSpline& sp,
                                          int segsPerSpan = 12) const;
    // Interpolate a smooth curve through raw 2D positions (no sketch points
    // needed) — used for the live in-progress spline preview. Falls back to
    // the input polyline on failure.
    static std::vector<glm::vec2> interpolate2D(
        const std::vector<glm::vec2>& ctrl, int segsPerSpan = 12,
        bool closed = false);

    // Element removal
    void removeElement(int id);
    // Remove points that no geometry references any more (e.g. a line's two
    // endpoints after the line itself is deleted), plus any constraint left
    // dangling by the removal. Returns the number of orphan points pruned.
    // Call after deleting elements; removeElement deliberately does NOT prune
    // (some callers remove a point by id directly).
    int pruneOrphanPoints();
    void clear();

    // Convert closed profiles to OCCT wires for extrusion
    std::vector<TopoDS_Wire> buildWires() const;

    // Whole-profile shape for "extrude everything this sketch encloses":
    // closed wires grouped even-odd by containment depth — every
    // even-depth wire is an island outer, its directly-contained
    // odd-depth wires are holes. Returns a compound of faces (one per
    // island), or a null shape when no closed wires exist. This replaced
    // the old single-face "largest wire + everything else as holes"
    // construction, which fed OCCT inverted/disjoint holes on multi-shape
    // sketches (SVG imports, text) and extruded non-manifold garbage.
    TopoDS_Shape buildProfileShape() const;

    // A planar region of the sketch (one outer boundary + zero or more holes).
    // Built from the closed wires via 2D containment analysis.
    struct Region {
        TopoDS_Wire outerWire;
        std::vector<TopoDS_Wire> holeWires; // already reversed for Add()
        TopoDS_Face face;                   // planar face on the sketch plane
        glm::vec2 representativePoint;      // a point guaranteed inside the region (2D sketch space)
    };
    // Cached: region construction runs an OCCT general fuse + splitter,
    // far too heavy for the per-frame hover pick that calls it (a text
    // sketch carries hundreds of glyph edges). The cache revalidates via a
    // cheap geometry hash, so the dozens of mutators don't each need to
    // remember an invalidate call.
    std::vector<Region> buildRegions() const;

    // True when buildRegions() would be a cache HIT (valid cache + current
    // geometry hash). Lets the per-frame hover pick skip sketches whose
    // regions would need the heavy OCCT fuse — a freshly-unhidden complex
    // sketch otherwise turns the first hover into a seconds-long stall.
    bool regionsCached() const;

    // 2D point-in-region test (sketch-space coordinates)
    bool isPointInRegion(const Region& region, glm::vec2 p) const;

    // As isPointInRegion, but also returns true when p lies within `tol` of the
    // region's boundary. Used so clicks just on/near the lines still select the
    // region, widening the otherwise pixel-thin boundary catch area.
    bool isPointInOrNearRegion(const Region& region, glm::vec2 p, float tol) const;

    // Originating face / body — set when the sketch was started on a planar face.
    // -1 means the sketch is on a freestanding plane (e.g. world XY).
    void setSourceBody(int bodyId) { m_sourceBodyId = bodyId; }
    int getSourceBody() const { return m_sourceBodyId; }

    // Parametric link state. When true, this sketch no longer drives the body
    // it created — set when the user moves the sketch (or its body) on its own
    // in 3D, deliberately breaking them out of unison. The sketch-edit cascade
    // skips detached sketches, so editing one won't retro-modify the body.
    // Cleared when the sketch and body are moved together (re-linked).
    void setDetachedFromBody(bool d) { m_detached = d; }
    bool isDetachedFromBody() const { return m_detached; }
    void setSourceFace(const TopoDS_Face& face) { m_sourceFace = face; m_centroidValid = false; }
    const TopoDS_Face& getSourceFace() const { return m_sourceFace; }

    // Area centroid of the host face's outer wire, projected into sketch-plane
    // 2D. Returns false if there is no source face. Computed once and cached.
    bool getSourceFaceCentroid(glm::vec2& out) const;

    // Reference geometry pulled from the source face on sketch entry — the
    // face's corner vertices, edge endpoints, edge midpoints, and straight
    // edges (start/end pairs) projected into sketch-plane 2D. The inference
    // snap reads these so the cursor can land on a 3D face's corners/edges
    // even when there are no equivalent sketch elements yet.
    struct FaceReference {
        std::vector<glm::vec2> points;
        std::vector<std::pair<glm::vec2, glm::vec2>> lines;
        // In-plane circular / arc edges (hole rims, fillet arcs) from the host
        // face or any coaxial in-plane neighbour edge. Stored as true circles so
        // the snap engine can land continuously on the perimeter rather than on
        // a handful of sampled points. A full circle has sweep == 2*PI; an arc's
        // live span is [startAngle, startAngle + sweep] measured CCW in the
        // sketch-plane 2D frame.
        struct Circle {
            glm::vec2 center;
            float radius;
            float startAngle;
            float sweep;
        };
        std::vector<Circle> circles;
    };
    void setFaceReferences(FaceReference refs) { m_faceRefs = std::move(refs); }
    const FaceReference& getFaceReferences() const { return m_faceRefs; }

    // Sketch state
    std::string getName() const { return m_name; }
    void setName(const std::string& name) { m_name = name; }

    int elementCount() const;
    int pointCount() const;

    // World-space axis-aligned bounds of all sketch geometry (points expanded
    // by circle radii). Returns false when the sketch has no points, leaving
    // out* untouched. Used to frame an in-progress sketch — it lives outside
    // the Document, so the usual body-bbox path can't see it.
    bool getWorldBounds(glm::vec3& outMin, glm::vec3& outMax) const;

    // --- Serialization helpers (used by ProjectIO) ---
    // Append elements directly, preserving their stored ids and construction
    // flags, so a sketch can be reloaded exactly as it was saved.
    void addRawPoint(const SketchPoint& p)     { m_points.push_back(p); }
    void addRawLine(const SketchLine& l)       { m_lines.push_back(l); }
    void addRawCircle(const SketchCircle& c)   { m_circles.push_back(c); }
    void addRawArc(const SketchArc& a)         { m_arcs.push_back(a); }
    void addRawSpline(const SketchSpline& s)   { m_splines.push_back(s); }
    void addRawPolygon(const SketchPolygon& p) { m_polygons.push_back(p); }
    void setNextId(int n) { m_nextId = n; }
    int  getNextId() const { return m_nextId; }

    // Mutators used by the constraint solver to apply Radius constraints
    // (the radius lives in the circle / arc struct, not in a point, so the
    // generic movePoint path can't update it).
    void setCircleRadius(int circleId, double r);
    void setArcRadius(int arcId, double r);

    // --- Post-draw size edits (Properties panel) ---------------------------
    // Resize a line to `newLength`, growing/shrinking symmetrically about its
    // midpoint along its current direction. Both endpoints move, so any element
    // sharing those points (chained lines, rectangle corners, arc ends) rides
    // along — that's the "anchored to the ends" behaviour. Arcs whose endpoint
    // moves are repaired to preserve their swept angle (see below).
    void setLineLength(int lineId, double newLength);

    // Resize an arc to `newRadius` keeping its centre fixed and its endpoints'
    // angles fixed (endpoints slide radially). The swept angle is preserved and
    // the endpoints stay exactly on the circle, so shared lines follow cleanly.
    void resizeArc(int arcId, double newRadius);

    // Set an arc's swept angle (radians, CCW from start) keeping centre, radius
    // and start point fixed; the end point moves to the new angle.
    void setArcSweep(int arcId, double sweepRad);

    // Set the straight-line distance between an arc's two endpoints (its chord)
    // keeping the SWEEP ANGLE fixed — i.e. the same arc shape, just scaled. Both
    // endpoints slide radially about the (fixed) centre; the radius adjusts to
    // hit the requested chord. (Equivalent to a Radius edit, parameterised by
    // endpoint distance instead of curvature.)
    void setArcChord(int arcId, double chordLen);

    // Axis-aligned rectangle described by four lines sharing four corner points.
    struct RectInfo {
        glm::vec2 center{0.0f};
        double width = 0.0;     // x-extent
        double height = 0.0;    // y-extent
        int cornerPts[4] = {-1, -1, -1, -1};
        int lineIds[4]   = {-1, -1, -1, -1};
    };
    // If `lineId` is one side of an axis-aligned rectangle (a closed 4-cycle of
    // axis-aligned lines over 4 shared points), fill `out` and return true.
    // Returns false for any non-rectangular / rotated / ambiguous arrangement,
    // so callers fall back to plain line editing.
    bool findAxisAlignedRect(int lineId, RectInfo& out) const;
    // Resize that rectangle to width × height, holding its centre fixed.
    void setRectangleSize(int lineId, double width, double height);

    // --- Constraints (entirely opt-in; only user-applied constraints land here) ---
    // The solver reads from and writes back to this vector. Constraints persist
    // with the sketch so multiple sketches don't share solver state and project
    // files can round-trip them.
    int addConstraint(const Constraint& c);
    void removeConstraint(int id);
    const std::vector<Constraint>& getConstraints() const { return m_constraints; }
    std::vector<Constraint>& getMutableConstraints() { return m_constraints; }
    // Serialization helper: append a constraint preserving its id (used by ProjectIO).
    void addRawConstraint(const Constraint& c) { m_constraints.push_back(c); }
    void setNextConstraintId(int n) { m_nextConstraintId = n; }
    int  getNextConstraintId() const { return m_nextConstraintId; }

private:
    // Move a point that may be an arc endpoint, then recompute each affected
    // arc's centre+radius so the arc keeps its original swept angle with the
    // (unmoved) opposite endpoint. Used by the size-edit mutators above so
    // stretching a line/rectangle that shares a point with an arc bends the arc
    // sensibly instead of leaving its endpoint off the circle.
    void moveEndpointPreservingArcs(int pointId, glm::vec2 newPos);

    int m_nextId = 1;
    std::string m_name = "Sketch";
    gp_Pln m_plane;
    int m_sourceBodyId = -1;
    bool m_detached = false;   // link to driven body deliberately broken
    TopoDS_Face m_sourceFace;

    std::vector<SketchPoint> m_points;
    std::vector<SketchLine> m_lines;
    std::vector<SketchCircle> m_circles;
    std::vector<SketchArc> m_arcs;
    std::vector<SketchSpline> m_splines;
    std::vector<SketchPolygon> m_polygons;
    std::vector<Constraint> m_constraints;
    int m_nextConstraintId = 1;
    FaceReference m_faceRefs;

    mutable bool m_centroidValid = false;
    mutable glm::vec2 m_centroid{0};

    // buildRegions cache (see the public declaration for rationale).
    mutable std::vector<Region> m_regionCache;
    mutable uint64_t m_regionHash = 0;
    mutable bool m_regionCacheValid = false;
    uint64_t geometryHash() const;
    std::vector<Region> buildRegionsUncached() const;

    int nextId() { return m_nextId++; }
    SketchPoint* findPoint(int id);
    gp_Pnt sketchToWorld(glm::vec2 pt2d) const;
};

} // namespace materializr
