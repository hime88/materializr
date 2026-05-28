#pragma once

#ifdef _WIN32
// Windows: opengl32.dll only exports GL 1.1, so load the GL 3.3 core entry
// points with GLEW (provided by vcpkg). glewInit() runs once after the context
// is current — see Window.cpp. GLEW must precede any other GL header.
#include <GL/glew.h>
#else
// On Linux with Mesa, GL_GLEXT_PROTOTYPES gives direct access to GL 3.3+ functions.
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif
