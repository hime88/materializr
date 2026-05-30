#include "gl_common.h"
#include "SketchRenderer.h"
#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchConstraints.h"

#include <glm/gtc/type_ptr.hpp>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Geom_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <cstdio>
#include <cmath>

namespace materializr {

static const char* s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_fragSource = R"(
#version 330 core
uniform vec3 u_color;
out vec4 fragColor;
void main() {
    fragColor = vec4(u_color, 1.0);
}
)";

SketchRenderer::SketchRenderer() {}

SketchRenderer::~SketchRenderer() {
    if (m_program) glDeleteProgram(m_program);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

bool SketchRenderer::initialize() {
    unsigned int vert = 0, frag = 0;
    if (!compileShader(vert, GL_VERTEX_SHADER, s_vertSource)) return false;
    if (!compileShader(frag, GL_FRAGMENT_SHADER, s_fragSource)) {
        glDeleteShader(vert);
        return false;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vert);
    glAttachShader(m_program, frag);
    glLinkProgram(m_program);
    glDeleteShader(vert);
    glDeleteShader(frag);

    int success = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(m_program, 512, nullptr, log);
        std::fprintf(stderr, "SketchRenderer link error: %s\n", log);
        return false;
    }

    m_locMVP = glGetUniformLocation(m_program, "u_mvp");
    m_locColor = glGetUniformLocation(m_program, "u_color");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    return true;
}

void SketchRenderer::uploadAndDraw(const std::vector<float>& verts, GLenum mode,
                                    const glm::vec3& color, const glm::mat4& vp,
                                    float lineWidth) {
    if (verts.empty()) return;

    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform3fv(m_locColor, 1, glm::value_ptr(color));

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);

    glLineWidth(lineWidth);
    if (mode == GL_POINTS) glPointSize(6.0f);

    glDrawArrays(mode, 0, static_cast<int>(verts.size() / 3));

    glBindVertexArray(0);
    glUseProgram(0);
}

