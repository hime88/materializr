#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace materializr {

// MP4 (H.264) writer with a per-platform backend behind one interface:
//   - POSIX desktop (Linux/macOS): raw RGBA piped to an `ffmpeg` binary found
//     on PATH, spawned with an argv array via fork/exec — no shell is ever
//     involved (same posture as url_open.h). VideoEncoder.cpp.
//   - iOS: AVAssetWriter driving the hardware H.264 encoder — the same way
//     the big drawing apps export their timelapses. ios_videowriter.mm.
//   - Android: AMediaCodec + AMediaMuxer (NDK C API, hardware encoder).
//     android_videowriter.cpp.
//   - Windows: stubs; available() reports false so callers fall back to the
//     pixel store + GIF path there.
class VideoEncoder {
public:
    ~VideoEncoder(); // aborts cleanly if end() wasn't called

    static bool available();

    // fragmented=true writes recoverable fragmented MP4 (a crash mid-segment
    // loses at most the tail, not the file) — used for the rolling recording
    // segments; exports keep plain faststart MP4.
    bool begin(const std::string& path, int width, int height, int fps,
               bool fragmented = false);
    bool addFrame(const uint8_t* rgba); // width*height*4, top-left origin
    bool end();                          // finalizes; true when the encoder succeeded

    // Concatenate finished segments (same codec/dimensions, in order) into
    // one MP4. condenseSeconds > 0 retimes the whole thing to that length
    // (re-encode); <= 0 stream-copies losslessly. totalFrames = sum of frames
    // across segments (for the retime ratio).
    static bool concatSegments(const std::vector<std::string>& segments,
                               const std::string& outPath, int totalFrames,
                               int condenseSeconds, std::string* err);

private:
    int m_pipe = -1;        // ffmpeg backend
    long m_pid = -1;        // ffmpeg backend
    size_t m_frameBytes = 0;
    void* m_impl = nullptr; // iOS backend (owned AVAssetWriter state)
};

} // namespace materializr
