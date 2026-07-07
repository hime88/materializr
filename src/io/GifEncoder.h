#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace materializr {

// Minimal first-party animated-GIF writer (GIF89a), written for the timelapse
// export. One global 256-colour palette (6x6x6 web cube + 40-step grey ramp),
// Floyd–Steinberg dithering, standard LZW with dynamic code sizes, NETSCAPE
// looping extension. No third-party code.
//
//   GifEncoder gif;
//   gif.begin("out.gif", w, h, /*delayCs=*/10);   // delay in 1/100 s per frame
//   gif.addFrame(rgba);                            // w*h*4 bytes, top-left origin
//   ...
//   gif.end();
//
// All frames must match the begin() dimensions. Returns false on I/O failure.
class GifEncoder {
public:
    ~GifEncoder(); // closes (without trailer) if end() was never called

    bool begin(const std::string& path, int width, int height, int delayCs);
    // delayCs < 0 uses the begin() default; otherwise per-frame override.
    bool addFrame(const uint8_t* rgba, int delayCs = -1);
    bool end();

    bool active() const { return m_fp != nullptr; }

private:
    void writePalette();
    void writeFrameHeader(int delayCs);
    void quantize(const uint8_t* rgba, std::vector<uint8_t>& indices) const;
    bool writeLzw(const std::vector<uint8_t>& indices);

    FILE* m_fp = nullptr;
    int m_w = 0, m_h = 0, m_delayCs = 10;
    uint8_t m_palette[256][3] = {};
};

} // namespace materializr
