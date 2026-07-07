#include "GifEncoder.h"

#include <algorithm>
#include <cstring>

namespace materializr {

namespace {

// Little-endian u16, as the GIF spec wants everywhere.
void putU16(FILE* fp, uint16_t v) {
    std::fputc(v & 0xFF, fp);
    std::fputc((v >> 8) & 0xFF, fp);
}

// LZW bit packer: codes are packed LSB-first into bytes, bytes into <=255-byte
// data sub-blocks, each prefixed with its length.
struct BitPacker {
    FILE* fp;
    uint32_t bitBuf = 0;
    int bitCount = 0;
    uint8_t block[255];
    int blockLen = 0;
    bool ok = true;

    explicit BitPacker(FILE* f) : fp(f) {}

    void flushBlock() {
        if (blockLen == 0) return;
        if (std::fputc(blockLen, fp) == EOF) ok = false;
        if (std::fwrite(block, 1, blockLen, fp) != size_t(blockLen)) ok = false;
        blockLen = 0;
    }
    void putCode(uint32_t code, int bits) {
        bitBuf |= code << bitCount;
        bitCount += bits;
        while (bitCount >= 8) {
            block[blockLen++] = uint8_t(bitBuf & 0xFF);
            if (blockLen == 255) flushBlock();
            bitBuf >>= 8;
            bitCount -= 8;
        }
    }
    void finish() {
        if (bitCount > 0) {
            block[blockLen++] = uint8_t(bitBuf & 0xFF);
            if (blockLen == 255) flushBlock();
        }
        flushBlock();
        if (std::fputc(0x00, fp) == EOF) ok = false; // block terminator
    }
};

// Palette layout: [0..215] the 6x6x6 colour cube, [216..255] a 40-step grey
// ramp. Nearest-index lookup considers both and picks the closer, so smooth
// shaded greys (most of a CAD render) don't band to the coarse cube.
inline int cubeLevel(int c) { return (c * 5 + 127) / 255; } // 0..5
inline int cubeValue(int level) { return level * 51; }      // 0,51,...,255

inline int nearestIndex(int r, int g, int b, int& pr, int& pg, int& pb) {
    const int cr = cubeLevel(r), cg = cubeLevel(g), cb = cubeLevel(b);
    const int qr = cubeValue(cr), qg = cubeValue(cg), qb = cubeValue(cb);
    long cubeErr = long(r - qr) * (r - qr) + long(g - qg) * (g - qg) +
                   long(b - qb) * (b - qb);

    // Grey ramp: v = i*255/39 for i in 0..39
    const int grey = (r * 2 + g * 5 + b) / 8; // cheap luma
    int gi = (grey * 39 + 127) / 255;
    gi = std::clamp(gi, 0, 39);
    const int gv = gi * 255 / 39;
    long greyErr = long(r - gv) * (r - gv) + long(g - gv) * (g - gv) +
                   long(b - gv) * (b - gv);

    if (greyErr < cubeErr) {
        pr = pg = pb = gv;
        return 216 + gi;
    }
    pr = qr; pg = qg; pb = qb;
    return cr * 36 + cg * 6 + cb;
}

} // namespace

GifEncoder::~GifEncoder() {
    if (m_fp) { std::fclose(m_fp); m_fp = nullptr; }
}

bool GifEncoder::begin(const std::string& path, int width, int height, int delayCs) {
    if (m_fp || width <= 0 || height <= 0 || width > 0xFFFF || height > 0xFFFF)
        return false;
    m_fp = std::fopen(path.c_str(), "wb");
    if (!m_fp) return false;
    m_w = width;
    m_h = height;
    m_delayCs = std::max(2, delayCs); // <2cs is ignored by most viewers

    // Build the palette once.
    for (int r = 0; r < 6; ++r)
        for (int g = 0; g < 6; ++g)
            for (int b = 0; b < 6; ++b) {
                uint8_t* p = m_palette[r * 36 + g * 6 + b];
                p[0] = uint8_t(cubeValue(r));
                p[1] = uint8_t(cubeValue(g));
                p[2] = uint8_t(cubeValue(b));
            }
    for (int i = 0; i < 40; ++i) {
        uint8_t v = uint8_t(i * 255 / 39);
        m_palette[216 + i][0] = m_palette[216 + i][1] = m_palette[216 + i][2] = v;
    }

    std::fwrite("GIF89a", 1, 6, m_fp);
    putU16(m_fp, uint16_t(m_w));
    putU16(m_fp, uint16_t(m_h));
    std::fputc(0xF7, m_fp); // global table, 8-bit colour res, 256 entries
    std::fputc(0x00, m_fp); // background colour index
    std::fputc(0x00, m_fp); // pixel aspect ratio
    writePalette();

    // NETSCAPE2.0 application extension: loop forever.
    std::fputc(0x21, m_fp); std::fputc(0xFF, m_fp); std::fputc(0x0B, m_fp);
    std::fwrite("NETSCAPE2.0", 1, 11, m_fp);
    std::fputc(0x03, m_fp); std::fputc(0x01, m_fp);
    putU16(m_fp, 0);        // 0 = loop forever
    std::fputc(0x00, m_fp);

    return !std::ferror(m_fp);
}

void GifEncoder::writePalette() {
    std::fwrite(m_palette, 1, 256 * 3, m_fp);
}

void GifEncoder::writeFrameHeader(int delayCs) {
    // Graphic Control Extension: disposal 1 (leave in place), no transparency.
    std::fputc(0x21, m_fp); std::fputc(0xF9, m_fp); std::fputc(0x04, m_fp);
    std::fputc(0x04, m_fp);
    putU16(m_fp, uint16_t(std::max(2, delayCs)));
    std::fputc(0x00, m_fp); // transparent index (unused)
    std::fputc(0x00, m_fp);
    // Image descriptor: full frame, global palette, no interlace.
    std::fputc(0x2C, m_fp);
    putU16(m_fp, 0); putU16(m_fp, 0);
    putU16(m_fp, uint16_t(m_w)); putU16(m_fp, uint16_t(m_h));
    std::fputc(0x00, m_fp);
}

void GifEncoder::quantize(const uint8_t* rgba, std::vector<uint8_t>& indices) const {
    // Floyd–Steinberg on a per-row error buffer (serpentine scan). Errors are
    // carried in int16 to allow negatives; clamped on read.
    const int w = m_w, h = m_h;
    indices.resize(size_t(w) * h);
    std::vector<int16_t> err(size_t(w + 2) * 3, 0), errNext(size_t(w + 2) * 3, 0);

    for (int y = 0; y < h; ++y) {
        std::fill(errNext.begin(), errNext.end(), int16_t(0));
        const bool ltr = (y % 2) == 0;
        for (int i = 0; i < w; ++i) {
            const int x = ltr ? i : (w - 1 - i);
            const uint8_t* px = rgba + (size_t(y) * w + x) * 4;
            const int ei = (x + 1) * 3;
            int r = std::clamp(px[0] + err[ei + 0] / 16, 0, 255);
            int g = std::clamp(px[1] + err[ei + 1] / 16, 0, 255);
            int b = std::clamp(px[2] + err[ei + 2] / 16, 0, 255);
            int pr, pg, pb;
            const int idx = nearestIndex(r, g, b, pr, pg, pb);
            indices[size_t(y) * w + x] = uint8_t(idx);
            const int dr = r - pr, dg = g - pg, db = b - pb;
            const int fwd = ltr ? 3 : -3, bck = ltr ? -3 : 3;
            // Weights x16: 7 forward; 3, 5, 1 on the next row.
            err[ei + fwd + 0] += int16_t(dr * 7); // NOLINT
            err[ei + fwd + 1] += int16_t(dg * 7);
            err[ei + fwd + 2] += int16_t(db * 7);
            errNext[ei + bck + 0] += int16_t(dr * 3);
            errNext[ei + bck + 1] += int16_t(dg * 3);
            errNext[ei + bck + 2] += int16_t(db * 3);
            errNext[ei + 0] += int16_t(dr * 5);
            errNext[ei + 1] += int16_t(dg * 5);
            errNext[ei + 2] += int16_t(db * 5);
            errNext[ei + fwd + 0] += int16_t(dr * 1);
            errNext[ei + fwd + 1] += int16_t(dg * 1);
            errNext[ei + fwd + 2] += int16_t(db * 1);
        }
        std::swap(err, errNext);
    }
}

bool GifEncoder::writeLzw(const std::vector<uint8_t>& indices) {
    // Standard GIF LZW: 8-bit min code size, clear/EOI codes, codes grow from
    // 9 to 12 bits, emit CLEAR and reset when the table is full.
    constexpr int kMinCodeSize = 8;
    constexpr int kClear = 1 << kMinCodeSize;   // 256
    constexpr int kEoi = kClear + 1;            // 257
    constexpr int kMaxCode = 4096;

    std::fputc(kMinCodeSize, m_fp);
    BitPacker bp(m_fp);

    // child[prefixCode][nextIndex] -> code, 0 = absent.
    std::vector<uint16_t> child(size_t(kMaxCode) * 256, 0);
    int nextCode = kEoi + 1;
    int codeBits = kMinCodeSize + 1;

    bp.putCode(kClear, codeBits);
    int prefix = indices.empty() ? 0 : indices[0];
    for (size_t i = 1; i < indices.size(); ++i) {
        const int k = indices[i];
        const size_t slot = size_t(prefix) * 256 + k;
        if (child[slot] != 0) {
            prefix = child[slot];
            continue;
        }
        bp.putCode(uint32_t(prefix), codeBits);
        if (nextCode < kMaxCode) {
            child[slot] = uint16_t(nextCode);
            if (nextCode == (1 << codeBits) && codeBits < 12) ++codeBits;
            ++nextCode;
        } else {
            bp.putCode(kClear, codeBits);
            std::fill(child.begin(), child.end(), uint16_t(0));
            nextCode = kEoi + 1;
            codeBits = kMinCodeSize + 1;
        }
        prefix = k;
    }
    bp.putCode(uint32_t(prefix), codeBits);
    bp.putCode(kEoi, codeBits);
    bp.finish();
    return bp.ok && !std::ferror(m_fp);
}

bool GifEncoder::addFrame(const uint8_t* rgba, int delayCs) {
    if (!m_fp || !rgba) return false;
    writeFrameHeader(delayCs < 0 ? m_delayCs : delayCs);
    std::vector<uint8_t> indices;
    quantize(rgba, indices);
    return writeLzw(indices);
}

bool GifEncoder::end() {
    if (!m_fp) return false;
    std::fputc(0x3B, m_fp); // trailer
    const bool ok = !std::ferror(m_fp);
    std::fclose(m_fp);
    m_fp = nullptr;
    return ok;
}

} // namespace materializr
