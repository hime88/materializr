#include "SketchTool.h"
#include "SketchConstraints.h"
#include <algorithm>
#include <cmath>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

SketchTool::SketchTool() = default;

void SketchTool::setSketch(Sketch* sketch) {
    m_sketch = sketch;
}

void SketchTool::setSolver(SketchSolver* solver) {
    m_solver = solver;
}

void SketchTool::setMode(SketchToolMode mode) {
    // Cancel any in-progress operation when switching modes
    if (m_isPlacing) {
        onCancel();
    }
    m_mode = mode;
    m_clickCount = 0;
    m_isPlacing = false;
    m_lastPointId = -1;
    m_isDragging = false;
    m_dragPointId = -1;
    m_splinePoints.clear();
}

SketchToolMode SketchTool::getMode() const {
    return m_mode;
}

void SketchTool::onMouseDown(glm::vec2 pos, bool addToSel) {
    if (!m_sketch) return;
    m_lastDownAddedToSel = addToSel;

    // Trim picks by proximity to existing geometry; snapping the cursor first
    // would pull the click toward unrelated nearby points/edges and pick wrong.
    glm::vec2 snapped = (m_mode == SketchToolMode::Trim) ? pos : snap(pos);

    if (m_mode == SketchToolMode::None) return;

    switch (m_mode) {
        case SketchToolMode::Select:
            handleSelectTool(snapped);
            break;
        case SketchToolMode::Line:
            handleLineTool(snapped);
            break;
        case SketchToolMode::Circle:
            handleCircleTool(snapped);
            break;
        case SketchToolMode::Rectangle:
            handleRectangleTool(snapped);
            break;
        case SketchToolMode::Arc:
            handleArcTool(snapped);
            break;
        case SketchToolMode::Spline:
            handleSplineTool(snapped);
            break;
        case SketchToolMode::Polygon:
            handlePolygonTool(snapped);
            break;
        case SketchToolMode::Trim:
            handleTrimTool(snapped);
            break;
        default:
            break;
    }
}

void SketchTool::onMouseMove(glm::vec2 pos) {
    // Trim uses the raw cursor for picking; snapping would pull the click toward
    // unrelated nearby targets and pick the wrong element.
    glm::vec2 newPos = (m_mode == SketchToolMode::Trim) ? pos : snap(pos);
    glm::vec2 delta  = newPos - m_currentPos;
    m_currentPos = newPos;

    if (m_mode == SketchToolMode::Trim) {
        computeTrimHover(pos);
    } else if (!m_trimHoverPoints.empty()) {
        m_trimHoverPoints.clear();
    }

    // Drag the active selection (or the single dragged point if no broader
    // selection exists) by the cursor delta each frame.
    if (m_isDragging && m_sketch) {
        std::set<int> pts = m_selectedPoints;
        for (int lid : m_selectedLines) {
            for (const auto& l : m_sketch->getLines()) {
                if (l.id == lid) {
                    pts.insert(l.startPointId);
                    pts.insert(l.endPointId);
                    break;
                }
            }
        }
        if (pts.empty() && m_dragPointId >= 0) pts.insert(m_dragPointId);

        for (int pid : pts) {
            const SketchPoint* p = m_sketch->getPoint(pid);
            if (p) m_sketch->movePoint(pid, p->pos + delta);
        }
        if (m_solver) m_solver->solve(*m_sketch);
    }
}

void SketchTool::onMouseUp(glm::vec2 /*pos*/) {
    if (m_isDragging) {
        m_isDragging = false;
        m_dragPointId = -1;
    }
}

void SketchTool::onConfirm() {
    // Finalize spline if in spline mode with enough points
    if (m_mode == SketchToolMode::Spline && m_splinePoints.size() >= 2 && m_sketch) {
        m_sketch->addSpline(m_splinePoints);
        m_splinePoints.clear();
    }

    // Finish the current chain (e.g., stop line chaining)
    m_isPlacing = false;
    m_clickCount = 0;
    m_lastPointId = -1;
}

void SketchTool::onCancel() {
    m_isPlacing = false;
    m_clickCount = 0;
    m_lastPointId = -1;
    m_splinePoints.clear();
}

bool SketchTool::applyDimension(float value) {
    if (!m_sketch || !m_isPlacing || value <= 0.0f) return false;

    // Direction from anchor toward current cursor position. If the cursor is on top
    // of the anchor, default to +X so something happens.
    glm::vec2 delta = m_currentPos - m_firstClick;
    float curLen = glm::length(delta);
    glm::vec2 dir = (curLen > 1e-6f) ? (delta / curLen) : glm::vec2(1.0f, 0.0f);

    switch (m_mode) {
        case SketchToolMode::Line: {
            glm::vec2 endPos = m_firstClick + dir * value;
            handleLineTool(endPos);
            return true;
        }
        case SketchToolMode::Circle: {
            // value = radius. Pass any point at that radius to handleCircleTool.
            glm::vec2 perimeter = m_firstClick + dir * value;
            handleCircleTool(perimeter);
            return true;
        }
        case SketchToolMode::Polygon: {
            // For polygon the typed value is the SIDE COUNT (≥3), not the
            // radius. We DON'T commit here — just update m_polygonSides so
            // the live preview re-renders with the new count, and the user
            // can keep dragging to size + rotation. The actual placement
            // commits on the second click as for any other polygon.
            m_polygonSides = std::max(3, static_cast<int>(std::round(value)));
            return true;
        }
        case SketchToolMode::Rectangle: {
            // value = side length; use the cursor direction's signs to decide which quadrant
            float sx = (delta.x >= 0.0f) ? 1.0f : -1.0f;
            float sy = (delta.y >= 0.0f) ? 1.0f : -1.0f;
            glm::vec2 corner = m_firstClick + glm::vec2(value * sx, value * sy);
            handleRectangleTool(corner);
            return true;
        }
        case SketchToolMode::Arc: {
            // Arc needs three clicks; a single value can't fully specify it. No-op.
            return false;
        }
        default:
            return false;
    }
}

bool SketchTool::hasPreview() const {
    return m_isPlacing && m_mode != SketchToolMode::None;
}

glm::vec2 SketchTool::getPreviewStart() const {
    return m_firstClick;
}

