#pragma once

// Portable GL include. Desktop uses OpenGL 3.3 Core; Android uses OpenGL ES 3.0,
// which provides the same subset Materializr relies on (VAOs, instancing,
// in/out shader stages, transpose/inverse). Shader sources written for GLSL 330
// core are adapted to GLSL ES 3.00 transparently at upload time — see below.
#if defined(__ANDROID__)
// Android / OpenGL ES 3.0 (function prototypes are exported directly by libGLESv3).
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

// Shader-source shim. Every renderer compiles shaders written as
// "#version 330 core"; on GLES those must become "#version 300 es" with default
// precision qualifiers. Rather than touch ~15 renderers, we intercept
// glShaderSource itself: the adapter rewrites each source string before upload.
// Precision qualifiers are valid in both vertex and fragment shaders, so the
// adapter needs only the source text, not the stage. Defined in gl_shader.cpp,
// which #undefs this macro so it can call the real glShaderSource.
namespace materializr {
void glShaderSourceAdapt(GLuint shader, GLsizei count,
                         const GLchar* const* string, const GLint* length);
}
#define glShaderSource(shader, count, string, length) \
        ::materializr::glShaderSourceAdapt((shader), (count), (string), (length))

#elif defined(_WIN32)
// Windows: opengl32.dll only exports GL 1.1, so load the GL 3.3 core entry
// points with GLEW (provided by vcpkg). glewInit() runs once after the context
// is current — see Window.cpp. GLEW must precede any other GL header.
#include <GL/glew.h>
#elif defined(__APPLE__)
// macOS: OpenGL.framework exports the 3.2+ core-profile entry points directly
// via <OpenGL/gl3.h> (up to GL 4.1 — the platform ceiling, which still covers
// every GLSL 330 shader here), so no GLEW-style loader is needed. The context
// must be created with the forward-compatible core profile — see Window.cpp.
// GL was deprecated on macOS 10.14; silence those headers (Apple still ships
// and supports the framework, and there's no Metal/MoltenVK port yet). The
// CMake build also defines this for ImGui's GL backend, so guard against the
// redefinition for translation units that get both.
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl3.h>
#else
// Linux desktop with Mesa: GL_GLEXT_PROTOTYPES gives direct access to GL 3.3+.
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif
