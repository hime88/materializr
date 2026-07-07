// Android backend for VideoEncoder (see VideoEncoder.h): AMediaCodec driving
// the hardware H.264 encoder + AMediaMuxer writing MP4 — the NDK's pure C
// media API, no Java/JNI. Runs on the timelapse feeder/export threads.
//
// Differences from the other backends, both accepted for v1:
//  - `fragmented` is ignored (AMediaMuxer can't write fragmented MP4), so a
//    crash loses the open segment rather than just its tail.
//  - The condensed export retimes by scaling sample timestamps during the
//    remux (frames can't be dropped from an H.264 stream copy); players treat
//    it as a high-fps clip, which everything tested handles fine.
#include "platform_defs.h"

#if defined(__ANDROID__)

#include "timelapse/VideoEncoder.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace materializr {

namespace {

constexpr int32_t kColorFlexible = 0x7F420888;   // COLOR_FormatYUV420Flexible
constexpr int32_t kColorSemiPlanar = 21;         // NV12
constexpr int32_t kColorPlanar = 19;             // I420
constexpr uint32_t kFlagCodecConfig = 2;         // BUFFER_FLAG_CODEC_CONFIG
constexpr uint32_t kFlagEndOfStream = 4;         // BUFFER_FLAG_END_OF_STREAM
constexpr uint32_t kFlagKeyFrame = 1;            // BUFFER_FLAG_KEY_FRAME

struct AndroidWriter {
    AMediaCodec* codec = nullptr;
    AMediaMuxer* muxer = nullptr;
    int fd = -1;
    ssize_t track = -1;
    bool muxStarted = false;
    bool wroteSample = false;
    int encW = 0, encH = 0;    // even-cropped encode dimensions
    int srcW = 0, srcH = 0;    // dimensions addFrame() is fed with
    int stride = 0, sliceH = 0;
    int32_t colorFormat = kColorSemiPlanar;
    int fps = 30;
    int64_t frameIdx = 0;
};

// BT.601 RGBA→YUV, chroma from a 2x2 RGB average.
inline uint8_t yOf(int r, int g, int b) {
    return uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}
inline uint8_t uOf(int r, int g, int b) {
    return uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
}
inline uint8_t vOf(int r, int g, int b) {
    return uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

// Drain every output buffer the encoder has ready; when the muxer track is
// announced, start the muxer. waitEos keeps pulling until the EOS flag lands.
bool drainOutputs(AndroidWriter* w, bool waitEos) {
    for (int spins = 0; spins < (waitEos ? 2000 : 32); ++spins) {
        AMediaCodecBufferInfo info{};
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(
            w->codec, &info, waitEos ? 10000 : 0);
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* fmt = AMediaCodec_getOutputFormat(w->codec);
            w->track = AMediaMuxer_addTrack(w->muxer, fmt);
            AMediaFormat_delete(fmt);
            if (w->track < 0) return false;
            if (AMediaMuxer_start(w->muxer) != AMEDIA_OK) return false;
            w->muxStarted = true;
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx < 0) {
            if (waitEos) continue; // TRY_AGAIN while waiting for EOS
            return true;
        }
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getOutputBuffer(w->codec, size_t(idx), &cap);
        if (buf && info.size > 0 && !(info.flags & kFlagCodecConfig) &&
            w->muxStarted) {
            AMediaMuxer_writeSampleData(w->muxer, size_t(w->track),
                                        buf + info.offset, &info);
            w->wroteSample = true;
        }
        const bool eos = (info.flags & kFlagEndOfStream) != 0;
        AMediaCodec_releaseOutputBuffer(w->codec, size_t(idx), false);
        if (eos) return true;
        if (waitEos) spins = 0; // progress: keep pulling
    }
    return !waitEos;
}

} // namespace

VideoEncoder::~VideoEncoder() {
    if (m_impl) {
        auto* w = static_cast<AndroidWriter*>(m_impl);
        if (w->codec) {
            AMediaCodec_stop(w->codec);
            AMediaCodec_delete(w->codec);
        }
        if (w->muxer) {
            if (w->muxStarted) AMediaMuxer_stop(w->muxer);
            AMediaMuxer_delete(w->muxer);
        }
        if (w->fd >= 0) close(w->fd);
        delete w;
        m_impl = nullptr;
    }
}

bool VideoEncoder::available() {
    static const bool ok = [] {
        AMediaCodec* c = AMediaCodec_createEncoderByType("video/avc");
        if (!c) return false;
        AMediaCodec_delete(c);
        return true;
    }();
    return ok;
}