glm::vec2 SketchTool::getPreviewEnd() const {
    return m_currentPos;
}

SketchToolMode SketchTool::getPreviewType() const {
    return m_mode;
}

bool SketchTool::isActive() const {
    return m_isPlacing;
}

glm::vec2 SketchTool::snap(glm::vec2 pos) const {
    // Point snap radius scales with grid step so it remains useful at 10 mm grids
    // and isn't overaggressive at 0.1 mm grids.
    float pointSnapThreshold = std::max(0.15f, m_gridStep * 0.4f);
    float curveSnapThreshold = pointSnapThreshold; // same band for circle/arc perimeters

    // First, try snapping to existing sketch points (always-on).
    if (m_sketch) {
        const auto& points = m_sketch->getPoints();
        for (const auto& pt : points) {
            if (glm::length(pos - pt.pos) < pointSnapThreshold) {
                return pt.pos;
            }
        }

        // Then, snap to the nearest point on any circle's perimeter (and arc perimeter).
        // This lets the line tool anchor on a curved edge so a sketch made off a circle
        // ends up topologically closed instead of leaving a sliver gap.
        const auto& circles = m_sketch->getCircles();
        for (const auto& c : circles) {
            const SketchPoint* center = m_sketch->getPoint(c.centerPointId);
            if (!center) continue;
            glm::vec2 v = pos - center->pos;
            float dist = glm::length(v);
            if (dist < 1e-6f) continue;
            float r = static_cast<float>(c.radius);
            float perimeterDist = std::abs(dist - r);
            if (perimeterDist < curveSnapThreshold) {
                return center->pos + (v / dist) * r;
            }
        }

        const auto& arcs = m_sketch->getArcs();
        for (const auto& a : arcs) {
            const SketchPoint* center = m_sketch->getPoint(a.centerPointId);
            if (!center) continue;
            glm::vec2 v = pos - center->pos;
            float dist = glm::length(v);
            if (dist < 1e-6f) continue;
            float r = static_cast<float>(a.radius);
            float perimeterDist = std::abs(dist - r);
            if (perimeterDist < curveSnapThreshold) {
                return center->pos + (v / dist) * r;
            }
        }

        // Snap to line midpoints (matches the green dots drawn by the renderer).
        const auto& lines = m_sketch->getLines();
        for (const auto& ln : lines) {
            const SketchPoint* p1 = m_sketch->getPoint(ln.startPointId);
            const SketchPoint* p2 = m_sketch->getPoint(ln.endPointId);
            if (!p1 || !p2) continue;
            glm::vec2 mid = 0.5f * (p1->pos + p2->pos);
            if (glm::length(pos - mid) < pointSnapThreshold) return mid;
        }
        // Snap to arc midpoints.
        for (const auto& a : arcs) {
            const SketchPoint* center = m_sketch->getPoint(a.centerPointId);
            const SketchPoint* s = m_sketch->getPoint(a.startPointId);
            const SketchPoint* e = m_sketch->getPoint(a.endPointId);
            if (!center || !s || !e) continue;
            float startA = std::atan2(s->pos.y - center->pos.y, s->pos.x - center->pos.x);
            float endA = std::atan2(e->pos.y - center->pos.y, e->pos.x - center->pos.x);
            const float TWO_PI = 2.0f * static_cast<float>(M_PI);
            float sweep = endA - startA;
            while (sweep < 0.0f) sweep += TWO_PI;
            while (sweep >= TWO_PI) sweep -= TWO_PI;
            float midA = startA + sweep * 0.5f;
            glm::vec2 mid = center->pos + glm::vec2(std::cos(midA), std::sin(midA))
                                            * static_cast<float>(a.radius);
            if (glm::length(pos - mid) < pointSnapThreshold) return mid;
        }
        // Snap to host face centroid (if sketch was started on a face).
        glm::vec2 faceCenter;
        if (m_sketch->getSourceFaceCentroid(faceCenter) &&
            glm::length(pos - faceCenter) < pointSnapThreshold) {
            return faceCenter;
        }
    }

    // Grid snap: only kicks in when within `gridSnapThreshold` of a grid line.
    // The threshold is a fraction of the grid step so 0.1 mm grids stay precise.
    if (m_gridStep <= 0.0f) return pos;
    const float gridSnapThreshold = m_gridStep * 0.25f;

    glm::vec2 result = pos;
    float nearestX = std::round(pos.x / m_gridStep) * m_gridStep;
    float nearestY = std::round(pos.y / m_gridStep) * m_gridStep;
    if (std::abs(pos.x - nearestX) < gridSnapThreshold) result.x = nearestX;
    if (std::abs(pos.y - nearestY) < gridSnapThreshold) result.y = nearestY;
    return result;
}

int SketchTool::findCoincidentPoint(glm::vec2 pos, int excludeId) const {
    if (!m_sketch) return -1;

    const float threshold = 0.3f; // same as point snap threshold
    const auto& points = m_sketch->getPoints();
    for (const auto& pt : points) {
        if (pt.id == excludeId) continue;
        if (glm::length(pos - pt.pos) < threshold) {
            return pt.id;
        }
    }
    return -1;
}

