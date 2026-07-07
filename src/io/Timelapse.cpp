#include "gl_common.h"
#include "Timelapse.h"
#include "GifEncoder.h"
#include "VideoEncoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <zlib.h>

namespace materializr {

namespace {

// Base config directory (mirrors Settings.cpp's resolution). Frames live
// under <config>/timelapse/<project-key>/.
std::string configBaseDir() {
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return std::string(up) + "\\materializr";
    return "materializr";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/materializr";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/materializr";
    return ".materializr";
#endif
}

// Stable directory key for a project reference (path or content:// URI).
std::string projectKey(const std::string& ref) {
    if (ref.empty()) return "_unsaved";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "p%016zx",
                  size_t(std::hash<std::string>{}(ref)));
    return buf;
}

// Recording resolution: frames are downscaled so the long edge is at most
// this (capture renders 1920x1080, stored as-is → 1080p MP4 exports).
constexpr int kMaxLongEdge = 1920;
// Store cap: beyond this the store is thinned by dropping every second frame,
// so a marathon session stays bounded while keeping even coverage. At 1080p a
// zlib frame runs ~0.5-2 MB, so the cap also bounds disk to the low hundreds
// of MB worst case.
constexpr int kMaxStoredFrames = 600;
// GIFs get re-downscaled at encode time: 256 colours + no temporal
// compression makes 1080p GIFs enormous and slow to dither; MP4 keeps the
// full stored resolution.
constexpr int kGifMaxLongEdge = 960;

// Frame file: "MZTL" magic, u16 w, u16 h, u32 rawSize, zlib-deflated RGBA.
constexpr char kMagic[4] = {'M', 'Z', 'T', 'L'};

bool readFrameFile(const std::string& path, std::vector<uint8_t>& rgba,
                   int& w, int& h) {
    std::ifstream is(path, std::ios::binary);
    if (!is.is_open()) return false;
    char magic[4];
    uint16_t w16 = 0, h16 = 0;
    uint32_t rawSize = 0;
    is.read(magic, 4);
    is.read(reinterpret_cast<char*>(&w16), 2);
    is.read(reinterpret_cast<char*>(&h16), 2);
    is.read(reinterpret_cast<char*>(&rawSize), 4);
    if (!is.good() || std::memcmp(magic, kMagic, 4) != 0) return false;
    if (w16 == 0 || h16 == 0 || rawSize != uint32_t(w16) * h16 * 4) return false;
    std::vector<uint8_t> packed((std::istreambuf_iterator<char>(is)),
                                std::istreambuf_iterator<char>());
    rgba.resize(rawSize);
    uLongf outLen = rawSize;
    if (uncompress(rgba.data(), &outLen, packed.data(),
                   uLong(packed.size())) != Z_OK || outLen != rawSize)
        return false;
    w = w16;
    h = h16;
    return true;
}

// Box-average downscale of RGBA to exactly dstW×dstH.
void downscale(const uint8_t* src, int sw, int sh, std::vector<uint8_t>& dst,
               int dw, int dh) {
    dst.resize(size_t(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* p = src + (size_t(sy) * sw + sx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3];
                    ++n;
                }
            uint8_t* q = dst.data() + (size_t(y) * dw + x) * 4;
            q[0] = uint8_t(r / n); q[1] = uint8_t(g / n);
            q[2] = uint8_t(b / n); q[3] = uint8_t(a / n);
        }
    }
}

// Scale-to-fit `src` onto a dstW×dstH canvas, preserving aspect and filling
// the border with the frame's own top-left pixel (frames can differ in aspect
// after a window resize; the GIF canvas is uniform).
void fitOnCanvas(const std::vector<uint8_t>& src, int sw, int sh,
                 std::vector<uint8_t>& dst, int dw, int dh) {
    if (sw == dw && sh == dh) { dst = src; return; }
    const double scale = std::min(double(dw) / sw, double(dh) / sh);
    const int fw = std::max(1, int(sw * scale)), fh = std::max(1, int(sh * scale));
    std::vector<uint8_t> fitted;
    downscale(src.data(), sw, sh, fitted, fw, fh);
    dst.assign(size_t(dw) * dh * 4, 255);
    const uint8_t* bg = src.data();
    for (size_t i = 0; i < size_t(dw) * dh; ++i)
        std::memcpy(dst.data() + i * 4, bg, 4);
    const int ox = (dw - fw) / 2, oy = (dh - fh) / 2;
    for (int y = 0; y < fh; ++y)
        std::memcpy(dst.data() + (size_t(oy + y) * dw + ox) * 4,
                    fitted.data() + size_t(y) * fw * 4, size_t(fw) * 4);
}

