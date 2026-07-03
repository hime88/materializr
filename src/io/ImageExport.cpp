#include "gl_common.h"
#include "ImageExport.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace materializr {

// TGA header for uncompressed true-color images
#pragma pack(push, 1)
struct TGAHeader {
    uint8_t  idLength        = 0;
    uint8_t  colorMapType    = 0;
    uint8_t  imageType       = 2;  // uncompressed true-color
    uint8_t  colorMapSpec[5] = {};
    uint16_t xOrigin         = 0;
    uint16_t yOrigin         = 0;
    uint16_t width           = 0;
    uint16_t height          = 0;
    uint8_t  bitsPerPixel    = 32; // BGRA
    uint8_t  imageDesc       = 0x20; // top-left origin
};
#pragma pack(pop)

static ImageExportResult writeTGA(const std::string& filePath,
                                   const std::vector<uint8_t>& pixels,
                                   int width, int height) {
    ImageExportResult result;

    FILE* fp = std::fopen(filePath.c_str(), "wb");
    if (!fp) {
        result.errorMessage = "Failed to open file for writing: " + filePath;
        return result;
    }

    TGAHeader header;
    header.width  = static_cast<uint16_t>(width);
    header.height = static_cast<uint16_t>(height);
    std::fwrite(&header, sizeof(header), 1, fp);

    // Pixel data: convert RGBA to BGRA row by row (top-to-bottom since imageDesc
    // has bit 5 set for top-left origin, but OpenGL reads bottom-to-top so we
    // need to flip rows)
    std::vector<uint8_t> row(width * 4);
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* src = &pixels[y * width * 4];
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2]; // B
            row[x * 4 + 1] = src[x * 4 + 1]; // G
            row[x * 4 + 2] = src[x * 4 + 0]; // R
            row[x * 4 + 3] = src[x * 4 + 3]; // A
        }
        std::fwrite(row.data(), 1, row.size(), fp);
    }

    std::fclose(fp);
    result.success = true;
    return result;
}

// Also write PPM as a simpler fallback (no alpha channel, wider viewer support)
static ImageExportResult writePPM(const std::string& filePath,
                                   const std::vector<uint8_t>& pixels,
                                   int width, int height) {
    ImageExportResult result;

    FILE* fp = std::fopen(filePath.c_str(), "wb");
    if (!fp) {
        result.errorMessage = "Failed to open file for writing: " + filePath;
        return result;
    }

    std::fprintf(fp, "P6\n%d %d\n255\n", width, height);

    // OpenGL pixels are bottom-to-top; PPM expects top-to-bottom
    std::vector<uint8_t> row(width * 3);
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* src = &pixels[y * width * 4];
        for (int x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0]; // R
            row[x * 3 + 1] = src[x * 4 + 1]; // G
            row[x * 3 + 2] = src[x * 4 + 2]; // B
        }
        std::fwrite(row.data(), 1, row.size(), fp);
    }

    std::fclose(fp);
    result.success = true;
    return result;
}

ImageExportResult ImageExport::exportPNG(const std::string& filePath,
                                          unsigned int fboTexture,
                                          int fboWidth, int fboHeight) {
    ImageExportResult result;

    if (fboTexture == 0 || fboWidth <= 0 || fboHeight <= 0) {
        result.errorMessage = "Invalid FBO texture or dimensions.";
        return result;
    }

    // Read pixels from the texture
    std::vector<uint8_t> pixels(fboWidth * fboHeight * 4);
#if defined(MZ_GLES)
    // GL ES has no glGetTexImage. Attach the texture to a temporary read
    // framebuffer and pull the pixels back with glReadPixels instead.
    GLint prevFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevFbo);
    GLuint tmpFbo = 0;
    glGenFramebuffers(1, &tmpFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, tmpFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, fboTexture, 0);
    glReadPixels(0, 0, fboWidth, fboHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glDeleteFramebuffers(1, &tmpFbo);
#else
    glBindTexture(GL_TEXTURE_2D, fboTexture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
#endif

    // Determine format from file extension
    // Despite the method name, we write TGA or PPM since we have no PNG library
    std::string ext;
    auto dotPos = filePath.rfind('.');
    if (dotPos != std::string::npos) {
        ext = filePath.substr(dotPos);
    }

    if (ext == ".ppm") {
        return writePPM(filePath, pixels, fboWidth, fboHeight);
    }

    // Default to TGA
    return writeTGA(filePath, pixels, fboWidth, fboHeight);
}

ImageExportResult ImageExport::exportAtResolution(const std::string& filePath,
                                                   int width, int height,
                                                   std::function<void()> renderCallback) {
    ImageExportResult result;

    if (width <= 0 || height <= 0) {
        result.errorMessage = "Invalid export dimensions.";
        return result;
    }

    // Save current viewport
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    // Save current FBO binding
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    // Create temporary FBO with color and depth attachments
    unsigned int fbo = 0, colorTex = 0, depthRbo = 0;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // Color attachment (RGBA texture)
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    // Depth/stencil renderbuffer
    glGenRenderbuffers(1, &depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_RENDERBUFFER, depthRbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        // Clean up
        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &colorTex);
        glDeleteRenderbuffers(1, &depthRbo);

        result.errorMessage = "Failed to create offscreen framebuffer.";
        return result;
    }

    // Set viewport and render
    glViewport(0, 0, width, height);
    glClearColor(0.15f, 0.15f, 0.18f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    renderCallback();

    // Read pixels
    std::vector<uint8_t> pixels(width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // Restore previous state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    // Clean up temporary FBO
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &colorTex);
    glDeleteRenderbuffers(1, &depthRbo);

    // Determine format from file extension
    std::string ext;
    auto dotPos = filePath.rfind('.');
    if (dotPos != std::string::npos) {
        ext = filePath.substr(dotPos);
    }

    if (ext == ".ppm") {
        return writePPM(filePath, pixels, width, height);
    }

    // Default to TGA
    return writeTGA(filePath, pixels, width, height);
}

} // namespace materializr
