#include "Picker.h"
#include "Camera.h"
#include "../core/Document.h"

#include <glm/gtc/matrix_transform.hpp>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <BRepGProp_Face.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <limits>

namespace materializr {

Picker::Picker() {}

void Picker::screenToRay(float sx, float sy, float vpW, float vpH,
                         const Camera& camera,
                         glm::vec3& rayOrigin, glm::vec3& rayDir)
{
    // Convert screen coordinates to normalized device coordinates
    float ndcX = (2.0f * sx) / vpW - 1.0f;
    float ndcY = 1.0f - (2.0f * sy) / vpH; // flip Y

    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 proj = camera.getProjectionMatrix();
    glm::mat4 invVP = glm::inverse(proj * view);

    // Unproject near and far points
    glm::vec4 nearPt = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farPt  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);

    nearPt /= nearPt.w;
    farPt  /= farPt.w;

    rayOrigin = glm::vec3(nearPt);
    rayDir = glm::normalize(glm::vec3(farPt) - glm::vec3(nearPt));
}

bool Picker::rayIntersectsBBox(const glm::vec3& origin, const glm::vec3& dir,
                               const TopoDS_Shape& shape, float& tMin)
{
    Bnd_Box bbox;
    BRepBndLib::Add(shape, bbox);

    if (bbox.IsVoid()) return false;

    double xMin, yMin, zMin, xMax, yMax, zMax;
    bbox.Get(xMin, yMin, zMin, xMax, yMax, zMax);

    // Add small gap to avoid zero-size boxes
    float gap = 1e-5f;
    glm::vec3 bMin(static_cast<float>(xMin) - gap,
                   static_cast<float>(yMin) - gap,
                   static_cast<float>(zMin) - gap);
    glm::vec3 bMax(static_cast<float>(xMax) + gap,
                   static_cast<float>(yMax) + gap,
                   static_cast<float>(zMax) + gap);

    // Ray-AABB intersection (slab method)
    float tMinVal = -std::numeric_limits<float>::max();
    float tMaxVal =  std::numeric_limits<float>::max();

    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-8f) {
            // Ray is parallel to slab
            if (origin[i] < bMin[i] || origin[i] > bMax[i])
                return false;
        } else {
            float invD = 1.0f / dir[i];
            float t1 = (bMin[i] - origin[i]) * invD;
            float t2 = (bMax[i] - origin[i]) * invD;

            if (t1 > t2) std::swap(t1, t2);

            tMinVal = std::max(tMinVal, t1);
            tMaxVal = std::min(tMaxVal, t2);

            if (tMinVal > tMaxVal)
                return false;
        }
    }

    if (tMaxVal < 0.0f)
        return false;

    tMin = tMinVal > 0.0f ? tMinVal : tMaxVal;
    return true;
}

// Moller-Trumbore ray-triangle intersection
static bool rayTriangleIntersect(const glm::vec3& origin, const glm::vec3& dir,
                                 const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                 float& t)
{
    const float EPSILON = 1e-7f;

    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(dir, edge2);
    float a = glm::dot(edge1, h);

    if (a > -EPSILON && a < EPSILON)
        return false; // parallel

    float f = 1.0f / a;
    glm::vec3 s = origin - v0;
    float u = f * glm::dot(s, h);

    if (u < 0.0f || u > 1.0f)
        return false;

    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(dir, q);

    if (v < 0.0f || u + v > 1.0f)
        return false;

    t = f * glm::dot(edge2, q);
    return t > EPSILON;
}