void SketchTool::autoConstrain(int lineId) {
    if (!m_sketch || !m_solver) return;

    // Find the line we just created
    const SketchLine* targetLine = nullptr;
    for (const auto& line : m_sketch->getLines()) {
        if (line.id == lineId) {
            targetLine = &line;
            break;
        }
    }
    if (!targetLine) return;

    const SketchPoint* p1 = m_sketch->getPoint(targetLine->startPointId);
    const SketchPoint* p2 = m_sketch->getPoint(targetLine->endPointId);
    if (!p1 || !p2) return;

    float dx = p2->pos.x - p1->pos.x;
    float dy = p2->pos.y - p1->pos.y;
    float angle = std::atan2(dy, dx);
    float absDeg = std::abs(angle * 180.0f / static_cast<float>(M_PI));

    // Check for near-horizontal (within 5 degrees)
    if (absDeg < 5.0f || absDeg > 175.0f) {
        Constraint c;
        c.id = 0; // solver will assign the real id
        c.type = ConstraintType::Horizontal;
        c.entityA = lineId;
        c.entityB = -1;
        c.value = 0.0;
        c.isSatisfied = false;
        m_solver->addConstraint(c);
    }
    // Check for near-vertical (within 5 degrees of 90)
    else if (std::abs(absDeg - 90.0f) < 5.0f) {
        Constraint c;
        c.id = 0;
        c.type = ConstraintType::Vertical;
        c.entityA = lineId;
        c.entityB = -1;
        c.value = 0.0;
        c.isSatisfied = false;
        m_solver->addConstraint(c);
    }

    // Check if start point is coincident with an existing point (other than itself)
    // Look through all points that are NOT part of this line
    const auto& allPoints = m_sketch->getPoints();
    for (const auto& pt : allPoints) {
        if (pt.id == p1->id || pt.id == p2->id) continue;

        if (glm::length(p1->pos - pt.pos) < 1e-4f) {
            Constraint c;
            c.id = 0;
            c.type = ConstraintType::Coincident;
            c.entityA = p1->id;
            c.entityB = pt.id;
            c.value = 0.0;
            c.isSatisfied = true; // they are already at the same position
            m_solver->addConstraint(c);
        }
        if (glm::length(p2->pos - pt.pos) < 1e-4f) {
            Constraint c;
            c.id = 0;
            c.type = ConstraintType::Coincident;
            c.entityA = p2->id;
            c.entityB = pt.id;
            c.value = 0.0;
            c.isSatisfied = true;
            m_solver->addConstraint(c);
        }
    }
}

void SketchTool::selectAll() {
    if (!m_sketch) return;
    m_selectedPoints.clear();
    m_selectedLines.clear();
    for (const auto& p : m_sketch->getPoints()) m_selectedPoints.insert(p.id);
    for (const auto& l : m_sketch->getLines())  m_selectedLines.insert(l.id);
}

void SketchTool::handleSelectTool(glm::vec2 pos) {
    if (!m_sketch) return;

    // Hit-test in order of priority: existing point first, then a nearby line.
    int nearPt = findCoincidentPoint(pos, -1);

    int nearLine = -1;
    if (nearPt < 0) {
        // Distance from `pos` to each line segment; pick the closest within tol.
        float bestD = 0.0f;
        const float tol = std::max(m_gridStep * 0.5f, 0.5f); // sketch units
        const auto& lines = m_sketch->getLines();
        for (const auto& l : lines) {
            const SketchPoint* a = m_sketch->getPoint(l.startPointId);
            const SketchPoint* b = m_sketch->getPoint(l.endPointId);
            if (!a || !b) continue;
            glm::vec2 ab = b->pos - a->pos;
            float len2 = glm::dot(ab, ab);
            if (len2 < 1e-12f) continue;
            float t = glm::clamp(glm::dot(pos - a->pos, ab) / len2, 0.0f, 1.0f);
            glm::vec2 proj = a->pos + ab * t;
            float d = glm::distance(proj, pos);
            if (d < tol && (nearLine < 0 || d < bestD)) {
                nearLine = l.id;
                bestD = d;
            }
        }
    }

    // If the user clicked on something that's ALREADY part of the selection,
    // start a drag of the whole selection in-place (don't replace the selection).
    bool reclickedSelected = (nearPt >= 0 && m_selectedPoints.count(nearPt)) ||
                             (nearLine >= 0 && m_selectedLines.count(nearLine));

    if (!m_lastDownAddedToSel && !reclickedSelected) {
        m_selectedPoints.clear();
        m_selectedLines.clear();
    }

    if (nearPt >= 0) {
        m_selectedPoints.insert(nearPt);
        m_dragPointId = nearPt;
        m_isDragging = true; // multi-point drag handled in onMouseMove
    } else if (nearLine >= 0) {
        if (m_lastDownAddedToSel) {
            // Ctrl+click on a line: toggle.
            if (m_selectedLines.count(nearLine)) m_selectedLines.erase(nearLine);
            else m_selectedLines.insert(nearLine);
        } else {
            m_selectedLines.insert(nearLine);
            m_isDragging = true; // dragging the line translates the whole selection
        }
    }
    // Empty space + no modifier: selection already cleared above.
}

void SketchTool::handleLineTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set start point
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;

        // Check if we're snapping to an existing point
        if (m_lastPointId == -1) {
            const auto& points = m_sketch->getPoints();
            for (const auto& pt : points) {
                if (glm::length(pos - pt.pos) < 1e-4f) {
                    m_lastPointId = pt.id;
                    break;
                }
            }
            if (m_lastPointId == -1) {
                m_lastPointId = m_sketch->addPoint(pos);
            }
        }
    } else {
        // Second click: create line and continue chain
        int endPointId = -1;

        // Check if snapping to existing point
        const auto& points = m_sketch->getPoints();
        for (const auto& pt : points) {
            if (pt.id != m_lastPointId && glm::length(pos - pt.pos) < 1e-4f) {
                endPointId = pt.id;
                break;
            }
        }

        if (endPointId == -1) {
            endPointId = m_sketch->addPoint(pos);
        }

        int newLineId = m_sketch->addLine(m_lastPointId, endPointId);
        autoConstrain(newLineId);

        // Continue chaining: the end becomes the new start
        m_lastPointId = endPointId;
        m_firstClick = pos;
        m_clickCount++;
    }
}

void SketchTool::handleCircleTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set center
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;
    } else {
        // Second click: set radius point, create circle
        float radius = glm::length(pos - m_firstClick);
        if (radius > 1e-6f) {
            // Reuse the existing point at this position if there is one
            // (this is how concentric circles share a center).
            int existing = findCoincidentPoint(m_firstClick, -1);
            int centerId = (existing >= 0) ? existing : m_sketch->addPoint(m_firstClick);
            m_sketch->addCircle(centerId, static_cast<double>(radius));
        }

        m_isPlacing = false;
        m_clickCount = 0;
    }
}

void SketchTool::handleRectangleTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set first corner
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;
    } else {
        // Second click: set opposite corner, create rectangle
        if (std::abs(pos.x - m_firstClick.x) > 1e-6f &&
            std::abs(pos.y - m_firstClick.y) > 1e-6f) {
            m_sketch->addRectangle(m_firstClick, pos);
        }

        m_isPlacing = false;
        m_clickCount = 0;
    }
}

