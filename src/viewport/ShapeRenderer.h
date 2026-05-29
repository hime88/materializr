#pragma once

#include "gl_common.h"

#include <glm/glm.hpp>

#include <TopoDS_Shape.hxx>

#include <vector>
#include <string>

namespace materializr {

/// Renders OCCT TopoDS_Shape objects as OpenGL meshes using Blinn-Phong shading.
/// Supports selection highlighting with outline via stencil technique.
/// Configurable lighting controls. Tuned to reduce the harsh single-direction
/// shadowing the default scene used to have.
struct LightingParams {
    float ambient      = 0.40f; // 0..1 base illumination; higher = softer shadows
    bool  headlight    = false; // key light tracks the camera
    bool  fill         = true;  // enable the soft opposing fill light
    float fillStrength = 0.35f; // contribution of the fill light when enabled
};

class ShapeRenderer {
public:
    ShapeRenderer();
    ~ShapeRenderer();

    /// Initialize shaders. Call once after OpenGL context is ready.
    bool initialize();

    /// Tessellate a TopoDS_Shape and store the resulting mesh.
    /// Returns the mesh index (for later color/selection control).
    /// `angularDeflection` (radians) controls faceting of curved surfaces — a
    /// tighter angle makes fillets/holes/cylinders visibly smoother while adding
    /// almost no triangles to flat faces.
    int tessellate(const TopoDS_Shape& shape, float deflection = 0.1f,
                   float angularDeflection = 0.2f);

    /// Render all meshes.
    void render(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& viewPos);

    /// Set the model matrix for a specific mesh.
    void setModelMatrix(int meshIndex, const glm::mat4& model);

    /// Set the color for a specific mesh.
    void setColor(int meshIndex, glm::vec3 color);

    /// Set the selection state for a specific mesh.
    void setSelected(int meshIndex, bool selected);

    /// Mark a mesh as a boolean-subtract preview: it is tinted and outlined red
    /// to show the volume that will be removed when the operation is committed.
    void setSubtractPreview(int meshIndex, bool subtractPreview);

    /// Update the scene lighting parameters (applied on the next render).
    void setLighting(const LightingParams& params) { m_lighting = params; }

    /// Remove all meshes.
    void clear();

    /// Get a color from a cycling palette for the given body index.
    static glm::vec3 bodyColor(int index);

    /// Get the number of stored meshes.
    int getMeshCount() const { return static_cast<int>(m_meshes.size()); }

private:
    struct MeshData {
        unsigned int vao = 0;
        unsigned int vbo = 0;
        int vertexCount = 0;
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        glm::vec3 color = glm::vec3(0.7f, 0.7f, 0.7f);
        bool selected = false;
        bool subtractPreview = false;
    };

    bool compileShader(unsigned int& shader, unsigned int type, const char* source);
    bool linkProgram(unsigned int program, unsigned int vertShader, unsigned int fragShader);
    void renderMeshOutline(const MeshData& mesh, const glm::mat4& view,
                           const glm::mat4& projection, const glm::vec4& color);

    std::vector<MeshData> m_meshes;

    // Mesh shader program
    unsigned int m_meshProgram = 0;
    int m_meshLoc_model = -1;
    int m_meshLoc_view = -1;
    int m_meshLoc_projection = -1;
    int m_meshLoc_viewPos = -1;
    int m_meshLoc_lightDir = -1;
    int m_meshLoc_fillDir = -1;
    int m_meshLoc_objectColor = -1;
    int m_meshLoc_selected = -1;
    int m_meshLoc_ambient = -1;
    int m_meshLoc_headlight = -1;
    int m_meshLoc_fillStrength = -1;
    int m_meshLoc_previewCut = -1;

    // Outline shader program
    unsigned int m_outlineProgram = 0;
    int m_outlineLoc_model = -1;
    int m_outlineLoc_view = -1;
    int m_outlineLoc_projection = -1;
    int m_outlineLoc_outlineColor = -1;
    int m_outlineLoc_outlineWidth = -1;

    // Key directional light from upper-right; fill light from the opposite side
    // and slightly below to lift the shadowed faces.
    glm::vec3 m_lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f));
    glm::vec3 m_fillDir = glm::normalize(glm::vec3(-1.0f, 0.3f, -0.5f));
    LightingParams m_lighting{};
    glm::vec4 m_outlineColor = glm::vec4(0.2f, 0.5f, 1.0f, 1.0f);
    float m_outlineWidth = 0.02f;
};

} // namespace materializr
