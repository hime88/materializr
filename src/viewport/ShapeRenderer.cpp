#include "gl_common.h"

#include "ShapeRenderer.h"

#include <glm/gtc/type_ptr.hpp>

#include <TopoDS_Shape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>

#include <cstdio>

namespace materializr {

// Embedded mesh shader sources
static const char* s_meshVertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;

out vec3 v_worldPos;
out vec3 v_worldNormal;

void main() {
    vec4 worldPos = u_model * vec4(a_position, 1.0);
    v_worldPos = worldPos.xyz;
    mat3 normalMatrix = transpose(inverse(mat3(u_model)));
    v_worldNormal = normalize(normalMatrix * a_normal);
    gl_Position = u_projection * u_view * worldPos;
}
)";

static const char* s_meshFragSource = R"(
#version 330 core
in vec3 v_worldPos;
in vec3 v_worldNormal;

uniform vec3 u_viewPos;
uniform vec3 u_lightDir;       // key light, direction TO the light (normalized)
uniform vec3 u_fillDir;        // fill light, direction TO the light (normalized)
uniform vec3 u_objectColor;
uniform bool u_selected;
uniform float u_ambient;       // base illumination 0..1 (softens shadows)
uniform bool u_headlight;      // key light tracks the camera when true
uniform float u_fillStrength;  // fill light contribution (0 disables it)
uniform bool u_previewCut;     // tint red: this volume will be subtracted
uniform bool u_sectionEnabled; // section view: clip away one side of a plane
uniform vec3 u_sectionPoint;   // a point on the section plane (world)
uniform vec3 u_sectionNormal;  // plane normal; the normal side is removed

out vec4 fragColor;

void main() {
    if (u_sectionEnabled &&
        dot(v_worldPos - u_sectionPoint, u_sectionNormal) > 0.0) discard;
    vec3 normal = normalize(v_worldNormal);
    // A section cut exposes interior back faces; flip their normals so the
    // inside is lit instead of rendering black.
    if (!gl_FrontFacing) normal = -normal;
    vec3 viewDir = normalize(u_viewPos - v_worldPos);
    // Headlight: the key light comes from the camera, so the face the user is
    // looking at is always lit and large cast shadows disappear.
    vec3 keyDir = u_headlight ? viewDir : normalize(u_lightDir);

    vec3 ambient = u_ambient * u_objectColor;

    float keyDiff = max(dot(normal, keyDir), 0.0);
    float fillDiff = max(dot(normal, normalize(u_fillDir)), 0.0) * u_fillStrength;
    vec3 diffuse = (keyDiff + fillDiff) * u_objectColor;

    float specularStrength = 0.5;
    float shininess = 32.0;
    vec3 halfwayDir = normalize(keyDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), shininess);
    vec3 specular = specularStrength * spec * vec3(1.0);

    vec3 result = ambient + diffuse + specular;

    if (u_previewCut) {
        result = mix(result, vec3(0.9, 0.1, 0.1), 0.55);
    }

    if (u_selected) {
        result = mix(result, vec3(0.3, 0.5, 1.0), 0.3);
    }

    fragColor = vec4(result, 1.0);
}
)";

// Embedded outline shader sources
static const char* s_outlineVertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
uniform float u_outlineWidth;

void main() {
    vec3 expandedPos = a_position + normalize(a_normal) * u_outlineWidth;
    gl_Position = u_projection * u_view * u_model * vec4(expandedPos, 1.0);
}
)";

static const char* s_outlineFragSource = R"(
#version 330 core
uniform vec4 u_outlineColor;
out vec4 fragColor;

void main() {
    fragColor = u_outlineColor;
}
)";

ShapeRenderer::ShapeRenderer() {}

ShapeRenderer::~ShapeRenderer()
{
    clear();
    if (m_meshProgram) {
        glDeleteProgram(m_meshProgram);
        m_meshProgram = 0;
    }
    if (m_outlineProgram) {
        glDeleteProgram(m_outlineProgram);
        m_outlineProgram = 0;
    }
}