void SketchTool::handleArcTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set start point
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;
    } else if (m_clickCount == 1) {
        // Second click: set end point
        m_secondClick = pos;
        m_clickCount = 2;
    } else if (m_clickCount == 2) {
        // Third click: set midpoint on arc, compute center and radius
        glm::vec2 start = m_firstClick;
        glm::vec2 end = m_secondClick;
        glm::vec2 mid = pos;

        // Compute the circumcenter of the three points (start, mid, end)
        // This gives us the arc center
        float ax = start.x, ay = start.y;
        float bx = mid.x, by = mid.y;
        float cx = end.x, cy = end.y;

        float D = 2.0f * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));

        if (std::abs(D) > 1e-10f) {
            float ux = ((ax * ax + ay * ay) * (by - cy) +
                        (bx * bx + by * by) * (cy - ay) +
                        (cx * cx + cy * cy) * (ay - by)) / D;
            float uy = ((ax * ax + ay * ay) * (cx - bx) +
                        (bx * bx + by * by) * (ax - cx) +
                        (cx * cx + cy * cy) * (bx - ax)) / D;

            glm::vec2 center(ux, uy);
            float radius = glm::length(start - center);

            auto reuseOrAdd = [&](glm::vec2 p) {
                int existing = findCoincidentPoint(p, -1);
                return (existing >= 0) ? existing : m_sketch->addPoint(p);
            };
            int centerId = reuseOrAdd(center);
            int startId  = reuseOrAdd(start);
            int endId    = reuseOrAdd(end);

            m_sketch->addArc(centerId, startId, endId, static_cast<double>(radius));
        }

        m_isPlacing = false;
        m_clickCount = 0;
    }
}

void SketchTool::handleSplineTool(glm::vec2 pos) {
    if (!m_sketch) return;

    // Each click adds a control point
    int ptId = -1;

    // Check if snapping to an existing point
    const auto& points = m_sketch->getPoints();
    for (const auto& pt : points) {
        if (glm::length(pos - pt.pos) < 1e-4f) {
            ptId = pt.id;
            break;
        }
    }

    if (ptId == -1) {
        ptId = m_sketch->addPoint(pos);
    }

    m_splinePoints.push_back(ptId);

    if (!m_isPlacing) {
        m_firstClick = pos;
        m_isPlacing = true;
    }

    m_clickCount = static_cast<int>(m_splinePoints.size());
    // The spline is finalized via onConfirm() (Enter key)
}

// ─── Trim tool ──────────────────────────────────────────────────────────────
//
// The trim tool removes the segment of a sketch element between its two nearest
// intersections with any other element. If the element has no intersections,
// the whole element is removed. Splines and polygons are not split — clicking
// one removes it entirely.

namespace {

// Wrap angle into [0, 2π).
float wrap2Pi(float a) {
    const float TWO_PI = 2.0f * static_cast<float>(M_PI);
    while (a < 0.0f) a += TWO_PI;
    while (a >= TWO_PI) a -= TWO_PI;
    return a;
}

// Closest point on segment p1→p2 to q, returns squared distance.
float distSqPointSegment(glm::vec2 q, glm::vec2 p1, glm::vec2 p2, float* outT = nullptr) {
    glm::vec2 d = p2 - p1;
    float len2 = glm::dot(d, d);
    if (len2 < 1e-12f) {
        if (outT) *outT = 0.0f;
        glm::vec2 v = q - p1;
        return glm::dot(v, v);
    }
    float t = glm::dot(q - p1, d) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    if (outT) *outT = t;
    glm::vec2 closest = p1 + t * d;
    glm::vec2 v = q - closest;
    return glm::dot(v, v);
}

// True if angle θ falls within arc going CCW from startAngle to endAngle.
bool angleInArc(float theta, float startAngle, float endAngle) {
    float s = wrap2Pi(startAngle);
    float e = wrap2Pi(endAngle);
    float t = wrap2Pi(theta);
    if (s <= e) return t >= s - 1e-5f && t <= e + 1e-5f;
    // arc crosses 0
    return t >= s - 1e-5f || t <= e + 1e-5f;
}

struct Hit { glm::vec2 pos; float param; };

// Intersect line p1-p2 with line p3-p4. param is t along p1→p2.
void intersectLineLine(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4,
                       std::vector<Hit>& out) {
    glm::vec2 d1 = p2 - p1;
    glm::vec2 d2 = p4 - p3;
    float denom = d1.x * d2.y - d1.y * d2.x;
    if (std::abs(denom) < 1e-10f) return; // parallel
    float t = ((p3.x - p1.x) * d2.y - (p3.y - p1.y) * d2.x) / denom;
    float u = ((p3.x - p1.x) * d1.y - (p3.y - p1.y) * d1.x) / denom;
    const float eps = 1e-4f;
    if (t < -eps || t > 1.0f + eps) return;
    if (u < -eps || u > 1.0f + eps) return;
    out.push_back({p1 + t * d1, t});
}

// Intersect line p1-p2 with circle (center, radius). param = t along the line.
void intersectLineCircle(glm::vec2 p1, glm::vec2 p2, glm::vec2 center, float radius,
                         std::vector<Hit>& out) {
    glm::vec2 d = p2 - p1;
    glm::vec2 f = p1 - center;
    float a = glm::dot(d, d);
    float b = 2.0f * glm::dot(f, d);
    float c = glm::dot(f, f) - radius * radius;
    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return;
    disc = std::sqrt(disc);
    const float eps = 1e-4f;
    for (float t : { (-b - disc) / (2 * a), (-b + disc) / (2 * a) }) {
        if (t < -eps || t > 1.0f + eps) continue;
        out.push_back({p1 + t * d, t});
    }
}

// Intersect line with arc (filter line-circle by arc angle range).
void intersectLineArc(glm::vec2 p1, glm::vec2 p2, glm::vec2 center, float radius,
                      float startAngle, float endAngle, std::vector<Hit>& out) {
    std::vector<Hit> tmp;
    intersectLineCircle(p1, p2, center, radius, tmp);
    for (const auto& h : tmp) {
        glm::vec2 r = h.pos - center;
        float theta = std::atan2(r.y, r.x);
        if (angleInArc(theta, startAngle, endAngle)) out.push_back(h);
    }
}

// Intersect circle (c1,r1) with circle (c2,r2). Param = angle on circle 1.
void intersectCircleCircle(glm::vec2 c1, float r1, glm::vec2 c2, float r2,
                           std::vector<Hit>& out) {
    glm::vec2 d = c2 - c1;
    float dist = glm::length(d);
    if (dist < 1e-10f) return; // concentric
    if (dist > r1 + r2 + 1e-5f) return;
    if (dist < std::abs(r1 - r2) - 1e-5f) return;
    float a = (r1 * r1 - r2 * r2 + dist * dist) / (2.0f * dist);
    float h2 = r1 * r1 - a * a;
    float h = (h2 > 0.0f) ? std::sqrt(h2) : 0.0f;
    glm::vec2 mid = c1 + d * (a / dist);
    glm::vec2 perp(-d.y / dist, d.x / dist);
    for (int sign : { 1, -1 }) {
        glm::vec2 p = mid + perp * (h * static_cast<float>(sign));
        float theta = std::atan2(p.y - c1.y, p.x - c1.x);
        out.push_back({p, theta});
        if (h < 1e-6f) break; // tangent
    }
}

} // anonymous