void SketchRenderer::render(const Sketch* sketch, const SketchTool* tool,
                             const glm::mat4& view, const glm::mat4& projection,
                             const SketchSolver* solver) {
    if (!sketch || !m_program) return;

    glm::mat4 vp = projection * view;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    drawLines(sketch, vp);
    drawCircles(sketch, vp);
    drawArcs(sketch, vp);
    drawSplines(sketch, vp);
    drawPolygons(sketch, vp);
    drawPoints(sketch, vp);
    drawMidpointDots(sketch, vp);

    // Selection highlight overlay: re-draw selected lines/points in a bright
    // colour so the user can see what Mirror/Copy/Rotate will act on.
    if (tool && tool->hasElementSelection()) {
        const auto& selLines = tool->getSelectedLines();
        const auto& selPoints = tool->getSelectedPoints();
        if (!selLines.empty()) {
            std::vector<float> lv;
            for (const auto& l : sketch->getLines()) {
                if (!selLines.count(l.id)) continue;
                const SketchPoint* a = sketch->getPoint(l.startPointId);
                const SketchPoint* b = sketch->getPoint(l.endPointId);
                if (!a || !b) continue;
                glm::vec3 wa = toWorld(sketch, a->pos);
                glm::vec3 wb = toWorld(sketch, b->pos);
                lv.push_back(wa.x); lv.push_back(wa.y); lv.push_back(wa.z);
                lv.push_back(wb.x); lv.push_back(wb.y); lv.push_back(wb.z);
            }
            if (!lv.empty())
                uploadAndDraw(lv, GL_LINES, glm::vec3(1.0f, 0.85f, 0.1f), vp, 3.5f);
        }
        if (!selPoints.empty()) {
            std::vector<float> pv;
            for (const auto& p : sketch->getPoints()) {
                if (!selPoints.count(p.id)) continue;
                glm::vec3 w = toWorld(sketch, p.pos);
                pv.push_back(w.x); pv.push_back(w.y); pv.push_back(w.z);
            }
            if (!pv.empty())
                uploadAndDraw(pv, GL_POINTS, glm::vec3(1.0f, 0.85f, 0.1f), vp, 8.0f);
        }
    }

    if (solver) drawConstraints(sketch, solver, vp);
    if (tool) {
        drawPreview(sketch, tool, vp);
        drawTrimHover(sketch, tool, vp);
    }

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

glm::vec3 SketchRenderer::toWorld(const Sketch* sketch, glm::vec2 pt2d) const {
    const gp_Pln& pln = sketch->getPlane();
    const gp_Ax3& ax = pln.Position();
    gp_Pnt origin = ax.Location();
    gp_Dir xd = ax.XDirection();
    gp_Dir yd = ax.YDirection();
    gp_Dir nd = ax.Direction();
    return glm::vec3(
        origin.X() + pt2d.x * xd.X() + pt2d.y * yd.X() + 0.001 * nd.X(),
        origin.Y() + pt2d.x * xd.Y() + pt2d.y * yd.Y() + 0.001 * nd.Y(),
        origin.Z() + pt2d.x * xd.Z() + pt2d.y * yd.Z() + 0.001 * nd.Z()
    );
}

void SketchRenderer::drawLines(const Sketch* sketch, const glm::mat4& vp) {
    std::vector<float> verts;
    for (const auto& line : sketch->getLines()) {
        const SketchPoint* p1 = sketch->getPoint(line.startPointId);
        const SketchPoint* p2 = sketch->getPoint(line.endPointId);
        if (!p1 || !p2) continue;

        glm::vec3 w1 = toWorld(sketch, p1->pos);
        glm::vec3 w2 = toWorld(sketch, p2->pos);
        verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
        verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
    }

    glm::vec3 color = glm::vec3(0.2f, 0.6f, 1.0f); // blue for sketch lines
    uploadAndDraw(verts, GL_LINES, color, vp, 2.0f);
}

void SketchRenderer::drawCircles(const Sketch* sketch, const glm::mat4& vp) {
    const int segments = 64;
    std::vector<float> verts;

    for (const auto& circle : sketch->getCircles()) {
        const SketchPoint* center = sketch->getPoint(circle.centerPointId);
        if (!center) continue;

        for (int i = 0; i < segments; i++) {
            float a1 = 2.0f * M_PI * i / segments;
            float a2 = 2.0f * M_PI * (i + 1) / segments;
            float r = static_cast<float>(circle.radius);

            glm::vec2 s1(center->pos.x + r * std::cos(a1), center->pos.y + r * std::sin(a1));
            glm::vec2 s2(center->pos.x + r * std::cos(a2), center->pos.y + r * std::sin(a2));
            glm::vec3 w1 = toWorld(sketch, s1);
            glm::vec3 w2 = toWorld(sketch, s2);
            verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
            verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
        }
    }

    glm::vec3 color = glm::vec3(0.2f, 0.6f, 1.0f);
    uploadAndDraw(verts, GL_LINES, color, vp, 2.0f);
}

void SketchRenderer::drawArcs(const Sketch* sketch, const glm::mat4& vp) {
    const int segments = 32;
    std::vector<float> verts;

    for (const auto& arc : sketch->getArcs()) {
        const SketchPoint* center = sketch->getPoint(arc.centerPointId);
        const SketchPoint* start = sketch->getPoint(arc.startPointId);
        const SketchPoint* end = sketch->getPoint(arc.endPointId);
        if (!center || !start || !end) continue;

        float startAngle = std::atan2(start->pos.y - center->pos.y, start->pos.x - center->pos.x);
        float endAngle = std::atan2(end->pos.y - center->pos.y, end->pos.x - center->pos.x);

        if (endAngle < startAngle) endAngle += 2.0f * M_PI;

        for (int i = 0; i < segments; i++) {
            float t1 = static_cast<float>(i) / segments;
            float t2 = static_cast<float>(i + 1) / segments;
            float a1 = startAngle + t1 * (endAngle - startAngle);
            float a2 = startAngle + t2 * (endAngle - startAngle);

            float r = static_cast<float>(arc.radius);
            glm::vec2 s1(center->pos.x + r * std::cos(a1), center->pos.y + r * std::sin(a1));
            glm::vec2 s2(center->pos.x + r * std::cos(a2), center->pos.y + r * std::sin(a2));
            glm::vec3 w1 = toWorld(sketch, s1);
            glm::vec3 w2 = toWorld(sketch, s2);
            verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
            verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
        }
    }

    glm::vec3 color = glm::vec3(0.2f, 0.6f, 1.0f);
    uploadAndDraw(verts, GL_LINES, color, vp, 2.0f);
}

void SketchRenderer::drawSplines(const Sketch* sketch, const glm::mat4& vp) {
    std::vector<float> verts;

    for (const auto& spline : sketch->getSplines()) {
        for (size_t i = 0; i + 1 < spline.controlPointIds.size(); ++i) {
            const SketchPoint* p1 = sketch->getPoint(spline.controlPointIds[i]);
            const SketchPoint* p2 = sketch->getPoint(spline.controlPointIds[i + 1]);
            if (!p1 || !p2) continue;
            glm::vec3 w1 = toWorld(sketch, p1->pos);
            glm::vec3 w2 = toWorld(sketch, p2->pos);
            verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
            verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
        }
    }

    glm::vec3 color = glm::vec3(0.2f, 0.85f, 0.3f); // green to distinguish from regular lines
    uploadAndDraw(verts, GL_LINES, color, vp, 2.0f);
}

void SketchRenderer::drawPolygons(const Sketch* sketch, const glm::mat4& vp) {
    // Polygons are already made of lines, so they render automatically through drawLines().
    // Optionally highlight the center point of each polygon.
    std::vector<float> verts;

    for (const auto& polygon : sketch->getPolygons()) {
        const SketchPoint* center = sketch->getPoint(polygon.centerPointId);
        if (!center) continue;

        const float s = 0.1f;
        glm::vec3 w1 = toWorld(sketch, glm::vec2(center->pos.x - s, center->pos.y));
        glm::vec3 w2 = toWorld(sketch, glm::vec2(center->pos.x + s, center->pos.y));
        glm::vec3 w3 = toWorld(sketch, glm::vec2(center->pos.x, center->pos.y - s));
        glm::vec3 w4 = toWorld(sketch, glm::vec2(center->pos.x, center->pos.y + s));
        verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
        verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
        verts.push_back(w3.x); verts.push_back(w3.y); verts.push_back(w3.z);
        verts.push_back(w4.x); verts.push_back(w4.y); verts.push_back(w4.z);
    }

    glm::vec3 color = glm::vec3(0.8f, 0.5f, 1.0f); // purple for polygon center markers
    uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
}

void SketchRenderer::drawPoints(const Sketch* sketch, const glm::mat4& vp) {
    std::vector<float> verts;
    for (const auto& pt : sketch->getPoints()) {
        glm::vec3 w = toWorld(sketch, pt.pos);
        verts.push_back(w.x);
        verts.push_back(w.y);
        verts.push_back(w.z);
    }

    glm::vec3 color = glm::vec3(1.0f, 0.8f, 0.2f); // yellow dots for points
    uploadAndDraw(verts, GL_POINTS, color, vp, 2.0f);
}

void SketchRenderer::drawTrimHover(const Sketch* sketch, const SketchTool* tool,
                                   const glm::mat4& vp) {
    if (!sketch || !tool) return;
    const auto& pts = tool->getTrimHoverPoints();
    if (pts.size() < 2) return;

    // Convert the densified 2D points into a GL_LINES vertex stream (each pair
    // adjacent in `pts` becomes one segment).
    std::vector<float> verts;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        glm::vec3 a = toWorld(sketch, pts[i]);
        glm::vec3 b = toWorld(sketch, pts[i + 1]);
        verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
        verts.push_back(b.x); verts.push_back(b.y); verts.push_back(b.z);
    }
    glm::vec3 red(1.0f, 0.25f, 0.25f);
    uploadAndDraw(verts, GL_LINES, red, vp, 4.0f);
}

