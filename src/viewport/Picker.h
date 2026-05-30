#pragma once

#include <glm/glm.hpp>
#include <TopoDS_Shape.hxx>
#include <optional>

class Document;
class SelectionManager;

namespace materializr {

class Camera;

struct PickResult {
    bool hit = false;
    int bodyId = -1;
    int faceIndex = -1;
    TopoDS_Shape pickedShape;    // the picked face
    TopoDS_Shape nearestEdge;    // closest edge to hit point (if any)
    float edgeScreenDist = 1e6f; // screen distance to nearest edge in pixels
    // Min(width, height) of the picked face's projected bbox in screen pixels.
    // The face-vs-edge classifier scales the edge-promotion threshold by this
    // so a small face on screen doesn't have every interior pixel "near" some
    // edge of its own boundary.
    float faceScreenSize = 1e6f;
    glm::vec3 hitPoint{0};
    float distance = 0;
};

class Picker {
public:
    Picker();

    // Cast a ray from screen coordinates and find the nearest hit
    PickResult pick(float screenX, float screenY,
                    float viewportWidth, float viewportHeight,
                    const Camera& camera, const Document& doc);

private:
    // Unproject screen point to world ray
    void screenToRay(float sx, float sy, float vpW, float vpH,
                     const Camera& camera,
                     glm::vec3& rayOrigin, glm::vec3& rayDir);

    // Test ray against an OCCT shape's bounding box
    bool rayIntersectsBBox(const glm::vec3& origin, const glm::vec3& dir,
                           const TopoDS_Shape& shape, float& tMin);

    int findNearestFace(const glm::vec3& origin, const glm::vec3& dir,
                        const TopoDS_Shape& shape, float& bestDist,
                        glm::vec3& hitPt, TopoDS_Shape& hitFace);

    // Find the nearest edge to a world-space point, return screen distance.
    // `facePlaneNormal` is the outward (camera-facing) normal of the picked
    // face at `hitPt`; edges that lie noticeably BEHIND the face's tangent
    // plane at the hit are rejected so back-side edges don't get clicked
    // through. Pass a zero vector to skip the plane check.
    void findNearestEdge(const TopoDS_Shape& shape, const glm::vec3& hitPt,
                         const glm::vec3& facePlaneNormal,
                         float screenX, float screenY, float vpW, float vpH,
                         const Camera& camera,
                         TopoDS_Shape& nearestEdge, float& screenDist);
};

} // namespace materializr