// Returns elementId of the picked element, or -1 if none within threshold.
// Sets pickType to one of "line","circle","arc","spline","polygon".
static int pickSketchElement(const Sketch& sketch, glm::vec2 pos, float threshold,
                             std::string& pickType) {
    float bestDistSq = threshold * threshold;
    int bestId = -1;
    pickType.clear();

    // Lines
    for (const auto& ln : sketch.getLines()) {
        const SketchPoint* a = sketch.getPoint(ln.startPointId);
        const SketchPoint* b = sketch.getPoint(ln.endPointId);
        if (!a || !b) continue;
        float dsq = distSqPointSegment(pos, a->pos, b->pos);
        if (dsq < bestDistSq) { bestDistSq = dsq; bestId = ln.id; pickType = "line"; }
    }
    // Circles
    for (const auto& ci : sketch.getCircles()) {
        const SketchPoint* c = sketch.getPoint(ci.centerPointId);
        if (!c) continue;
        float d = glm::length(pos - c->pos);
        float dsq = (d - static_cast<float>(ci.radius)) * (d - static_cast<float>(ci.radius));
        if (dsq < bestDistSq) { bestDistSq = dsq; bestId = ci.id; pickType = "circle"; }
    }
    // Arcs
    for (const auto& ar : sketch.getArcs()) {
        const SketchPoint* c = sketch.getPoint(ar.centerPointId);
        const SketchPoint* s = sketch.getPoint(ar.startPointId);
        const SketchPoint* e = sketch.getPoint(ar.endPointId);
        if (!c || !s || !e) continue;
        glm::vec2 r = pos - c->pos;
        float distC = glm::length(r);
        float perimeterDist = std::abs(distC - static_cast<float>(ar.radius));
        float theta = std::atan2(r.y, r.x);
        float startA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float endA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        if (angleInArc(theta, startA, endA)) {
            float dsq = perimeterDist * perimeterDist;
            if (dsq < bestDistSq) { bestDistSq = dsq; bestId = ar.id; pickType = "arc"; }
        }
    }
    // Splines / polygons: bounding-box hit only (no precise picking) — fall back to
    // simple proximity to control points or vertices.
    for (const auto& sp : sketch.getSplines()) {
        for (int pid : sp.controlPointIds) {
            const SketchPoint* p = sketch.getPoint(pid);
            if (!p) continue;
            glm::vec2 v = pos - p->pos;
            float dsq = glm::dot(v, v);
            if (dsq < bestDistSq) { bestDistSq = dsq; bestId = sp.id; pickType = "spline"; }
        }
    }
    for (const auto& po : sketch.getPolygons()) {
        for (int pid : po.vertexPointIds) {
            const SketchPoint* p = sketch.getPoint(pid);
            if (!p) continue;
            glm::vec2 v = pos - p->pos;
            float dsq = glm::dot(v, v);
            if (dsq < bestDistSq) { bestDistSq = dsq; bestId = po.id; pickType = "polygon"; }
        }
    }
    return bestId;
}

// Collect intersection points + parameters along a given line element.
static void collectLineIntersections(const Sketch& sketch, const SketchLine& target,
                                     std::vector<Hit>& out) {
    const SketchPoint* a = sketch.getPoint(target.startPointId);
    const SketchPoint* b = sketch.getPoint(target.endPointId);
    if (!a || !b) return;
    glm::vec2 p1 = a->pos, p2 = b->pos;

    for (const auto& ln : sketch.getLines()) {
        if (ln.id == target.id) continue;
        const SketchPoint* c = sketch.getPoint(ln.startPointId);
        const SketchPoint* d = sketch.getPoint(ln.endPointId);
        if (!c || !d) continue;
        intersectLineLine(p1, p2, c->pos, d->pos, out);
    }
    for (const auto& ci : sketch.getCircles()) {
        const SketchPoint* c = sketch.getPoint(ci.centerPointId);
        if (!c) continue;
        intersectLineCircle(p1, p2, c->pos, static_cast<float>(ci.radius), out);
    }
    for (const auto& ar : sketch.getArcs()) {
        const SketchPoint* c = sketch.getPoint(ar.centerPointId);
        const SketchPoint* s = sketch.getPoint(ar.startPointId);
        const SketchPoint* e = sketch.getPoint(ar.endPointId);
        if (!c || !s || !e) continue;
        float startA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float endA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        intersectLineArc(p1, p2, c->pos, static_cast<float>(ar.radius), startA, endA, out);
    }
}