void SketchRenderer::drawMidpointDots(const Sketch* sketch, const glm::mat4& vp) {
    if (!sketch) return;

    std::vector<float> verts;
    auto pushWorld = [&](glm::vec3 w) {
        verts.push_back(w.x); verts.push_back(w.y); verts.push_back(w.z);
    };

    // Midpoint of each line.
    for (const auto& line : sketch->getLines()) {
        const SketchPoint* p1 = sketch->getPoint(line.startPointId);
        const SketchPoint* p2 = sketch->getPoint(line.endPointId);
        if (!p1 || !p2) continue;
        pushWorld(toWorld(sketch, 0.5f * (p1->pos + p2->pos)));
    }
    // Midpoint of each arc (angular midpoint between start and end going CCW).
    for (const auto& arc : sketch->getArcs()) {
        const SketchPoint* c = sketch->getPoint(arc.centerPointId);
        const SketchPoint* s = sketch->getPoint(arc.startPointId);
        const SketchPoint* e = sketch->getPoint(arc.endPointId);
        if (!c || !s || !e) continue;
        float startA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float endA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        float sweep = endA - startA;
        const float TWO_PI = 2.0f * static_cast<float>(M_PI);
        while (sweep < 0.0f) sweep += TWO_PI;
        while (sweep >= TWO_PI) sweep -= TWO_PI;
        float midA = startA + sweep * 0.5f;
        glm::vec2 mid = c->pos + glm::vec2(std::cos(midA), std::sin(midA))
                                  * static_cast<float>(arc.radius);
        pushWorld(toWorld(sketch, mid));
    }

    // Centroid of the host face (skipped for freestanding-plane sketches).
    glm::vec2 centroid;
    if (sketch->getSourceFaceCentroid(centroid)) {
        pushWorld(toWorld(sketch, centroid));
    }

    if (verts.empty()) return;
    glm::vec3 green(0.2f, 1.0f, 0.4f);
    uploadAndDraw(verts, GL_POINTS, green, vp, 4.0f);
}