// Camera-move filler frames carry an 'm' before the extension; exports play
// them fast and skip the crossfade into them (they are already smooth).
bool isMoveFrame(const std::string& name) {
    return name.size() > 5 && name[name.size() - 5] == 'm';
}

} // namespace

std::string TimelapseRecorder::frameDir() const {
    return configBaseDir() + "/timelapse/" + m_key;
}

void TimelapseRecorder::bindProject(const std::string& projectRef,
                                    bool carryFrames) {
    namespace fs = std::filesystem;
    const std::string newKey = projectKey(projectRef);
    if (newKey == m_key && !m_frames.empty()) return;

    const std::string oldDir = frameDir();
    const bool hadFrames = !m_frames.empty();
    m_key = newKey;
    const std::string newDir = frameDir();

    std::error_code ec;
    if (carryFrames && hadFrames && oldDir != newDir && !fs::exists(newDir, ec)) {
        fs::create_directories(fs::path(newDir).parent_path(), ec);
        fs::rename(oldDir, newDir, ec); // Save As adopts the unsaved recording
    }

    // (Re)load the frame listing for the bound project.
    m_frames.clear();
    m_nextSerial = 0;
    if (fs::exists(newDir, ec)) {
        for (const auto& e : fs::directory_iterator(newDir, ec))
            if (e.is_regular_file() && e.path().extension() == ".mzf")
                m_frames.push_back(e.path().filename().string());
        std::sort(m_frames.begin(), m_frames.end());
        if (!m_frames.empty()) {
            // Serials keep growing past the highest existing one.
            m_nextSerial = std::atoi(m_frames.back().c_str()) + 1;
        }
    }
}

void TimelapseRecorder::captureFromTexture(unsigned texture, int w, int h,
                                           bool moveFrame) {
    if (!m_enabled || texture == 0 || w <= 0 || h <= 0) return;

    std::vector<uint8_t> pixels(size_t(w) * h * 4);
#if defined(MZ_GLES)
    // GL ES has no glGetTexImage: attach to a temporary read FBO instead
    // (same pattern as ImageExport).
    GLint prevFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevFbo);
    GLuint tmpFbo = 0;
    glGenFramebuffers(1, &tmpFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, tmpFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glDeleteFramebuffers(1, &tmpFbo);
#else
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
#endif

    // GL hands rows back bottom-up; frames are stored top-left origin.
    std::vector<uint8_t> row(size_t(w) * 4);
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = pixels.data() + size_t(y) * w * 4;
        uint8_t* b = pixels.data() + size_t(h - 1 - y) * w * 4;
        std::memcpy(row.data(), a, row.size());
        std::memcpy(a, b, row.size());
        std::memcpy(b, row.data(), row.size());
    }

    storeFrame(pixels.data(), w, h, moveFrame);
}

void TimelapseRecorder::storeFrame(const uint8_t* rgba, int w, int h,
                                   bool moveFrame) {
    if (!m_enabled || !rgba || w <= 0 || h <= 0) return;

    // Downscale to the recording resolution.
    const int longEdge = std::max(w, h);
    std::vector<uint8_t> frame;
    int fw = w, fh = h;
    if (longEdge > kMaxLongEdge) {
        fw = std::max(1, w * kMaxLongEdge / longEdge);
        fh = std::max(1, h * kMaxLongEdge / longEdge);
        downscale(rgba, w, h, frame, fw, fh);
    } else {
        frame.assign(rgba, rgba + size_t(w) * h * 4);
    }
    appendFrameFile(frame, fw, fh, moveFrame);
    thinIfOverCap();
}

void TimelapseRecorder::appendFrameFile(const std::vector<uint8_t>& rgba,
                                        int w, int h, bool moveFrame) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(frameDir(), ec);

    uLongf packedCap = compressBound(uLong(rgba.size()));
    std::vector<uint8_t> packed(packedCap);
    if (compress2(packed.data(), &packedCap, rgba.data(), uLong(rgba.size()),
                  Z_BEST_SPEED) != Z_OK)
        return;

    // Camera-move fillers carry an 'm' marker the export loops read back
    // ('.'<'m', so a step and its trailing fillers still sort by serial).
    char name[24];
    std::snprintf(name, sizeof(name), "%08d%s.mzf", m_nextSerial,
                  moveFrame ? "m" : "");
    const std::string path = frameDir() + "/" + name;
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os.is_open()) return;
    const uint16_t w16 = uint16_t(w), h16 = uint16_t(h);
    const uint32_t rawSize = uint32_t(rgba.size());
    os.write(kMagic, 4);
    os.write(reinterpret_cast<const char*>(&w16), 2);
    os.write(reinterpret_cast<const char*>(&h16), 2);
    os.write(reinterpret_cast<const char*>(&rawSize), 4);
    os.write(reinterpret_cast<const char*>(packed.data()),
             std::streamsize(packedCap));
    if (!os.good()) return;
    os.close();
    ++m_nextSerial;
    m_frames.push_back(name);
}

