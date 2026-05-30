#pragma once
#include <glm/glm.hpp>
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
};

struct SketchLine {
    int id;
    int startPointId;
    int endPointId;
    bool isConstruction = false;
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
    int addPoint(glm::vec2 pos);
    void movePoint(int id, glm::vec2 pos);
    const SketchPoint* getPoint(int id) const;
    const std::vector<SketchPoint>& getPoints() const;

    // Element creation
    int addLine(int startPtId, int endPtId);
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

    // Element removal
    void removeElement(int id);
    void clear();

    // Convert closed profiles to OCCT wires for extrusion
    std::vector<TopoDS_Wire> buildWires() const;

    // A planar region of the sketch (one outer boundary + zero or more holes).
    // Built from the closed wires via 2D containment analysis.
    struct Region {
        TopoDS_Wire outerWire;
        std::vector<TopoDS_Wire> holeWires; // already reversed for Add()
        TopoDS_Face face;                   // planar face on the sketch plane
        glm::vec2 representativePoint;      // a point guaranteed inside the region (2D sketch space)
    };
    std::vector<Region> buildRegions() const;

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
    void setSourceFace(const TopoDS_Face& face) { m_sourceFace = face; m_centroidValid = false; }
    const TopoDS_Face& getSourceFace() const { return m_sourceFace; }

    // Area centroid of the host face's outer wire, projected into sketch-plane
    // 2D. Returns false if there is no source face. Computed once and cached.
    bool getSourceFaceCentroid(glm::vec2& out) const;

    // Sketch state
    std::string getName() const { return m_name; }
    void setName(const std::string& name) { m_name = name; }

    int elementCount() const;
    int pointCount() const;

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

private:
    int m_nextId = 1;
    std::string m_name = "Sketch";
    gp_Pln m_plane;
    int m_sourceBodyId = -1;
    TopoDS_Face m_sourceFace;

    std::vector<SketchPoint> m_points;
    std::vector<SketchLine> m_lines;
    std::vector<SketchCircle> m_circles;
    std::vector<SketchArc> m_arcs;
    std::vector<SketchSpline> m_splines;
    std::vector<SketchPolygon> m_polygons;

    mutable bool m_centroidValid = false;
    mutable glm::vec2 m_centroid{0};

    int nextId() { return m_nextId++; }
    SketchPoint* findPoint(int id);
    gp_Pnt sketchToWorld(glm::vec2 pt2d) const;
};

} // namespace materializr