// Collect intersection angles around a circle element (parameters are angles in [0,2π)).
static void collectCircleIntersections(const Sketch& sketch, glm::vec2 center, float radius,
                                       int skipId, std::vector<Hit>& out) {
    for (const auto& ln : sketch.getLines()) {
        if (ln.id == skipId) continue;
        const SketchPoint* a = sketch.getPoint(ln.startPointId);
        const SketchPoint* b = sketch.getPoint(ln.endPointId);
        if (!a || !b) continue;
        std::vector<Hit> tmp;
        intersectLineCircle(a->pos, b->pos, center, radius, tmp);
        for (auto& h : tmp) {
            float theta = std::atan2(h.pos.y - center.y, h.pos.x - center.x);
            out.push_back({h.pos, wrap2Pi(theta)});
        }
    }
    for (const auto& ci : sketch.getCircles()) {
        if (ci.id == skipId) continue;
        const SketchPoint* c = sketch.getPoint(ci.centerPointId);
        if (!c) continue;
        std::vector<Hit> tmp;
        intersectCircleCircle(center, radius, c->pos, static_cast<float>(ci.radius), tmp);
        for (auto& h : tmp) out.push_back({h.pos, wrap2Pi(h.param)});
    }
    for (const auto& ar : sketch.getArcs()) {
        if (ar.id == skipId) continue;
        const SketchPoint* c = sketch.getPoint(ar.centerPointId);
        const SketchPoint* s = sketch.getPoint(ar.startPointId);
        const SketchPoint* e = sketch.getPoint(ar.endPointId);
        if (!c || !s || !e) continue;
        std::vector<Hit> tmp;
        intersectCircleCircle(center, radius, c->pos, static_cast<float>(ar.radius), tmp);
        float startA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float endA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        for (auto& h : tmp) {
            glm::vec2 r = h.pos - c->pos;
            float thetaOnOther = std::atan2(r.y, r.x);
            if (!angleInArc(thetaOnOther, startA, endA)) continue;
            float theta = std::atan2(h.pos.y - center.y, h.pos.x - center.x);
            out.push_back({h.pos, wrap2Pi(theta)});
        }
    }
}

namespace {

// Plan for a single trim click: what to delete, plus the data needed to
// regenerate the surviving sub-segments. Computed by planTrim() without any
// mutation so that both the hover preview and the actual click use the same
// geometric decision.
struct TrimAction {
    enum class Kind { None, FullDelete, Line, Circle, Arc };

    Kind kind = Kind::None;
    int targetId = -1;

    // Line
    glm::vec2 lineP1{0}, lineP2{0};
    int lineKillIndex = -1;
    std::vector<float> lineBounds;

    // Circle (whole circle being cut into arcs)
    glm::vec2 circCenter{0};
    float circRadius = 0.0f;
    int circCenterPointId = -1;
    int circKillIndex = -1;
    std::vector<float> circAngles; // CCW-sorted intersection angles in [0, 2π)

    // Arc (existing arc being cut into sub-arcs)
    glm::vec2 arcCenter{0};
    float arcRadius = 0.0f;
    float arcStartA = 0.0f;
    int arcCenterPointId = -1;
    int arcKillIndex = -1;
    std::vector<float> arcBounds; // CCW distances from arcStartA: [0, d1, ..., totalArc]