int Picker::findNearestFace(const glm::vec3& origin, const glm::vec3& dir,
                            const TopoDS_Shape& shape, float& bestDist,
                            glm::vec3& hitPt, TopoDS_Shape& hitFace)
{
    // Ensure the shape is tessellated
    BRepMesh_IncrementalMesh meshGen(shape, 0.1);
    meshGen.Perform();

    int faceIndex = -1;
    int currentFace = 0;
    bestDist = std::numeric_limits<float>::max();

    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face& face = TopoDS::Face(explorer.Current());
        TopLoc_Location location;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);

        if (triangulation.IsNull()) {
            ++currentFace;
            continue;
        }

        const gp_Trsf& trsf = location.Transformation();
        bool hasTransform = !location.IsIdentity();

        int nbTriangles = triangulation->NbTriangles();
        for (int i = 1; i <= nbTriangles; ++i) {
            const Poly_Triangle& tri = triangulation->Triangle(i);

            int n1, n2, n3;
            tri.Get(n1, n2, n3);

            gp_Pnt p1 = triangulation->Node(n1);
            gp_Pnt p2 = triangulation->Node(n2);
            gp_Pnt p3 = triangulation->Node(n3);

            if (hasTransform) {
                p1.Transform(trsf);
                p2.Transform(trsf);
                p3.Transform(trsf);
            }

            glm::vec3 v0(static_cast<float>(p1.X()), static_cast<float>(p1.Y()), static_cast<float>(p1.Z()));
            glm::vec3 v1(static_cast<float>(p2.X()), static_cast<float>(p2.Y()), static_cast<float>(p2.Z()));
            glm::vec3 v2(static_cast<float>(p3.X()), static_cast<float>(p3.Y()), static_cast<float>(p3.Z()));

            float t = 0.0f;
            if (rayTriangleIntersect(origin, dir, v0, v1, v2, t)) {
                if (t < bestDist) {
                    bestDist = t;
                    hitPt = origin + dir * t;
                    hitFace = face;
                    faceIndex = currentFace;
                }
            }
        }

        ++currentFace;
    }

    return faceIndex;
}

void Picker::findNearestEdge(const TopoDS_Shape& shape, const glm::vec3& hitPt,
                             const glm::vec3& facePlaneNormal,
                             float screenX, float screenY, float vpW, float vpH,
                             const Camera& camera,
                             TopoDS_Shape& nearestEdge, float& screenDist)
{
    glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
    glm::vec2 mouse(screenX, screenY);
    screenDist = 1e6f;

    auto worldToScreen = [&](glm::vec3 p) -> glm::vec2 {
        glm::vec4 clip = vp * glm::vec4(p, 1.0f);
        if (std::abs(clip.w) < 1e-6f) return glm::vec2(1e6f);
        glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
        return glm::vec2((ndc.x * 0.5f + 0.5f) * vpW, (0.5f - ndc.y * 0.5f) * vpH);
    };

    // Face-plane occlusion: edges behind the picked face's tangent plane at
    // the hit point are hidden and must not steal the click. `facePlaneNormal`
    // is oriented camera-side by the caller, so signed distance against the
    // plane is positive in front and negative behind. The 0.3 mm slack covers
    // tessellation noise on curved silhouettes while rejecting any wall ≥ 0.3 mm
    // — and it's camera-angle-independent, unlike a view-direction depth check.
    bool havePlaneCheck = glm::length(facePlaneNormal) > 0.5f;
    glm::vec3 planeN = havePlaneCheck ? glm::normalize(facePlaneNormal) : glm::vec3(0.0f);
    const float planeTol = 0.3f;

    for (TopExp_Explorer exp(shape, TopAbs_EDGE); exp.More(); exp.Next()) {
        TopoDS_Edge edge = TopoDS::Edge(exp.Current());
        try {
            BRepAdaptor_Curve curve(edge);
            GCPnts_TangentialDeflection discretizer(curve, 0.1, 0.1);
            int nPts = discretizer.NbPoints();

            for (int i = 1; i < nPts; i++) {
                gp_Pnt p1 = discretizer.Value(i);
                gp_Pnt p2 = discretizer.Value(i + 1);
                glm::vec3 w1(p1.X(), p1.Y(), p1.Z());
                glm::vec3 w2(p2.X(), p2.Y(), p2.Z());

                glm::vec2 s1 = worldToScreen(w1);
                glm::vec2 s2 = worldToScreen(w2);

                glm::vec2 seg = s2 - s1;
                float segLen2 = glm::dot(seg, seg);
                float t = 0.0f;
                float d;
                if (segLen2 < 1e-6f) {
                    d = glm::length(mouse - s1);
                } else {
                    t = glm::clamp(glm::dot(mouse - s1, seg) / segLen2, 0.0f, 1.0f);
                    d = glm::length(mouse - (s1 + t * seg));
                }

                // Skip edge segments hidden behind the picked face's plane.
                glm::vec3 wp = w1 + t * (w2 - w1);
                if (havePlaneCheck &&
                    glm::dot(wp - hitPt, planeN) < -planeTol) continue;

                if (d < screenDist) {
                    screenDist = d;
                    nearestEdge = edge;
                }
            }
        } catch (...) {
            continue;
        }
    }
}