bool ShapeRenderer::initialize()
{
    // Compile mesh shader
    unsigned int meshVert = 0, meshFrag = 0;
    if (!compileShader(meshVert, GL_VERTEX_SHADER, s_meshVertSource)) return false;
    if (!compileShader(meshFrag, GL_FRAGMENT_SHADER, s_meshFragSource)) {
        glDeleteShader(meshVert);
        return false;
    }
    m_meshProgram = glCreateProgram();
    if (!linkProgram(m_meshProgram, meshVert, meshFrag)) {
        glDeleteShader(meshVert);
        glDeleteShader(meshFrag);
        return false;
    }
    glDeleteShader(meshVert);
    glDeleteShader(meshFrag);

    // Cache mesh uniform locations
    m_meshLoc_model = glGetUniformLocation(m_meshProgram, "u_model");
    m_meshLoc_view = glGetUniformLocation(m_meshProgram, "u_view");
    m_meshLoc_projection = glGetUniformLocation(m_meshProgram, "u_projection");
    m_meshLoc_viewPos = glGetUniformLocation(m_meshProgram, "u_viewPos");
    m_meshLoc_lightDir = glGetUniformLocation(m_meshProgram, "u_lightDir");
    m_meshLoc_fillDir = glGetUniformLocation(m_meshProgram, "u_fillDir");
    m_meshLoc_objectColor = glGetUniformLocation(m_meshProgram, "u_objectColor");
    m_meshLoc_selected = glGetUniformLocation(m_meshProgram, "u_selected");
    m_meshLoc_ambient = glGetUniformLocation(m_meshProgram, "u_ambient");
    m_meshLoc_headlight = glGetUniformLocation(m_meshProgram, "u_headlight");
    m_meshLoc_fillStrength = glGetUniformLocation(m_meshProgram, "u_fillStrength");
    m_meshLoc_previewCut = glGetUniformLocation(m_meshProgram, "u_previewCut");
    m_meshLoc_sectionEnabled = glGetUniformLocation(m_meshProgram, "u_sectionEnabled");
    m_meshLoc_sectionPoint = glGetUniformLocation(m_meshProgram, "u_sectionPoint");
    m_meshLoc_sectionNormal = glGetUniformLocation(m_meshProgram, "u_sectionNormal");

    // Compile outline shader
    unsigned int outlineVert = 0, outlineFrag = 0;
    if (!compileShader(outlineVert, GL_VERTEX_SHADER, s_outlineVertSource)) return false;
    if (!compileShader(outlineFrag, GL_FRAGMENT_SHADER, s_outlineFragSource)) {
        glDeleteShader(outlineVert);
        return false;
    }
    m_outlineProgram = glCreateProgram();
    if (!linkProgram(m_outlineProgram, outlineVert, outlineFrag)) {
        glDeleteShader(outlineVert);
        glDeleteShader(outlineFrag);
        return false;
    }
    glDeleteShader(outlineVert);
    glDeleteShader(outlineFrag);

    // Cache outline uniform locations
    m_outlineLoc_model = glGetUniformLocation(m_outlineProgram, "u_model");
    m_outlineLoc_view = glGetUniformLocation(m_outlineProgram, "u_view");
    m_outlineLoc_projection = glGetUniformLocation(m_outlineProgram, "u_projection");
    m_outlineLoc_outlineColor = glGetUniformLocation(m_outlineProgram, "u_outlineColor");
    m_outlineLoc_outlineWidth = glGetUniformLocation(m_outlineProgram, "u_outlineWidth");

    return true;
}

