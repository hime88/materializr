#pragma once
#include <cstdint>
#include <string>

namespace materializr {

// MP4 (H.264) writer with a per-platform backend behind one interface:
//   - POSIX desktop (Linux/macOS): raw RGBA piped to an `ffmpeg` binary found
//     on PATH, spawned with an argv array via fork/exec — no shell is ever
//     involved (same posture as url_open.h). VideoEncoder.cpp.
//   - iOS: AVAssetWriter driving the hardware H.264 encoder — the same way
//     the big drawing apps export their timelapses. ios_videowriter.mm.
//   - Windows / Android: stubs; available() reports false so callers simply
//     don't offer MP4 there (Android's AMediaCodec backend is future work).
class VideoEncoder {
public:
    ~VideoEncoder(); // aborts cleanly if end() wasn't called

    static bool available();

    bool begin(const std::string& path, int width, int height, int fps);
    bool addFrame(const uint8_t* rgba); // width*height*4, top-left origin
    bool end();                          // finalizes; true when the encoder succeeded

private:
    int m_pipe = -1;        // ffmpeg backend
    long m_pid = -1;        // ffmpeg backend
    size_t m_frameBytes = 0;
    void* m_impl = nullptr; // iOS backend (owned AVAssetWriter state)
};

} // namespace materializr
