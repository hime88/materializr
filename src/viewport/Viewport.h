#pragma once

#include "gl_common.h"

#include "Camera.h"

#include <glm/glm.hpp>

namespace materializr {

/// Manages the 3D viewport FBO and input within an ImGui window.
class Viewport {
public:
    Viewport();
    ~Viewport();

    /// Recreate the framebuffer at a new size.
    void resize(int width, int height);

    /// Bind the viewport FBO for off-screen rendering.
    void bind();

    /// Unbind the viewport FBO (restore default framebuffer). When MSAA is on,
    /// this also resolves the multisampled buffer into the displayed texture.
    void unbind();

    /// Set the MSAA sample count (0 = off; clamped to the GL maximum). Recreates
    /// the framebuffer when the count changes.
    void setSamples(int samples);
    int getSamples() const { return m_samples; }

    /// Get the color texture ID for ImGui::Image().
    unsigned int getTextureID() const { return m_colorTexture; }

    /// Get the current viewport dimensions.
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    /// Access the camera.
    Camera& getCamera() { return m_camera; }
    const Camera& getCamera() const { return m_camera; }

private:
    void createFramebuffer();
    void destroyFramebuffer();
    void handleInput();

    Camera m_camera;

    unsigned int m_fbo = 0;
    unsigned int m_colorTexture = 0;
    unsigned int m_depthRenderbuffer = 0;

    // Multisampled render target (only created when m_samples > 0). The scene is
    // drawn here, then blitted ("resolved") into m_fbo/m_colorTexture for display.
    unsigned int m_msaaFbo = 0;
    unsigned int m_msaaColor = 0;
    unsigned int m_msaaDepth = 0;
    int m_samples = 0;

    int m_width = 1280;
    int m_height = 720;

    // Input state
    bool m_isHovered = false;
    glm::vec2 m_lastMousePos = glm::vec2(0.0f);
    bool m_isDragging = false;
};

} // namespace materializr