int ShapeRenderer::tessellate(const TopoDS_Shape& shape, float deflection,
                              float angularDeflection)
{
    // A worker thread may have PRE-MESHED this shape at the current quality
    // (heavy results like a swept thread — meshing its 35-turn helicoid faces
    // on the main thread froze the app for ~10s). Reuse that cache only when
    // every face carries a triangulation at EXACTLY the requested linear
    // deflection — any other value re-meshes below, so the quality slider
    // still takes effect in BOTH directions (a finer-than-requested cache
    // must NOT survive a quality lowering; that was a real bug once).
    bool preMeshed = true;
    for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More() && preMeshed;
         fx.Next()) {
        TopLoc_Location l;
        Handle(Poly_Triangulation) t =
            BRep_Tool::Triangulation(TopoDS::Face(fx.Current()), l);
        if (t.IsNull() || std::abs(t->Deflection() - deflection) > 1e-4)
            preMeshed = false;
    }
    if (!preMeshed) {
        // Drop any cached triangulation first. BRepMesh_IncrementalMesh only
        // ever refines an existing mesh, so without this a previously finer
        // tessellation would be kept when the user *lowers* the quality.
        // Cleaning forces the mesh to be rebuilt at exactly the requested
        // deflection in either direction.
        BRepTools::Clean(shape);

        // Perform tessellation. The angular deflection subdivides curved
        // surfaces by normal angle, so rounded edges/holes get more facets
        // (smoother) while flat faces stay cheap. Run in parallel to absorb
        // the extra triangles.
        BRepMesh_IncrementalMesh meshGen(shape, deflection, Standard_False,
                                         angularDeflection, Standard_True);
        meshGen.Perform();
    }

    // Collect all triangle vertices (position + normal)
    std::vector<float> vertices;

    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face& face = TopoDS::Face(explorer.Current());
        TopLoc_Location location;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);

        if (triangulation.IsNull()) continue;

        const gp_Trsf& trsf = location.Transformation();
        bool hasTransform = !location.IsIdentity();

        int nbTriangles = triangulation->NbTriangles();
        for (int i = 1; i <= nbTriangles; ++i) {
            const Poly_Triangle& tri = triangulation->Triangle(i);

            int n1, n2, n3;
            tri.Get(n1, n2, n3);

            // Handle face orientation
            if (face.Orientation() == TopAbs_REVERSED) {
                std::swap(n1, n2);
            }

            // Get vertices
            gp_Pnt p1 = triangulation->Node(n1);
            gp_Pnt p2 = triangulation->Node(n2);
            gp_Pnt p3 = triangulation->Node(n3);

            // Apply location transformation if present
            if (hasTransform) {
                p1.Transform(trsf);
                p2.Transform(trsf);
                p3.Transform(trsf);
            }

            // Compute face normal from triangle vertices
            gp_Vec v1(p1, p2);
            gp_Vec v2(p1, p3);
            gp_Vec normal = v1.Crossed(v2);
            if (normal.Magnitude() > 1e-10) {
                normal.Normalize();
            } else {
                normal = gp_Vec(0, 1, 0);
            }

            // If triangulation has per-vertex normals, use them
            bool hasNormals = triangulation->HasNormals();

            auto addVertex = [&](const gp_Pnt& p, int nodeIdx) {
                vertices.push_back(static_cast<float>(p.X()));
                vertices.push_back(static_cast<float>(p.Y()));
                vertices.push_back(static_cast<float>(p.Z()));

                if (hasNormals) {
                    gp_Dir n = triangulation->Normal(nodeIdx);
                    if (hasTransform) {
                        n.Transform(trsf);
                    }
                    if (face.Orientation() == TopAbs_REVERSED) {
                        vertices.push_back(static_cast<float>(-n.X()));
                        vertices.push_back(static_cast<float>(-n.Y()));
                        vertices.push_back(static_cast<float>(-n.Z()));
                    } else {
                        vertices.push_back(static_cast<float>(n.X()));
                        vertices.push_back(static_cast<float>(n.Y()));
                        vertices.push_back(static_cast<float>(n.Z()));
                    }
                } else {
                    vertices.push_back(static_cast<float>(normal.X()));
                    vertices.push_back(static_cast<float>(normal.Y()));
                    vertices.push_back(static_cast<float>(normal.Z()));
                }
            };

            addVertex(p1, n1);
            addVertex(p2, n2);
            addVertex(p3, n3);
        }
    }

    if (vertices.empty()) {
        return -1;
    }

    // Upload to GPU
    MeshData mesh;
    mesh.vertexCount = static_cast<int>(vertices.size() / 6); // 6 floats per vertex

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);

    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 vertices.size() * sizeof(float),
                 vertices.data(),
                 GL_STATIC_DRAW);

    // Position attribute (location = 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Normal attribute (location = 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    int index = static_cast<int>(m_meshes.size());
    m_meshes.push_back(mesh);
    return index;
}

