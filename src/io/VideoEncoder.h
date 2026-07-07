#pragma once
#include <cstdint>
#include <string>

namespace materializr {

// MP4 (H.264) writer that pipes raw RGBA frames to an `ffmpeg` binary found
// on PATH — POSIX desktop only (Linux/macOS); Windows and mobile builds
// compile the stubs and available() reports false, so callers simply don't
// offer MP4 there. ffmpeg is spawned with an argv array via fork/exec — no
// shell is ever involved (same posture as url_open.h).
class VideoEncoder {
public:
    ~VideoEncoder(); // aborts (closes pipe, reaps child) if end() wasn't called

    static bool available();

    bool begin(const std::string& path, int width, int height, int fps);
    bool addFrame(const uint8_t* rgba); // width*height*4, top-left origin
    bool end();                          // closes the stream; true if ffmpeg exited 0

private:
    int m_pipe = -1;
    long m_pid = -1;
    size_t m_frameBytes = 0;
};

} // namespace materializr