void TimelapseRecorder::thinIfOverCap() {
    if (int(m_frames.size()) <= kMaxStoredFrames) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::string> kept;
    kept.reserve(m_frames.size() / 2 + 1);
    for (size_t i = 0; i < m_frames.size(); ++i) {
        if (i % 2 == 1 && i + 1 != m_frames.size()) // keep evens + the newest
            fs::remove(frameDir() + "/" + m_frames[i], ec);
        else
            kept.push_back(m_frames[i]);
    }
    m_frames = std::move(kept);
}

void TimelapseRecorder::clearFrames() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& f : m_frames) fs::remove(frameDir() + "/" + f, ec);
    m_frames.clear();
    m_nextSerial = 0;
}

bool TimelapseRecorder::encodeGif(const std::string& dir,
                                  const std::vector<std::string>& names,
                                  const std::string& gifPath,
                                  int condenseSeconds, std::string* err) {
    if (names.size() < 2) {
        if (err) *err = "Not enough frames recorded yet.";
        return false;
    }

    // Playback shape: each step is CROSSFADED in (kTweens short blended
    // frames), then HELD — hard step-cuts at a fixed rate read as flicker.
    // The final frame holds ~1.5 s so the loop has a resting point before it
    // restarts.
    constexpr int kTweens = 2;        // blended frames between steps
    constexpr int kTweenDelayCs = 5;  // each
    constexpr int kFinalHoldCs = 150;

    // Pick frames + the per-step hold. A condensed export samples steps
    // evenly and divides the requested duration across them — charging the
    // tween frames and the final hold against the budget too, so the export
    // actually lands on the requested length.
    std::vector<size_t> pick;
    int holdCs = 30; // full length: ~0.4 s per step including the blend
    if (condenseSeconds > 0) {
        const int budgetCs = condenseSeconds * 100;
        const int minStepCs = kTweens * kTweenDelayCs + 4;
        const size_t maxSteps =
            size_t(std::max(2, (budgetCs - kFinalHoldCs) / minStepCs));
        const size_t n = std::min(names.size(), maxSteps);
        pick.reserve(n);
        for (size_t i = 0; i < n; ++i)
            pick.push_back(i * (names.size() - 1) / (n - 1));
        holdCs = std::clamp((budgetCs - kFinalHoldCs) / int(n) -
                                kTweens * kTweenDelayCs,
                            4, 60);
    } else {
        pick.reserve(names.size());
        for (size_t i = 0; i < names.size(); ++i) pick.push_back(i);
    }

    // Canvas = dimensions of the first decodable picked frame.
    int cw = 0, ch = 0;
    GifEncoder gif;
    std::vector<uint8_t> rgba, canvas, prev, blend;
    int written = 0;
    for (size_t pi = 0; pi < pick.size(); ++pi) {
        int w = 0, h = 0;
        if (!readFrameFile(dir + "/" + names[pick[pi]], rgba, w, h)) continue;
        if (cw == 0) {
            cw = w;
            ch = h;
            const int longEdge = std::max(cw, ch);
            if (longEdge > kGifMaxLongEdge) {
                cw = std::max(1, cw * kGifMaxLongEdge / longEdge);
                ch = std::max(1, ch * kGifMaxLongEdge / longEdge);
            }
            if (!gif.begin(gifPath, cw, ch, holdCs)) {
                if (err) *err = "Couldn't write " + gifPath;
                return false;
            }
        }
        fitOnCanvas(rgba, w, h, canvas, cw, ch);
        const bool move = isMoveFrame(names[pick[pi]]);
        if (!prev.empty() && !move) {
            blend.resize(canvas.size());
            for (int t = 1; t <= kTweens; ++t) {
                const int a = t * 255 / (kTweens + 1);
                for (size_t i = 0; i < canvas.size(); ++i)
                    blend[i] = uint8_t(
                        (int(prev[i]) * (255 - a) + int(canvas[i]) * a) / 255);
                if (!gif.addFrame(blend.data(), kTweenDelayCs)) {
                    if (err) *err = "Write failed at frame " +
                                    std::to_string(written);
                    return false;
                }
            }
        }
        const bool last = (pi + 1 == pick.size());
        const int delay = last ? kFinalHoldCs : (move ? kTweenDelayCs : holdCs);
        if (!gif.addFrame(canvas.data(), delay)) {
            if (err) *err = "Write failed at frame " + std::to_string(written);
            return false;
        }
        prev = canvas;
        ++written;
    }
    if (written < 2) {
        if (err) *err = "Recorded frames could not be read back.";
        return false;
    }
    if (!gif.end()) {
        if (err) *err = "Couldn't finalize " + gifPath;
        return false;
    }
    return true;
}