int ShapeRenderer::setBodyMesh(int bodyId, const TopoDS_Shape& shape,
                               float deflection, float angularDeflection)
{
    // Tessellate first so we don't free the old slot's GL resources unless
    // the new tessellation actually succeeds.
    int appendedSlot = tessellate(shape, deflection, angularDeflection);
    if (appendedSlot < 0) {
        // Failed — leave the existing slot (if any) in place. LOUDLY: a
        // kept stale slot means the screen shows geometry the document no
        // longer has (phantom bodies, "unclickable" faces).
        auto it = m_bodyToSlot.find(bodyId);
        std::fprintf(stderr,
                     "[Mesh] tessellation FAILED for body %d — %s\n", bodyId,
                     (it == m_bodyToSlot.end())
                         ? "no previous mesh, body will not render"
                         : "KEEPING STALE MESH (render != document!)");
        return (it == m_bodyToSlot.end()) ? -1 : it->second;
    }
    auto it = m_bodyToSlot.find(bodyId);
    if (it == m_bodyToSlot.end()) {
        // New body; the appended slot is its home.
        m_meshes[appendedSlot].bodyId = bodyId;
        m_bodyToSlot[bodyId] = appendedSlot;
        return appendedSlot;
    }
    // Existing slot — relocate the new mesh's GL data into it so callers
    // that cached the slot index keep working (setColor / setSelected use
    // index lookups). Carry over cosmetic state.
    int oldSlot = it->second;
    if (oldSlot < 0 || oldSlot >= static_cast<int>(m_meshes.size())) {
        // Stale mapping — treat as fresh insert.
        m_meshes[appendedSlot].bodyId = bodyId;
        m_bodyToSlot[bodyId] = appendedSlot;
        return appendedSlot;
    }
    MeshData& old = m_meshes[oldSlot];
    if (old.vao) glDeleteVertexArrays(1, &old.vao);
    if (old.vbo) glDeleteBuffers(1, &old.vbo);
    glm::vec3 keepColor   = old.color;
    bool keepSelected     = old.selected;
    bool keepSubPreview   = old.subtractPreview;
    glm::mat4 keepModel   = old.modelMatrix;
    old = m_meshes[appendedSlot];
    old.bodyId          = bodyId;
    old.color           = keepColor;
    old.selected        = keepSelected;
    old.subtractPreview = keepSubPreview;
    old.modelMatrix     = keepModel;
    // The appended slot was always the tail (tessellate's push_back); pop it
    // so render() doesn't visit a stale duplicate.
    m_meshes.pop_back();
    return oldSlot;
}

void ShapeRenderer::removeBody(int bodyId)
{
    auto it = m_bodyToSlot.find(bodyId);
    if (it == m_bodyToSlot.end()) return;
    int slot = it->second;
    m_bodyToSlot.erase(it);
    if (slot < 0 || slot >= static_cast<int>(m_meshes.size())) return;
    MeshData& m = m_meshes[slot];
    if (m.vao) glDeleteVertexArrays(1, &m.vao);
    if (m.vbo) glDeleteBuffers(1, &m.vbo);
    m.vao = 0; m.vbo = 0; m.vertexCount = 0; m.bodyId = -1;
    // Slot stays in the vector so other slots' indices don't shift.
    // render() skips slots with vertexCount==0.
}