PickResult Picker::pick(float screenX, float screenY,
                        float viewportWidth, float viewportHeight,
                        const Camera& camera, const Document& doc)
{
    PickResult result;

    glm::vec3 rayOrigin, rayDir;
    screenToRay(screenX, screenY, viewportWidth, viewportHeight, camera, rayOrigin, rayDir);

    float nearestDist = std::numeric_limits<float>::max();

    std::vector<int> bodyIds = doc.getAllBodyIds();
    for (int bodyId : bodyIds) {
        if (!doc.isBodyVisible(bodyId)) continue;

        const TopoDS_Shape& shape = doc.getBody(bodyId);
        if (shape.IsNull()) continue;

        // Quick bounding box rejection
        float bboxT = 0.0f;
        if (!rayIntersectsBBox(rayOrigin, rayDir, shape, bboxT)) {
            continue;
        }

        // Detailed face-level test
        float faceDist = 0.0f;
        glm::vec3 faceHitPt(0.0f);
        TopoDS_Shape faceShape;
        int faceIdx = findNearestFace(rayOrigin, rayDir, shape, faceDist, faceHitPt, faceShape);

        if (faceIdx >= 0 && faceDist < nearestDist) {
            nearestDist = faceDist;
            result.hit = true;
            result.bodyId = bodyId;
            result.faceIndex = faceIdx;
            result.pickedShape = faceShape;
            result.hitPoint = faceHitPt;
            result.distance = faceDist;

            // Compute the picked face's surface normal at the hit point and
            // orient it toward the camera. findNearestEdge uses this to reject
            // edges that lie behind the face's tangent plane (the proper
            // "is this edge on the same side as the camera?" check).
            glm::vec3 facePlaneN(0.0f);
            try {
                TopoDS_Face f = TopoDS::Face(faceShape);
                BRepGProp_Face prop(f);
                double u1, u2, v1, v2;
                prop.Bounds(u1, u2, v1, v2);
                gp_Pnt fp; gp_Vec fn;
                prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, fp, fn);
                if (fn.Magnitude() > 1e-10) {
                    fn.Normalize();
                    facePlaneN = glm::vec3(fn.X(), fn.Y(), fn.Z());
                    glm::vec3 toCam = glm::normalize(camera.getPosition() - faceHitPt);
                    if (glm::dot(facePlaneN, toCam) < 0.0f) facePlaneN = -facePlaneN;
                }
            } catch (...) {}

            // Find nearest edge on this body
            TopoDS_Shape edgeShape;
            float edgeDist = 1e6f;
            findNearestEdge(shape, faceHitPt, facePlaneN, screenX, screenY,
                           viewportWidth, viewportHeight, camera,
                           edgeShape, edgeDist);
            result.nearestEdge = edgeShape;
            result.edgeScreenDist = edgeDist;

            // Project the face's vertices to screen space and take the
            // bbox's shorter side. Callers use this to clamp the edge-
            // promotion threshold so a small on-screen face doesn't lose
            // every interior click to one of its own boundary edges.
            float sMinX = 1e9f, sMinY = 1e9f, sMaxX = -1e9f, sMaxY = -1e9f;
            int sN = 0;
            glm::mat4 svp = camera.getProjectionMatrix() * camera.getViewMatrix();
            for (TopExp_Explorer vex(faceShape, TopAbs_VERTEX); vex.More(); vex.Next()) {
                gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vex.Current()));
                glm::vec4 clip = svp * glm::vec4(p.X(), p.Y(), p.Z(), 1.0f);
                if (std::abs(clip.w) < 1e-6f) continue;
                float sx = (clip.x / clip.w * 0.5f + 0.5f) * viewportWidth;
                float sy = (0.5f - clip.y / clip.w * 0.5f) * viewportHeight;
                sMinX = std::min(sMinX, sx); sMaxX = std::max(sMaxX, sx);
                sMinY = std::min(sMinY, sy); sMaxY = std::max(sMaxY, sy);
                ++sN;
            }
            if (sN > 0) {
                result.faceScreenSize = std::min(sMaxX - sMinX, sMaxY - sMinY);
            }
        }
    }

    return result;
}

} // namespace materializr