    bool valid() const { return kind != Kind::None; }
};

TrimAction planTrim(const Sketch& sketch, glm::vec2 pos, float threshold) {
    TrimAction action;
    std::string pickType;
    int id = pickSketchElement(sketch, pos, threshold, pickType);
    if (id < 0) return action;
    action.targetId = id;

    if (pickType == "spline" || pickType == "polygon") {
        action.kind = TrimAction::Kind::FullDelete;
        return action;
    }

    if (pickType == "line") {
        const SketchLine* line = nullptr;
        for (const auto& l : sketch.getLines()) if (l.id == id) { line = &l; break; }
        if (!line) return action;
        const SketchPoint* a = sketch.getPoint(line->startPointId);
        const SketchPoint* b = sketch.getPoint(line->endPointId);
        if (!a || !b) return action;
        action.lineP1 = a->pos;
        action.lineP2 = b->pos;

        std::vector<Hit> hits;
        collectLineIntersections(sketch, *line, hits);
        if (hits.empty()) { action.kind = TrimAction::Kind::FullDelete; return action; }

        std::sort(hits.begin(), hits.end(),
                  [](const Hit& x, const Hit& y) { return x.param < y.param; });
        std::vector<Hit> uniq;
        for (const auto& h : hits) {
            if (uniq.empty() || std::abs(h.param - uniq.back().param) > 1e-3f) uniq.push_back(h);
        }

        std::vector<float> bounds;
        bounds.push_back(0.0f);
        for (const auto& h : uniq) {
            if (h.param > 1e-3f && h.param < 1.0f - 1e-3f) bounds.push_back(h.param);
        }
        bounds.push_back(1.0f);

        float tClick;
        distSqPointSegment(pos, action.lineP1, action.lineP2, &tClick);

        int killIndex = -1;
        for (int i = 0; i + 1 < static_cast<int>(bounds.size()); ++i) {
            if (tClick >= bounds[i] - 1e-3f && tClick <= bounds[i + 1] + 1e-3f) {
                killIndex = i; break;
            }
        }
        if (killIndex < 0) { action.kind = TrimAction::Kind::FullDelete; return action; }

        action.kind = TrimAction::Kind::Line;
        action.lineBounds = std::move(bounds);
        action.lineKillIndex = killIndex;
        return action;
    }

    if (pickType == "circle") {
        const SketchCircle* circ = nullptr;
        for (const auto& c : sketch.getCircles()) if (c.id == id) { circ = &c; break; }
        if (!circ) return action;
        const SketchPoint* cp = sketch.getPoint(circ->centerPointId);
        if (!cp) return action;
        action.circCenter = cp->pos;
        action.circRadius = static_cast<float>(circ->radius);
        action.circCenterPointId = circ->centerPointId;

        std::vector<Hit> hits;
        collectCircleIntersections(sketch, action.circCenter, action.circRadius, id, hits);
        if (hits.empty()) { action.kind = TrimAction::Kind::FullDelete; return action; }

        std::sort(hits.begin(), hits.end(),
                  [](const Hit& x, const Hit& y) { return x.param < y.param; });
        std::vector<float> angles;
        for (const auto& h : hits) {
            if (angles.empty() || std::abs(h.param - angles.back()) > 1e-3f) angles.push_back(h.param);
        }

        glm::vec2 r = pos - action.circCenter;
        float thetaClick = wrap2Pi(std::atan2(r.y, r.x));
        int n = static_cast<int>(angles.size());
        int killIndex = -1;
        for (int i = 0; i < n; ++i) {
            float a = angles[i];
            float b = angles[(i + 1) % n];
            if (angleInArc(thetaClick, a, b)) { killIndex = i; break; }
        }
        if (killIndex < 0) { action.kind = TrimAction::Kind::FullDelete; return action; }

        action.kind = TrimAction::Kind::Circle;
        action.circAngles = std::move(angles);
        action.circKillIndex = killIndex;
        return action;
    }

    if (pickType == "arc") {
        const SketchArc* arc = nullptr;
        for (const auto& a : sketch.getArcs()) if (a.id == id) { arc = &a; break; }
        if (!arc) return action;
        const SketchPoint* cp = sketch.getPoint(arc->centerPointId);
        const SketchPoint* sp = sketch.getPoint(arc->startPointId);
        const SketchPoint* ep = sketch.getPoint(arc->endPointId);
        if (!cp || !sp || !ep) return action;
        action.arcCenter = cp->pos;
        action.arcRadius = static_cast<float>(arc->radius);
        action.arcStartA = std::atan2(sp->pos.y - cp->pos.y, sp->pos.x - cp->pos.x);
        action.arcCenterPointId = arc->centerPointId;
        float endA = std::atan2(ep->pos.y - cp->pos.y, ep->pos.x - cp->pos.x);
        float totalCCW = wrap2Pi(endA - action.arcStartA);

        std::vector<Hit> hits;
        collectCircleIntersections(sketch, action.arcCenter, action.arcRadius, id, hits);
        std::vector<float> inRangeD; // CCW distances of in-range intersections
        for (const auto& h : hits) {
            float d = wrap2Pi(h.param - action.arcStartA);
            if (d > 1e-3f && d < totalCCW - 1e-3f) inRangeD.push_back(d);
        }
        if (inRangeD.empty()) { action.kind = TrimAction::Kind::FullDelete; return action; }

        std::sort(inRangeD.begin(), inRangeD.end());
        std::vector<float> bounds;
        bounds.push_back(0.0f);
        for (float d : inRangeD) {
            if (bounds.empty() || std::abs(d - bounds.back()) > 1e-3f) bounds.push_back(d);
        }
        bounds.push_back(totalCCW);

        glm::vec2 r = pos - action.arcCenter;
        float dClick = wrap2Pi(std::atan2(r.y, r.x) - action.arcStartA);
        int killIndex = -1;
        for (int i = 0; i + 1 < static_cast<int>(bounds.size()); ++i) {
            if (dClick >= bounds[i] - 1e-3f && dClick <= bounds[i + 1] + 1e-3f) {
                killIndex = i; break;
            }
        }
        if (killIndex < 0) { action.kind = TrimAction::Kind::FullDelete; return action; }

        action.kind = TrimAction::Kind::Arc;
        action.arcBounds = std::move(bounds);
        action.arcKillIndex = killIndex;
        return action;
    }

    return action;
}

// Densify the segment that would be removed into 2D sketch-space points.
// For FullDelete the entire element is densified (so the renderer can highlight it).
void densifyTrimPreview(const Sketch& sketch, const TrimAction& a, std::vector<glm::vec2>& out) {
    out.clear();
    if (a.kind == TrimAction::Kind::FullDelete) {
        for (const auto& l : sketch.getLines()) if (l.id == a.targetId) {
            const SketchPoint* p1 = sketch.getPoint(l.startPointId);
            const SketchPoint* p2 = sketch.getPoint(l.endPointId);
            if (p1 && p2) { out.push_back(p1->pos); out.push_back(p2->pos); }
            return;
        }
        for (const auto& c : sketch.getCircles()) if (c.id == a.targetId) {
            const SketchPoint* cp = sketch.getPoint(c.centerPointId);
            if (!cp) return;
            float r = static_cast<float>(c.radius);
            const int samples = 64;
            for (int i = 0; i <= samples; ++i) {
                float t = 2.0f * static_cast<float>(M_PI) * i / samples;
                out.push_back(cp->pos + glm::vec2(std::cos(t), std::sin(t)) * r);
            }
            return;
        }
        for (const auto& ar : sketch.getArcs()) if (ar.id == a.targetId) {
            const SketchPoint* cp = sketch.getPoint(ar.centerPointId);
            const SketchPoint* sp = sketch.getPoint(ar.startPointId);
            const SketchPoint* ep = sketch.getPoint(ar.endPointId);
            if (!cp || !sp || !ep) return;
            float r = static_cast<float>(ar.radius);
            float startA = std::atan2(sp->pos.y - cp->pos.y, sp->pos.x - cp->pos.x);
            float endA = std::atan2(ep->pos.y - cp->pos.y, ep->pos.x - cp->pos.x);
            float total = wrap2Pi(endA - startA);
            const int samples = 32;
            for (int i = 0; i <= samples; ++i) {
                float t = total * i / samples;
                float th = startA + t;
                out.push_back(cp->pos + glm::vec2(std::cos(th), std::sin(th)) * r);
            }
            return;
        }
        for (const auto& spl : sketch.getSplines()) if (spl.id == a.targetId) {
            for (int pid : spl.controlPointIds) {
                const SketchPoint* p = sketch.getPoint(pid);
                if (p) out.push_back(p->pos);
            }
            return;
        }
        for (const auto& po : sketch.getPolygons()) if (po.id == a.targetId) {
            for (int pid : po.vertexPointIds) {
                const SketchPoint* p = sketch.getPoint(pid);
                if (p) out.push_back(p->pos);
            }
            if (!out.empty()) out.push_back(out.front());
            return;
        }
        return;
    }
    switch (a.kind) {
        case TrimAction::Kind::None: return;
        case TrimAction::Kind::FullDelete: return; // handled above
        case TrimAction::Kind::Line: {
            float t0 = a.lineBounds[a.lineKillIndex];
            float t1 = a.lineBounds[a.lineKillIndex + 1];
            out.push_back(a.lineP1 + t0 * (a.lineP2 - a.lineP1));
            out.push_back(a.lineP1 + t1 * (a.lineP2 - a.lineP1));
            return;
        }
        case TrimAction::Kind::Circle: {
            int n = static_cast<int>(a.circAngles.size());
            float a0 = a.circAngles[a.circKillIndex];
            float a1 = a.circAngles[(a.circKillIndex + 1) % n];
            float sweep = wrap2Pi(a1 - a0);
            const int samples = 32;
            for (int i = 0; i <= samples; ++i) {
                float t = static_cast<float>(i) / samples;
                float th = a0 + sweep * t;
                out.push_back(a.circCenter +
                              glm::vec2(std::cos(th), std::sin(th)) * a.circRadius);
            }
            return;
        }
        case TrimAction::Kind::Arc: {
            float d0 = a.arcBounds[a.arcKillIndex];
            float d1 = a.arcBounds[a.arcKillIndex + 1];
            const int samples = 32;
            for (int i = 0; i <= samples; ++i) {
                float t = static_cast<float>(i) / samples;
                float th = a.arcStartA + d0 + (d1 - d0) * t;
                out.push_back(a.arcCenter +
                              glm::vec2(std::cos(th), std::sin(th)) * a.arcRadius);
            }
            return;
        }
    }
}

// Reuse an existing sketch point if one already sits within `tol` of `pos`,
// otherwise add a new one. Critical for trim: new sub-segments must share their
// endpoint IDs with the adjacent geometry (the other element that defined the
// intersection point), otherwise buildWires() can't stitch them into a closed
// loop and buildRegions() never sees a manifold region for push/pull.
int findOrAddPoint(Sketch& sketch, glm::vec2 pos, float tol = 1e-2f) {
    for (const auto& p : sketch.getPoints()) {
        if (glm::length(pos - p.pos) < tol) return p.id;
    }
    return sketch.addPoint(pos);
}

void applyTrim(Sketch& sketch, const TrimAction& a) {
    if (a.kind == TrimAction::Kind::None) return;

    if (a.kind == TrimAction::Kind::FullDelete) {
        sketch.removeElement(a.targetId);
        return;
    }

    if (a.kind == TrimAction::Kind::Line) {
        sketch.removeElement(a.targetId);
        for (int i = 0; i + 1 < static_cast<int>(a.lineBounds.size()); ++i) {
            if (i == a.lineKillIndex) continue;
            glm::vec2 sp = a.lineP1 + a.lineBounds[i] * (a.lineP2 - a.lineP1);
            glm::vec2 ep = a.lineP1 + a.lineBounds[i + 1] * (a.lineP2 - a.lineP1);
            int sid = findOrAddPoint(sketch, sp);
            int eid = findOrAddPoint(sketch, ep);
            sketch.addLine(sid, eid);
        }
        return;
    }

    if (a.kind == TrimAction::Kind::Circle) {
        sketch.removeElement(a.targetId);
        int centerId = sketch.getPoint(a.circCenterPointId) ? a.circCenterPointId
                                                            : findOrAddPoint(sketch, a.circCenter);
        int n = static_cast<int>(a.circAngles.size());
        for (int i = 0; i < n; ++i) {
            if (i == a.circKillIndex) continue;
            float a0 = a.circAngles[i];
            float a1 = a.circAngles[(i + 1) % n];
            glm::vec2 sp = a.circCenter + glm::vec2(std::cos(a0), std::sin(a0)) * a.circRadius;
            glm::vec2 ep = a.circCenter + glm::vec2(std::cos(a1), std::sin(a1)) * a.circRadius;
            int sid = findOrAddPoint(sketch, sp);
            int eid = findOrAddPoint(sketch, ep);
            sketch.addArc(centerId, sid, eid, static_cast<double>(a.circRadius));
        }
        return;
    }

    if (a.kind == TrimAction::Kind::Arc) {
        sketch.removeElement(a.targetId);
        int centerId = sketch.getPoint(a.arcCenterPointId) ? a.arcCenterPointId
                                                           : findOrAddPoint(sketch, a.arcCenter);
        for (int i = 0; i + 1 < static_cast<int>(a.arcBounds.size()); ++i) {
            if (i == a.arcKillIndex) continue;
            float angA = a.arcStartA + a.arcBounds[i];
            float angB = a.arcStartA + a.arcBounds[i + 1];
            glm::vec2 sp = a.arcCenter + glm::vec2(std::cos(angA), std::sin(angA)) * a.arcRadius;
            glm::vec2 ep = a.arcCenter + glm::vec2(std::cos(angB), std::sin(angB)) * a.arcRadius;
            int sid = findOrAddPoint(sketch, sp);
            int eid = findOrAddPoint(sketch, ep);
            sketch.addArc(centerId, sid, eid, static_cast<double>(a.arcRadius));
        }
        return;
    }
}

} // anonymous

