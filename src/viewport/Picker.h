#pragma once

#include <glm/glm.hpp>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

class Document;
class SelectionManager;

namespace materializr {

class Camera;

struct PickResult {
    bool hit = false;
    int bodyId = -1;
    int faceIndex = -1;
    // Set when the hit was on a construction plane (bodyId == -1). The
    // viewport input handler builds a SelectionType::Plane entry from this.
    int planeId = -1;
    // Construction-axis hit (bodyId == -1, planeId == -1). Builds a
    // SelectionType::Axis entry.
    int axisId = -1;
    TopoDS_Shape pickedShape;    // the picked face
    TopoDS_Shape nearestEdge;    // closest edge to hit point (if any)
    float edgeScreenDist = 1e6f; // screen distance to nearest edge in pixels
    // Closest vertex of the picked face to the cursor in screen pixels.
    // The viewport input handler uses this to expand a corner click into a
    // multi-edge selection (all edges meeting at that vertex), so the user
    // can fillet/chamfer a whole corner in one click. Empty when nothing
    // was hit.
    TopoDS_Shape nearestVertex;
    float vertexScreenDist = 1e6f;
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

    // Per-body diagnostic prints inside pick() — set for ONE call (the
    // re-pick the click diagnostic runs), then cleared. Static so call
    // sites need no plumbing.
    static bool s_verbose;

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

    // Face-resolving ray test for imported tessellated meshes. These can carry
    // thousands of faces, so the per-face findNearestFace path (which re-meshes
    // and explores every face + every edge each hover frame) is ruinous. Instead
    // we flatten the body ONCE into a cached list of world-space triangles, each
    // tagged with its owning face, and ray-test that. Returns the face index
    // (exploration order) and sets hitFace to the actual TopoDS_Face — so face
    // selection and sketch-on-face keep working — or -1 on a miss.
    int pickMeshBody(const glm::vec3& origin, const glm::vec3& dir,
                     const TopoDS_Shape& shape, float& bestDist,
                     glm::vec3& hitPt, TopoDS_Shape& hitFace);

    struct MeshTri { glm::vec3 v[3]; TopoDS_Face face; int faceIdx; };
    struct MeshCacheEntry {
        TopoDS_Shape shape;            // IsEqual() detects a pose change → rebuild
        std::vector<MeshTri> tris;
    };
    std::unordered_map<const void*, MeshCacheEntry> m_meshCache;

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