bool VideoEncoder::begin(const std::string& path, int width, int height,
                         int fps, bool fragmented) {
    (void)fragmented; // AMediaMuxer can't write fragmented MP4
    if (m_impl || width <= 0 || height <= 0) return false;

    auto* w = new AndroidWriter;
    w->srcW = width;
    w->srcH = height;
    w->encW = width & ~1;
    w->encH = height & ~1;
    w->fps = fps > 0 ? fps : 30;

    w->codec = AMediaCodec_createEncoderByType("video/avc");
    if (!w->codec) {
        delete w;
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w->encW);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, w->encH);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFlexible);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, 8'000'000);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, w->fps);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    const media_status_t cfg = AMediaCodec_configure(
        w->codec, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(fmt);
    if (cfg != AMEDIA_OK || AMediaCodec_start(w->codec) != AMEDIA_OK) {
        AMediaCodec_delete(w->codec);
        w->codec = nullptr;
        delete w;
        return false;
    }

    // What layout did the codec actually pick? (Flexible resolves to NV12 on
    // most hardware, I420 on the software encoder.) Keys are plain strings so
    // this works on every API level; sensible fallbacks when absent.
    w->stride = w->encW;
    w->sliceH = w->encH;
    if (AMediaFormat* in = AMediaCodec_getInputFormat(w->codec)) {
        int32_t v = 0;
        if (AMediaFormat_getInt32(in, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) &&
            (v == kColorPlanar || v == kColorSemiPlanar))
            w->colorFormat = v;
        if (AMediaFormat_getInt32(in, "stride", &v) && v >= w->encW)
            w->stride = v;
        if (AMediaFormat_getInt32(in, "slice-height", &v) && v >= w->encH)
            w->sliceH = v;
        AMediaFormat_delete(in);
    }

    w->fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (w->fd < 0) {
        AMediaCodec_stop(w->codec);
        AMediaCodec_delete(w->codec);
        delete w;
        return false;
    }
    w->muxer = AMediaMuxer_new(w->fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!w->muxer) {
        close(w->fd);
        AMediaCodec_stop(w->codec);
        AMediaCodec_delete(w->codec);
        delete w;
        return false;
    }

    m_impl = w;
    m_frameBytes = size_t(width) * height * 4;
    return true;
}

bool VideoEncoder::addFrame(const uint8_t* rgba) {
    auto* w = static_cast<AndroidWriter*>(m_impl);
    if (!w || !rgba) return false;

    ssize_t idx = -1;
    for (int spins = 0; spins < 500 && idx < 0; ++spins)
        idx = AMediaCodec_dequeueInputBuffer(w->codec, 10000); // 10 ms
    if (idx < 0) return false;

    size_t cap = 0;
    uint8_t* in = AMediaCodec_getInputBuffer(w->codec, size_t(idx), &cap);
    const size_t need =
        size_t(w->stride) * w->sliceH + size_t(w->stride) * w->encH / 2;
    if (!in || cap < need) return false;
    std::memset(in, 0, need);

    // Luma.
    for (int y = 0; y < w->encH; ++y) {
        const uint8_t* src = rgba + size_t(y) * w->srcW * 4;
        uint8_t* dst = in + size_t(y) * w->stride;
        for (int x = 0; x < w->encW; ++x) {
            const uint8_t* p = src + size_t(x) * 4;
            dst[x] = yOf(p[0], p[1], p[2]);
        }
    }
    // Chroma from 2x2 averages.
    uint8_t* uvBase = in + size_t(w->stride) * w->sliceH;
    for (int y = 0; y < w->encH / 2; ++y) {
        const uint8_t* r0 = rgba + size_t(y * 2) * w->srcW * 4;
        const uint8_t* r1 = rgba + size_t(y * 2 + 1) * w->srcW * 4;
        for (int x = 0; x < w->encW / 2; ++x) {
            const uint8_t* a = r0 + size_t(x * 2) * 4;
            const uint8_t* b = r0 + size_t(x * 2 + 1) * 4;
            const uint8_t* c = r1 + size_t(x * 2) * 4;
            const uint8_t* d = r1 + size_t(x * 2 + 1) * 4;
            const int r = (a[0] + b[0] + c[0] + d[0]) / 4;
            const int g = (a[1] + b[1] + c[1] + d[1]) / 4;
            const int bl = (a[2] + b[2] + c[2] + d[2]) / 4;
            if (w->colorFormat == kColorPlanar) {
                uint8_t* uP = uvBase + size_t(y) * (w->stride / 2) + x;
                uint8_t* vP = uvBase +
                              size_t(w->stride / 2) * (w->sliceH / 2) +
                              size_t(y) * (w->stride / 2) + x;
                *uP = uOf(r, g, bl);
                *vP = vOf(r, g, bl);
            } else { // NV12
                uint8_t* uv = uvBase + size_t(y) * w->stride + size_t(x) * 2;
                uv[0] = uOf(r, g, bl);
                uv[1] = vOf(r, g, bl);
            }
        }
    }

    const int64_t ptsUs = w->frameIdx * 1000000LL / w->fps;
    ++w->frameIdx;
    if (AMediaCodec_queueInputBuffer(w->codec, size_t(idx), 0, need, ptsUs,
                                     0) != AMEDIA_OK)
        return false;
    return drainOutputs(w, /*waitEos=*/false);
}