void SketchRenderer::drawPreview(const Sketch* sketch, const SketchTool* tool,
                                  const glm::mat4& vp) {
    if (!tool->hasPreview() || !sketch) return;

    glm::vec2 start = tool->getPreviewStart();
    glm::vec2 end = tool->getPreviewEnd();
    SketchToolMode mode = tool->getPreviewType();

    auto pw = [&](glm::vec2 p) -> glm::vec3 { return toWorld(sketch, p); };
    auto pushPt = [](std::vector<float>& v, glm::vec3 p) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
    };

    std::vector<float> verts;
    glm::vec3 color(1.0f, 1.0f, 1.0f);

    if (mode == SketchToolMode::Line || mode == SketchToolMode::Spline) {
        pushPt(verts, pw(start));
        pushPt(verts, pw(end));
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Circle) {
        float radius = glm::length(end - start);
        const int segs = 64;
        for (int i = 0; i < segs; i++) {
            float a1 = 2.0f * (float)M_PI * i / segs;
            float a2 = 2.0f * (float)M_PI * (i + 1) / segs;
            pushPt(verts, pw(glm::vec2(start.x + radius*std::cos(a1), start.y + radius*std::sin(a1))));
            pushPt(verts, pw(glm::vec2(start.x + radius*std::cos(a2), start.y + radius*std::sin(a2))));
        }
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Rectangle) {
        glm::vec2 corners[4] = { start, {end.x, start.y}, end, {start.x, end.y} };
        for (int i = 0; i < 4; i++) {
            pushPt(verts, pw(corners[i]));
            pushPt(verts, pw(corners[(i+1)%4]));
        }
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Polygon) {
        glm::vec2 delta = end - start;
        float radius = glm::length(delta);
        const int sides = tool ? tool->getPolygonSides() : 6;
        // Rotation = direction from centre toward cursor. Matches the actual
        // placement so vertex 0 sits exactly on the (grid-snapped) cursor.
        float rotation = (radius > 1e-6f) ? std::atan2(delta.y, delta.x) : 0.0f;
        for (int i = 0; i < sides; i++) {
            float a1 = rotation + 2.0f * (float)M_PI * i / sides;
            float a2 = rotation + 2.0f * (float)M_PI * (i + 1) / sides;
            pushPt(verts, pw(glm::vec2(start.x + radius*std::cos(a1), start.y + radius*std::sin(a1))));
            pushPt(verts, pw(glm::vec2(start.x + radius*std::cos(a2), start.y + radius*std::sin(a2))));
        }
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Arc) {
        // Arc needs 3 clicks. After click 1: rubber-band line from start to cursor.
        // After click 2: dashed-style preview of the arc through (start, cursor, second).
        int clicks = tool->getClickCount();
        if (clicks == 1) {
            pushPt(verts, pw(start));
            pushPt(verts, pw(end));
            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
        } else if (clicks == 2) {
            glm::vec2 second = tool->getSecondClick();
            glm::vec2 mid = end;
            float ax = start.x, ay = start.y;
            float bx = mid.x, by = mid.y;
            float cx = second.x, cy = second.y;
            float D = 2.0f * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
            if (std::abs(D) > 1e-10f) {
                float ux = ((ax*ax+ay*ay)*(by-cy) + (bx*bx+by*by)*(cy-ay) + (cx*cx+cy*cy)*(ay-by)) / D;
                float uy = ((ax*ax+ay*ay)*(cx-bx) + (bx*bx+by*by)*(ax-cx) + (cx*cx+cy*cy)*(bx-ax)) / D;
                glm::vec2 center(ux, uy);
                float r = glm::length(start - center);
                float sA = std::atan2(start.y - center.y, start.x - center.x);
                float mA = std::atan2(mid.y - center.y, mid.x - center.x);
                float eA = std::atan2(second.y - center.y, second.x - center.x);
                // Sweep from sA to eA in whichever direction passes through mA.
                auto norm = [](float a){ const float TWO_PI = 2.0f * (float)M_PI;
                                          while (a < 0) a += TWO_PI; while (a >= TWO_PI) a -= TWO_PI; return a; };
                float ds = norm(eA - sA);
                float dm = norm(mA - sA);
                float sweep = (dm < ds) ? ds : (ds - 2.0f * (float)M_PI); // CCW or CW
                const int segs = 48;
                for (int i = 0; i < segs; ++i) {
                    float t1 = float(i) / segs;
                    float t2 = float(i + 1) / segs;
                    float a1 = sA + sweep * t1;
                    float a2 = sA + sweep * t2;
                    pushPt(verts, pw(glm::vec2(center.x + r*std::cos(a1), center.y + r*std::sin(a1))));
                    pushPt(verts, pw(glm::vec2(center.x + r*std::cos(a2), center.y + r*std::sin(a2))));
                }
                uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
            } else {
                // Collinear: fall back to chord
                pushPt(verts, pw(start));
                pushPt(verts, pw(second));
                uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
            }
        }
    }
}