bool TimelapseRecorder::encodeMp4(const std::string& dir,
                                  const std::vector<std::string>& names,
                                  const std::string& mp4Path,
                                  int condenseSeconds, std::string* err) {
    if (!VideoEncoder::available()) {
        if (err) *err = "MP4 export needs ffmpeg on PATH.";
        return false;
    }
    if (names.size() < 2) {
        if (err) *err = "Not enough frames recorded yet.";
        return false;
    }

    // Fixed 30 fps timeline: holds become repeated frames (x264 compresses
    // duplicates to almost nothing), tweens are 3 blended frames per step.
    constexpr int kFps = 30;
    constexpr int kTweens = 3;
    constexpr int kFinalHoldFrames = kFps * 3 / 2; // 1.5 s

    std::vector<size_t> pick;
    int holdFrames = 9; // full length: ~0.3 s per step + 0.1 s of blend
    if (condenseSeconds > 0) {
        // Budget the whole timeline (tweens + holds + final rest) so the
        // export lands on the requested duration.
        const int budget = condenseSeconds * kFps;
        const int minStep = kTweens + 2;
        const size_t maxSteps =
            size_t(std::max(2, (budget - kFinalHoldFrames) / minStep));
        const size_t n = std::min(names.size(), maxSteps);
        pick.reserve(n);
        for (size_t i = 0; i < n; ++i)
            pick.push_back(i * (names.size() - 1) / (n - 1));
        holdFrames = std::clamp(
            (budget - kFinalHoldFrames) / int(n) - kTweens, 2, 30);
    } else {
        pick.reserve(names.size());
        for (size_t i = 0; i < names.size(); ++i) pick.push_back(i);
    }

    int cw = 0, ch = 0;
    VideoEncoder enc;
    std::vector<uint8_t> rgba, canvas, prev, blend;
    int written = 0;
    for (size_t pi = 0; pi < pick.size(); ++pi) {
        int w = 0, h = 0;
        if (!readFrameFile(dir + "/" + names[pick[pi]], rgba, w, h)) continue;
        if (cw == 0) {
            cw = w;
            ch = h;
            if (!enc.begin(mp4Path, cw, ch, kFps)) {
                if (err) *err = "Couldn't start ffmpeg for " + mp4Path;
                return false;
            }
        }
        fitOnCanvas(rgba, w, h, canvas, cw, ch);
        const bool move = isMoveFrame(names[pick[pi]]);
        if (!prev.empty() && !move) {
            blend.resize(canvas.size());
            for (int t = 1; t <= kTweens; ++t) {
                const int a = t * 255 / (kTweens + 1);
                for (size_t i = 0; i < canvas.size(); ++i)
                    blend[i] = uint8_t(
                        (int(prev[i]) * (255 - a) + int(canvas[i]) * a) / 255);
                if (!enc.addFrame(blend.data())) {
                    if (err) *err = "ffmpeg stopped accepting frames.";
                    return false;
                }
            }
        }
        const bool last = (pi + 1 == pick.size());
        const int reps = last ? kFinalHoldFrames : (move ? 2 : holdFrames);
        for (int r = 0; r < reps; ++r)
            if (!enc.addFrame(canvas.data())) {
                if (err) *err = "ffmpeg stopped accepting frames.";
                return false;
            }
        prev = canvas;
        ++written;
    }
    if (written < 2) {
        if (err) *err = "Recorded frames could not be read back.";
        return false;
    }
    if (!enc.end()) {
        if (err) *err = "ffmpeg reported an encoding error.";
        return false;
    }
    return true;
}

} // namespace materializr