void SketchTool::handleTrimTool(glm::vec2 pos) {
    if (!m_sketch) return;
    float threshold = std::max(0.3f, m_gridStep * 0.5f);
    TrimAction a = planTrim(*m_sketch, pos, threshold);
    if (!a.valid()) return;
    applyTrim(*m_sketch, a);
    m_trimHoverPoints.clear(); // stale after mutation; recomputed on next move
}

void SketchTool::computeTrimHover(glm::vec2 pos) {
    m_trimHoverPoints.clear();
    if (!m_sketch) return;
    float threshold = std::max(0.3f, m_gridStep * 0.5f);
    TrimAction a = planTrim(*m_sketch, pos, threshold);
    if (a.valid()) densifyTrimPreview(*m_sketch, a, m_trimHoverPoints);
}

void SketchTool::handlePolygonTool(glm::vec2 pos) {
    if (!m_isPlacing) {
        // First click: set center
        m_firstClick = pos;
        m_isPlacing = true;
        m_clickCount = 1;
    } else {
        // Second click: set radius + rotation and create polygon. Cursor
        // direction from center defines vertex 0's angle, so the first
        // vertex lands exactly under the (grid-snapped) cursor — same
        // "corner snaps to grid" behaviour the user gets with circles.
        glm::vec2 delta = pos - m_firstClick;
        float radius = glm::length(delta);
        if (radius > 1e-6f) {
            int existing = findCoincidentPoint(m_firstClick, -1);
            int centerId = (existing >= 0) ? existing : m_sketch->addPoint(m_firstClick);
            double rotation = std::atan2(delta.y, delta.x);
            m_sketch->addPolygon(centerId, static_cast<double>(radius),
                                 m_polygonSides, rotation);
        }

        m_isPlacing = false;
        m_clickCount = 0;
    }
}

} // namespace materializr
