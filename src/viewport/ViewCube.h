#pragma once

#include <imgui.h>
#include <glm/glm.hpp>

namespace materializr {

class Camera;

enum class ViewCubeAction {
    None, Front, Back, Left, Right, Top, Bottom,
    FrontTopRight, FrontTopLeft, BackTopRight, BackTopLeft,
    FrontBottomRight, FrontBottomLeft, BackBottomRight, BackBottomLeft,
    // Edge (two-face) views — clicking the seam between two faces looks down
    // that edge so both faces are seen at once. One zero component each.
    TopFront, TopBack, TopLeft, TopRight,
    BottomFront, BottomBack, BottomLeft, BottomRight,
    FrontLeft, FrontRight, BackLeft, BackRight,
    RotateLeft, RotateRight, RotateUp, RotateDown,
    // Roll the camera 90° around the view axis (CCW / CW). Lets the user
    // re-orient a snapped ortho view (e.g. Top) without un-snapping.
    RollLeft, RollRight,
    // Snap to the default 3/4 isometric view (FrontTopRight equivalent).
    Home
};

class ViewCube {
public:
    ViewCube();

    // Render the view cube overlay. Call inside an ImGui window. Returns the
    // action if a face was clicked. `invertDrag` flips the orbit sign when the
    // user drags the cube body (configurable in Settings). `lightMode` flips the
    // "ink" (labels/borders/arrows/home) from light to dark for the light theme;
    // the blue/grey cube faces and the yellow hover are unchanged.
    ViewCubeAction render(Camera& camera, bool invertDrag = false,
                          bool lightMode = false);

    // True while the mouse is over the cube/ring widget — the viewport uses
    // this to suppress its own selection logic so cube clicks don't pass through.
    bool wasHovered() const { return m_lastHovered; }

private:
    float m_size = 120.0f;
    bool  m_lastHovered = false;
    // Cube drag-to-orbit state: a press on a face arms a pending snap; if the
    // user drags before releasing, treat it as a free orbit instead and
    // suppress the snap on release.
    bool           m_cubeDragging = false;
    ViewCubeAction m_pendingClick = ViewCubeAction::None;
};

} // namespace materializr