int ShapeRenderer::findSlotByBody(int bodyId) const
{
    auto it = m_bodyToSlot.find(bodyId);
    return (it == m_bodyToSlot.end()) ? -1 : it->second;
}

void ShapeRenderer::render(const glm::mat4& view, const glm::mat4& projection,
                           const glm::vec3& viewPos)
{
    if (!m_meshProgram || m_meshes.empty()) return;

    glEnable(GL_DEPTH_TEST);

    // Section-view clip uniforms. Set before BOTH passes — the outline pass
    // below also runs the mesh program for its stencil fill, and it must
    // clip identically or the selection glow paints over the removed half.
    glUseProgram(m_meshProgram);
    glUniform1i(m_meshLoc_sectionEnabled, m_sectionEnabled ? 1 : 0);
    glUniform3fv(m_meshLoc_sectionPoint, 1, glm::value_ptr(m_sectionPoint));
    glUniform3fv(m_meshLoc_sectionNormal, 1, glm::value_ptr(m_sectionNormal));

    // First pass: render all selected meshes with stencil write
    // Then render the outline for selected meshes
    // Finally render all meshes normally

    // --- Outline pass (stencil technique) ---
    // Selected meshes get the blue selection outline; subtract previews get a
    // red one so it reads as "this volume will be removed".
    static const glm::vec4 kSubtractOutline(0.95f, 0.12f, 0.12f, 1.0f);
    for (const auto& mesh : m_meshes) {
        if (mesh.vertexCount == 0) continue; // vacant slot (removeBody)
        if (mesh.selected) renderMeshOutline(mesh, view, projection, m_outlineColor);
        else if (mesh.subtractPreview) renderMeshOutline(mesh, view, projection, kSubtractOutline);
    }

    // --- Main render pass ---
    glUseProgram(m_meshProgram);
    glUniformMatrix4fv(m_meshLoc_view, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(m_meshLoc_projection, 1, GL_FALSE, glm::value_ptr(projection));
    glUniform3fv(m_meshLoc_viewPos, 1, glm::value_ptr(viewPos));
    glUniform3fv(m_meshLoc_lightDir, 1, glm::value_ptr(m_lightDir));
    glUniform3fv(m_meshLoc_fillDir, 1, glm::value_ptr(m_fillDir));
    glUniform1f(m_meshLoc_ambient, m_lighting.ambient);
    glUniform1i(m_meshLoc_headlight, m_lighting.headlight ? 1 : 0);
    glUniform1f(m_meshLoc_fillStrength, m_lighting.fill ? m_lighting.fillStrength : 0.0f);

    for (const auto& mesh : m_meshes) {
        if (mesh.vertexCount == 0) continue; // vacant slot
        glUniformMatrix4fv(m_meshLoc_model, 1, GL_FALSE, glm::value_ptr(mesh.modelMatrix));
        glUniform3fv(m_meshLoc_objectColor, 1, glm::value_ptr(mesh.color));
        glUniform1i(m_meshLoc_selected, mesh.selected ? 1 : 0);
        glUniform1i(m_meshLoc_previewCut, mesh.subtractPreview ? 1 : 0);

        glBindVertexArray(mesh.vao);
        glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void ShapeRenderer::renderMeshOutline(const MeshData& mesh, const glm::mat4& view,
                                      const glm::mat4& projection, const glm::vec4& color)
{
    // Stencil technique:
    // 1. Draw the mesh into stencil buffer (marking pixels with 1)
    // 2. Draw the expanded outline where stencil != 1

    glEnable(GL_STENCIL_TEST);

    // Step 1: Fill stencil with 1 where the mesh is drawn
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilMask(0xFF);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);

    glUseProgram(m_meshProgram);
    glUniformMatrix4fv(m_meshLoc_model, 1, GL_FALSE, glm::value_ptr(mesh.modelMatrix));
    glUniformMatrix4fv(m_meshLoc_view, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(m_meshLoc_projection, 1, GL_FALSE, glm::value_ptr(projection));

    glBindVertexArray(mesh.vao);
    glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);

    // Step 2: Draw outline where stencil != 1
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilMask(0x00);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);

    glUseProgram(m_outlineProgram);
    glUniformMatrix4fv(m_outlineLoc_model, 1, GL_FALSE, glm::value_ptr(mesh.modelMatrix));
    glUniformMatrix4fv(m_outlineLoc_view, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(m_outlineLoc_projection, 1, GL_FALSE, glm::value_ptr(projection));
    glUniform4fv(m_outlineLoc_outlineColor, 1, glm::value_ptr(color));
    glUniform1f(m_outlineLoc_outlineWidth, m_outlineWidth);

    glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
    glBindVertexArray(0);

    // Reset stencil state
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glDisable(GL_STENCIL_TEST);
    glUseProgram(0);
}

void ShapeRenderer::setModelMatrix(int meshIndex, const glm::mat4& model)
{
    if (meshIndex >= 0 && meshIndex < static_cast<int>(m_meshes.size())) {
        m_meshes[meshIndex].modelMatrix = model;
    }
}

void ShapeRenderer::setColor(int meshIndex, glm::vec3 color)
{
    if (meshIndex >= 0 && meshIndex < static_cast<int>(m_meshes.size())) {
        m_meshes[meshIndex].color = color;
    }
}

void ShapeRenderer::setSelected(int meshIndex, bool selected)
{
    if (meshIndex >= 0 && meshIndex < static_cast<int>(m_meshes.size())) {
        m_meshes[meshIndex].selected = selected;
    }
}

void ShapeRenderer::setSubtractPreview(int meshIndex, bool subtractPreview)
{
    if (meshIndex >= 0 && meshIndex < static_cast<int>(m_meshes.size())) {
        m_meshes[meshIndex].subtractPreview = subtractPreview;
    }
}

void ShapeRenderer::clear()
{
    for (auto& mesh : m_meshes) {
        if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
        if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    }
    m_meshes.clear();
    m_bodyToSlot.clear();
}

void ShapeRenderer::debugDumpSlots() const
{
    std::fprintf(stderr, "  [Slots] %zu mesh slots:\n", m_meshes.size());
    for (size_t i = 0; i < m_meshes.size(); ++i) {
        const auto& m = m_meshes[i];
        std::fprintf(stderr,
                     "    slot %zu: body=%d verts=%d sel=%d subPrev=%d\n",
                     i, m.bodyId, m.vertexCount, m.selected ? 1 : 0,
                     m.subtractPreview ? 1 : 0);
    }
}

glm::vec3 ShapeRenderer::bodyColor(int index)
{
    static const glm::vec3 palette[] = {
        {0.60f, 0.65f, 0.75f},  // steel blue
        {0.75f, 0.55f, 0.40f},  // copper
        {0.50f, 0.70f, 0.50f},  // sage green
        {0.70f, 0.50f, 0.65f},  // mauve
        {0.65f, 0.65f, 0.45f},  // olive
        {0.45f, 0.60f, 0.70f},  // teal
        {0.72f, 0.58f, 0.55f},  // salmon
        {0.55f, 0.55f, 0.70f},  // lavender
    };
    static const int paletteSize = static_cast<int>(sizeof(palette) / sizeof(palette[0]));

    int i = index % paletteSize;
    if (i < 0) i += paletteSize;
    return palette[i];
}

bool ShapeRenderer::compileShader(unsigned int& shader, unsigned int type, const char* source)
{
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::fprintf(stderr, "ShapeRenderer shader compilation failed: %s\n", infoLog);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

bool ShapeRenderer::linkProgram(unsigned int program, unsigned int vertShader,
                                unsigned int fragShader)
{
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);
    glLinkProgram(program);

    int success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        std::fprintf(stderr, "ShapeRenderer shader linking failed: %s\n", infoLog);
        return false;
    }
    return true;
}

} // namespace materializr
