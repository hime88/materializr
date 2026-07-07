#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace materializr {

// Always-on timelapse: one viewport frame is stored per
// committed history mutation (Application polls History::revision() and calls
// captureFromTexture once the meshes have settled). Frames live in a
// per-project directory under <config>/timelapse/<key>/ as small
// zlib-compressed RGBA files, so the recording survives restarts and can be
// exported to a GIF any number of times — full length or condensed.
//
// Threading: captureFromTexture/storeFrame/bindProject run on the GL/main
// thread. exportGif is pure CPU + file reads and safe to run on a worker;
// a frame deleted concurrently (cap thinning) is skipped, not fatal.
class TimelapseRecorder {
public:
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }

    // Bind the store to the current document ("" = unsaved). When carryFrames
    // is true and the previous binding has frames but the new one has none,
    // the store is moved (Save As adopts the unsaved session's recording).
    void bindProject(const std::string& projectRef, bool carryFrames);

    // GL thread: read `texture` (w×h RGBA), downscale to the recording
    // resolution, and append a frame to the store. moveFrame marks a camera
    // interpolation filler ('m' filename suffix): exports play those fast and
    // skip crossfades into them, so camera moves glide instead of snapping.
    void captureFromTexture(unsigned texture, int w, int h,
                            bool moveFrame = false);

    // Pure part of capture (also the unit-test entry): downscale + compress +
    // append `rgba` (w×h×4, top-left origin).
    void storeFrame(const uint8_t* rgba, int w, int h, bool moveFrame = false);

    int frameCount() const { return static_cast<int>(m_frames.size()); }
    void clearFrames();

    // Snapshot for a worker-thread encode: take these on the main thread
    // (m_frames is mutated by captures), then call encodeGif off-thread.
    std::vector<std::string> frameSnapshot() const { return m_frames; }
    std::string frameDirPath() const { return frameDir(); }

    // Encode `names` (relative to `dir`) into `gifPath`. condenseSeconds > 0
    // samples frames evenly to fit that many seconds at ~10 fps; <= 0 exports
    // full length (~8 fps). Pure CPU + file reads — safe off-thread; frames
    // deleted concurrently (cap thinning) are skipped. Returns false and
    // fills `err` on failure.
    static bool encodeGif(const std::string& dir,
                          const std::vector<std::string>& names,
                          const std::string& gifPath, int condenseSeconds,
                          std::string* err);

    // Same contract, H.264 MP4 through the ffmpeg pipe (VideoEncoder). Fails
    // with a clear message where ffmpeg isn't available (Windows, mobile).
    static bool encodeMp4(const std::string& dir,
                          const std::vector<std::string>& names,
                          const std::string& mp4Path, int condenseSeconds,
                          std::string* err);

private:
    std::string frameDir() const;
    void appendFrameFile(const std::vector<uint8_t>& rgba, int w, int h,
                         bool moveFrame);
    void thinIfOverCap();

    bool m_enabled = true;
    std::string m_key = "_unsaved";
    std::vector<std::string> m_frames; // ordered file names within frameDir()
    int m_nextSerial = 0;
};

} // namespace materializr