void SketchRenderer::drawConstraints(const Sketch* sketch, const SketchSolver* solver,
                                      const glm::mat4& vp) {
    if (!solver) return;

    const auto& constraints = solver->getConstraints();
    if (constraints.empty()) return;

    const float markerSize = 0.08f;

    // Helper: convert 2D sketch coord to 3D with slight offset along normal
    auto tw = [&](float x, float y) -> glm::vec3 {
        return toWorld(sketch, glm::vec2(x, y));
    };
    auto pushV = [](std::vector<float>& v, glm::vec3 p) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
    };
    // Compatibility shim: replaces the old "push x, push y, push z" pattern
    // by transforming (x,y) through the sketch plane
    auto push2d = [&](std::vector<float>& v, float x, float y) {
        glm::vec3 w = tw(x, y);
        v.push_back(w.x); v.push_back(w.y); v.push_back(w.z);
    };

    for (const auto& c : constraints) {
        glm::vec3 color = c.isSatisfied ? glm::vec3(0.0f, 0.85f, 0.3f)   // green for satisfied
                                        : glm::vec3(0.95f, 0.15f, 0.15f); // red for unsatisfied

        std::vector<float> verts;

        if (c.type == ConstraintType::Horizontal || c.type == ConstraintType::Vertical) {
            // Draw a small marker at the midpoint of the constrained line
            // entityA refers to the line id
            glm::vec2 midpoint(0.0f);
            bool found = false;
            for (const auto& line : sketch->getLines()) {
                if (line.id == c.entityA) {
                    const SketchPoint* p1 = sketch->getPoint(line.startPointId);
                    const SketchPoint* p2 = sketch->getPoint(line.endPointId);
                    if (p1 && p2) {
                        midpoint = (p1->pos + p2->pos) * 0.5f;
                        found = true;
                    }
                    break;
                }
            }
            if (!found) continue;

            // Offset the marker slightly above the line
            glm::vec2 offset(0.0f, markerSize * 1.5f);

            if (c.type == ConstraintType::Horizontal) {
                // Draw "H" using 3 line segments
                float s = markerSize * 0.5f;
                glm::vec2 base = midpoint + offset;
                // Left vertical
                push2d(verts, base.x - s, base.y - s);
                push2d(verts, base.x - s, base.y + s);
                // Right vertical
                push2d(verts, base.x + s, base.y - s);
                push2d(verts, base.x + s, base.y + s);
                // Horizontal bar
                push2d(verts, base.x - s, base.y);
                push2d(verts, base.x + s, base.y);
            } else {
                // Draw "V" using 2 line segments
                float s = markerSize * 0.5f;
                glm::vec2 base = midpoint + offset;
                // Left arm of V
                push2d(verts, base.x - s, base.y + s);
                push2d(verts, base.x, base.y - s);
                // Right arm of V
                push2d(verts, base.x + s, base.y + s);
                push2d(verts, base.x, base.y - s);
            }

            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);

        } else if (c.type == ConstraintType::Coincident) {
            // Draw a larger yellow/green dot at the coincident point
            // entityA is one of the coincident points
            const SketchPoint* pt = sketch->getPoint(c.entityA);
            if (!pt) continue;

            // Draw a small diamond shape around the point
            float s = markerSize * 0.6f;
            glm::vec2 p = pt->pos;
            push2d(verts, p.x, p.y + s);
            push2d(verts, p.x + s, p.y);

            push2d(verts, p.x + s, p.y);
            push2d(verts, p.x, p.y - s);

            push2d(verts, p.x, p.y - s);
            push2d(verts, p.x - s, p.y);

            push2d(verts, p.x - s, p.y);
            push2d(verts, p.x, p.y + s);

            // Use yellow for coincident markers
            glm::vec3 coincColor = c.isSatisfied ? glm::vec3(1.0f, 0.9f, 0.2f)
                                                 : glm::vec3(0.95f, 0.15f, 0.15f);
            uploadAndDraw(verts, GL_LINES, coincColor, vp, 2.0f);

        } else if (c.type == ConstraintType::Distance) {
            // Draw dimension line between two points with perpendicular end caps
            const SketchPoint* p1 = sketch->getPoint(c.entityA);
            const SketchPoint* p2 = sketch->getPoint(c.entityB);
            if (!p1 || !p2) continue;

            glm::vec2 a = p1->pos;
            glm::vec2 b = p2->pos;
            glm::vec2 dir = b - a;
            float len = glm::length(dir);
            if (len < 1e-6f) continue;

            glm::vec2 unit = dir / len;
            glm::vec2 perp(-unit.y, unit.x);

            // Offset the dimension line
            float offsetDist = markerSize * 3.0f;
            glm::vec2 a_off = a + perp * offsetDist;
            glm::vec2 b_off = b + perp * offsetDist;

            // Main dimension line
            push2d(verts, a_off.x, a_off.y);
            push2d(verts, b_off.x, b_off.y);

            // Extension lines from points to dimension line
            float capLen = markerSize * 0.8f;
            glm::vec2 a_ext = a + perp * (offsetDist - capLen);
            glm::vec2 b_ext = b + perp * (offsetDist - capLen);
            glm::vec2 a_ext2 = a + perp * (offsetDist + capLen);
            glm::vec2 b_ext2 = b + perp * (offsetDist + capLen);

            // Left extension
            push2d(verts, a_ext.x, a_ext.y);
            push2d(verts, a_ext2.x, a_ext2.y);
            // Right extension
            push2d(verts, b_ext.x, b_ext.y);
            push2d(verts, b_ext2.x, b_ext2.y);

            // Leader lines from actual points to dimension line
            push2d(verts, a.x, a.y);
            push2d(verts, a_off.x, a_off.y);

            push2d(verts, b.x, b.y);
            push2d(verts, b_off.x, b_off.y);

            uploadAndDraw(verts, GL_LINES, color, vp, 1.0f);

        } else if (c.type == ConstraintType::Fixed) {
            // Draw an X marker at the fixed point
            const SketchPoint* pt = sketch->getPoint(c.entityA);
            if (!pt) continue;

            float s = markerSize * 0.6f;
            glm::vec2 p = pt->pos;
            // Diagonal 1
            push2d(verts, p.x - s, p.y - s);
            push2d(verts, p.x + s, p.y + s);
            // Diagonal 2
            push2d(verts, p.x - s, p.y + s);
            push2d(verts, p.x + s, p.y - s);

            // Use a specific red-orange for fixed constraints
            glm::vec3 fixedColor = c.isSatisfied ? glm::vec3(0.9f, 0.4f, 0.1f)
                                                 : glm::vec3(0.95f, 0.15f, 0.15f);
            uploadAndDraw(verts, GL_LINES, fixedColor, vp, 2.0f);

        } else if (c.type == ConstraintType::Radius) {
            // Draw a small circle indicator near the constrained circle/arc center
            // entityA refers to the circle/arc id; look for matching circle center
            glm::vec2 center(0.0f);
            bool found = false;
            for (const auto& circle : sketch->getCircles()) {
                if (circle.id == c.entityA) {
                    const SketchPoint* cp = sketch->getPoint(circle.centerPointId);
                    if (cp) {
                        center = cp->pos;
                        found = true;
                    }
                    break;
                }
            }
            if (!found) {
                for (const auto& arc : sketch->getArcs()) {
                    if (arc.id == c.entityA) {
                        const SketchPoint* cp = sketch->getPoint(arc.centerPointId);
                        if (cp) {
                            center = cp->pos;
                            found = true;
                        }
                        break;
                    }
                }
            }
            if (!found) continue;

            // Draw small "R" indicator: vertical line + small bump
            float s = markerSize * 0.5f;
            glm::vec2 base = center + glm::vec2(markerSize * 1.5f, markerSize * 1.5f);
            // Vertical stroke
            push2d(verts, base.x, base.y - s);
            push2d(verts, base.x, base.y + s);
            // Top horizontal
            push2d(verts, base.x, base.y + s);
            push2d(verts, base.x + s, base.y + s);
            // Diagonal kick
            push2d(verts, base.x, base.y);
            push2d(verts, base.x + s, base.y - s);

            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);

        } else if (c.type == ConstraintType::Parallel || c.type == ConstraintType::Perpendicular) {
            // Draw a small marker at the midpoint of the first line
            glm::vec2 midpoint(0.0f);
            bool found = false;
            for (const auto& line : sketch->getLines()) {
                if (line.id == c.entityA) {
                    const SketchPoint* p1 = sketch->getPoint(line.startPointId);
                    const SketchPoint* p2 = sketch->getPoint(line.endPointId);
                    if (p1 && p2) {
                        midpoint = (p1->pos + p2->pos) * 0.5f;
                        found = true;
                    }
                    break;
                }
            }
            if (!found) continue;

            float s = markerSize * 0.5f;
            glm::vec2 base = midpoint + glm::vec2(0.0f, markerSize * 1.5f);

            if (c.type == ConstraintType::Parallel) {
                // Two parallel short lines
                push2d(verts, base.x - s * 0.3f, base.y - s);
                push2d(verts, base.x - s * 0.3f, base.y + s);
                push2d(verts, base.x + s * 0.3f, base.y - s);
                push2d(verts, base.x + s * 0.3f, base.y + s);
            } else {
                // Small right angle symbol
                push2d(verts, base.x - s, base.y - s);
                push2d(verts, base.x - s, base.y + s);
                push2d(verts, base.x - s, base.y - s);
                push2d(verts, base.x + s, base.y - s);
            }

            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
        }
    }

    // Draw overall sketch state indicator as a colored point at the origin
    SketchState state = solver->getState();
    glm::vec3 stateColor;
    switch (state) {
        case SketchState::FullyConstrained:
            stateColor = glm::vec3(0.0f, 0.85f, 0.3f);  // green
            break;
        case SketchState::UnderConstrained:
            stateColor = glm::vec3(0.2f, 0.6f, 1.0f);   // blue
            break;
        case SketchState::OverConstrained:
            stateColor = glm::vec3(0.95f, 0.15f, 0.15f); // red
            break;
    }

    // Draw a state indicator dot at the top-left of the sketch area
    std::vector<float> stateVerts;
    float indicatorSize = markerSize * 0.8f;
    // Small filled-looking square via lines at a fixed position
    float ix = -5.0f, iy = 5.0f;
    push2d(stateVerts, ix - indicatorSize, iy - indicatorSize);
    push2d(stateVerts, ix + indicatorSize, iy - indicatorSize);
    push2d(stateVerts, ix + indicatorSize, iy - indicatorSize);
    push2d(stateVerts, ix + indicatorSize, iy + indicatorSize);
    push2d(stateVerts, ix + indicatorSize, iy + indicatorSize);
    push2d(stateVerts, ix - indicatorSize, iy + indicatorSize);
    push2d(stateVerts, ix - indicatorSize, iy + indicatorSize);
    push2d(stateVerts, ix - indicatorSize, iy - indicatorSize);

    uploadAndDraw(stateVerts, GL_LINES, stateColor, vp, 3.0f);
}