bool VideoEncoder::end() {
    auto* w = static_cast<AndroidWriter*>(m_impl);
    if (!w) return false;

    ssize_t idx = -1;
    for (int spins = 0; spins < 500 && idx < 0; ++spins)
        idx = AMediaCodec_dequeueInputBuffer(w->codec, 10000);
    if (idx >= 0)
        AMediaCodec_queueInputBuffer(w->codec, size_t(idx), 0, 0, 0,
                                     kFlagEndOfStream);
    drainOutputs(w, /*waitEos=*/true);

    const bool ok = w->muxStarted && w->wroteSample;
    if (w->muxStarted) AMediaMuxer_stop(w->muxer);
    AMediaMuxer_delete(w->muxer);
    w->muxer = nullptr;
    AMediaCodec_stop(w->codec);
    AMediaCodec_delete(w->codec);
    w->codec = nullptr;
    close(w->fd);
    delete w;
    m_impl = nullptr;
    return ok;
}

bool VideoEncoder::concatSegments(const std::vector<std::string>& segments,
                                  const std::string& outPath, int totalFrames,
                                  int condenseSeconds, std::string* err) {
    if (segments.empty()) {
        if (err) *err = "Nothing recorded yet.";
        return false;
    }
    const int outFd = open(outPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (outFd < 0) {
        if (err) *err = "Couldn't write " + outPath;
        return false;
    }
    AMediaMuxer* mux = AMediaMuxer_new(outFd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!mux) {
        close(outFd);
        if (err) *err = "Couldn't create the MP4 muxer.";
        return false;
    }

    // Condensed export: scale sample timestamps (stream copy can't drop
    // H.264 frames); players read it as a high-fps clip.
    double scale = 1.0;
    const double totalUs = double(totalFrames) * 1e6 / 30.0;
    if (condenseSeconds > 0 && totalUs > condenseSeconds * 1e6)
        scale = condenseSeconds * 1e6 / totalUs;

    ssize_t track = -1;
    bool started = false, wrote = false;
    int trackW = 0, trackH = 0;
    int64_t offsetUs = 0;
    std::vector<uint8_t> buf(2 * 1024 * 1024);

    for (const auto& seg : segments) {
        const int fd = open(seg.c_str(), O_RDONLY);
        if (fd < 0) continue;
        struct stat st{};
        fstat(fd, &st);
        AMediaExtractor* ex = AMediaExtractor_new();
        if (AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) !=
            AMEDIA_OK) {
            AMediaExtractor_delete(ex);
            close(fd);
            continue; // unreadable (e.g. crashed mid-write): skip, keep going
        }
        int vid = -1;
        AMediaFormat* fmt = nullptr;
        const size_t nTracks = AMediaExtractor_getTrackCount(ex);
        for (size_t i = 0; i < nTracks && vid < 0; ++i) {
            AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
            const char* mime = nullptr;
            if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) &&
                mime && std::strncmp(mime, "video/", 6) == 0) {
                vid = int(i);
                fmt = f;
            } else {
                AMediaFormat_delete(f);
            }
        }
        if (vid < 0) {
            AMediaExtractor_delete(ex);
            close(fd);
            continue;
        }
        int32_t sw = 0, sh = 0;
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &sw);
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &sh);
        if (track < 0) {
            track = AMediaMuxer_addTrack(mux, fmt);
            trackW = sw;
            trackH = sh;
            if (track >= 0 && AMediaMuxer_start(mux) == AMEDIA_OK)
                started = true;
        }
        AMediaFormat_delete(fmt);
        if (!started || sw != trackW || sh != trackH) {
            // One track = one dimension set; a rotated-device segment with
            // different dimensions is skipped rather than corrupting the file.
            AMediaExtractor_delete(ex);
            close(fd);
            continue;
        }
        AMediaExtractor_selectTrack(ex, size_t(vid));
        int64_t lastPts = 0;
        for (;;) {
            const ssize_t n =
                AMediaExtractor_readSampleData(ex, buf.data(), buf.size());
            if (n < 0) break;
            AMediaCodecBufferInfo info{};
            info.offset = 0;
            info.size = int32_t(n);
            const int64_t t = AMediaExtractor_getSampleTime(ex);
            lastPts = t;
            info.presentationTimeUs = int64_t(double(offsetUs + t) * scale);
            info.flags = (AMediaExtractor_getSampleFlags(ex) &
                          AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC)
                             ? kFlagKeyFrame
                             : 0;
            AMediaMuxer_writeSampleData(mux, size_t(track), buf.data(), &info);
            wrote = true;
            AMediaExtractor_advance(ex);
        }
        offsetUs += lastPts + 1000000LL / 30;
        AMediaExtractor_delete(ex);
        close(fd);
    }

    if (started) AMediaMuxer_stop(mux);
    AMediaMuxer_delete(mux);
    close(outFd);
    if (!wrote) {
        unlink(outPath.c_str());
        if (err) *err = "Recorded segments could not be read.";
        return false;
    }
    return true;
}

} // namespace materializr

#endif // __ANDROID__