void SketchRenderer::renderRegionBoundary(const Sketch* sketch, int regionIndex,
                                          const glm::vec3& color, float lineWidth,
                                          const glm::mat4& view, const glm::mat4& projection) {
    if (!sketch || !m_program) return;
    auto regions = sketch->buildRegions();
    if (regionIndex < 0 || regionIndex >= static_cast<int>(regions.size())) return;

    glm::mat4 vp = projection * view;
    const auto& region = regions[regionIndex];

    auto wireToSegments = [&](const TopoDS_Wire& wire, std::vector<float>& verts) {
        for (BRepTools_WireExplorer ex(wire); ex.More(); ex.Next()) {
            TopoDS_Edge edge = TopoDS::Edge(ex.Current());
            double f, l;
            Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
            if (curve.IsNull()) continue;
            const int samples = 64;
            gp_Pnt prev;
            for (int i = 0; i <= samples; ++i) {
                double t = f + (l - f) * (double(i) / samples);
                gp_Pnt p;
                curve->D0(t, p);
                if (i > 0) {
                    verts.push_back(static_cast<float>(prev.X()));
                    verts.push_back(static_cast<float>(prev.Y()));
                    verts.push_back(static_cast<float>(prev.Z()));
                    verts.push_back(static_cast<float>(p.X()));
                    verts.push_back(static_cast<float>(p.Y()));
                    verts.push_back(static_cast<float>(p.Z()));
                }
                prev = p;
            }
        }
    };

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::vector<float> verts;
    wireToSegments(region.outerWire, verts);
    for (const auto& h : region.holeWires) wireToSegments(h, verts);
    uploadAndDraw(verts, GL_LINES, color, vp, lineWidth);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

void SketchRenderer::renderFaceGrid(const Sketch* sketch, float faceExtent, float gridStep,
                                    const glm::mat4& view, const glm::mat4& projection) {
    if (!sketch || !m_program) return;
    if (faceExtent <= 0.0f || gridStep <= 0.0f) return;

    glm::mat4 vp = projection * view;

    // Centre the grid on the host face's centroid in sketch-plane 2D — otherwise
    // a face that doesn't straddle the sketch origin gets a grid that runs off
    // to one side (it's still aligned to the ground grid, just not over the face).
    glm::vec2 c{0.0f};
    sketch->getSourceFaceCentroid(c);

    // Split lines into minor and every-10th major so the major lines read clearly.
    std::vector<float> minorVerts, majorVerts;
    int steps = static_cast<int>(std::floor(faceExtent / gridStep));
    auto pushLine = [](std::vector<float>& v, glm::vec3 a, glm::vec3 b) {
        v.push_back(a.x); v.push_back(a.y); v.push_back(a.z);
        v.push_back(b.x); v.push_back(b.y); v.push_back(b.z);
    };
    for (int i = -steps; i <= steps; ++i) {
        float t = i * gridStep;
        std::vector<float>& dst = (i % 10 == 0) ? majorVerts : minorVerts;
        pushLine(dst, toWorld(sketch, glm::vec2(c.x + t, c.y - faceExtent)),
                      toWorld(sketch, glm::vec2(c.x + t, c.y + faceExtent)));
        pushLine(dst, toWorld(sketch, glm::vec2(c.x - faceExtent, c.y + t)),
                      toWorld(sketch, glm::vec2(c.x + faceExtent, c.y + t)));
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Faint minor lines, then brighter/thicker major (every 10th) on top.
    uploadAndDraw(minorVerts, GL_LINES, glm::vec3(0.33f, 0.37f, 0.42f), vp, 0.7f);
    uploadAndDraw(majorVerts, GL_LINES, glm::vec3(0.62f, 0.68f, 0.78f), vp, 1.6f);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

bool SketchRenderer::compileShader(unsigned int& shader, unsigned int type, const char* source) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "SketchRenderer shader error: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

} // namespace materializr
