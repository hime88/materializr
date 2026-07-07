// iOS backend for VideoEncoder (see io/VideoEncoder.h): AVAssetWriter driving
// the hardware H.264 encoder. Runs on the timelapse export worker thread —
// AVAssetWriter is explicitly fine (and preferred) off the main thread.
#include "platform_defs.h"

#if defined(MZ_IOS)

#include "io/VideoEncoder.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <cstring>
#include <unistd.h>

namespace materializr {

namespace {

// ObjC++ struct: strong ObjC members are legal here under ARC, and hiding it
// behind VideoEncoder's void* keeps AVFoundation out of the shared header.
struct WriterImpl {
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    int encW = 0, encH = 0;   // even-cropped encode dimensions
    int srcW = 0, srcH = 0;   // dimensions addFrame() is fed with
    int fps = 30;
    int64_t frameIdx = 0;
};

} // namespace

VideoEncoder::~VideoEncoder() {
    if (m_impl) {
        auto* im = static_cast<WriterImpl*>(m_impl);
        if (im->writer && im->writer.status == AVAssetWriterStatusWriting)
            [im->writer cancelWriting];
        delete im;
        m_impl = nullptr;
    }
}

bool VideoEncoder::available() { return true; }

bool VideoEncoder::begin(const std::string& path, int width, int height,
                         int fps) {
    if (m_impl || width <= 0 || height <= 0) return false;
    @autoreleasepool {
        auto* im = new WriterImpl;
        im->srcW = width;
        im->srcH = height;
        im->encW = width & ~1;  // H.264 wants even dimensions
        im->encH = height & ~1;
        im->fps = fps > 0 ? fps : 30;

        NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
        NSError* err = nil;
        im->writer = [[AVAssetWriter alloc] initWithURL:url
                                               fileType:AVFileTypeMPEG4
                                                  error:&err];
        if (!im->writer || err) {
            delete im;
            return false;
        }
        NSDictionary* settings = @{
            AVVideoCodecKey : AVVideoCodecTypeH264,
            AVVideoWidthKey : @(im->encW),
            AVVideoHeightKey : @(im->encH),
        };
        im->input = [[AVAssetWriterInput alloc]
            initWithMediaType:AVMediaTypeVideo
               outputSettings:settings];
        im->input.expectsMediaDataInRealTime = NO;
        NSDictionary* pbAttrs = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey : @(im->encW),
            (id)kCVPixelBufferHeightKey : @(im->encH),
        };
        im->adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
            initWithAssetWriterInput:im->input
         sourcePixelBufferAttributes:pbAttrs];
        if (![im->writer canAddInput:im->input]) {
            delete im;
            return false;
        }
        [im->writer addInput:im->input];
        if (![im->writer startWriting]) {
            delete im;
            return false;
        }
        [im->writer startSessionAtSourceTime:kCMTimeZero];

        m_impl = im;
        m_frameBytes = size_t(width) * height * 4;
        return true;
    }
}

bool VideoEncoder::addFrame(const uint8_t* rgba) {
    auto* im = static_cast<WriterImpl*>(m_impl);
    if (!im || !rgba) return false;
    @autoreleasepool {
        // The input applies back-pressure; wait politely (cap ~5 s).
        int spins = 0;
        while (!im->input.readyForMoreMediaData) {
            usleep(2000);
            if (++spins > 2500) return false;
        }

        CVPixelBufferRef pb = nullptr;
        if (im->adaptor.pixelBufferPool)
            CVPixelBufferPoolCreatePixelBuffer(
                kCFAllocatorDefault, im->adaptor.pixelBufferPool, &pb);
        if (!pb &&
            CVPixelBufferCreate(kCFAllocatorDefault, im->encW, im->encH,
                                kCVPixelFormatType_32BGRA, nullptr,
                                &pb) != kCVReturnSuccess)
            return false;

        CVPixelBufferLockBaseAddress(pb, 0);
        auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
        const size_t stride = CVPixelBufferGetBytesPerRow(pb);
        // RGBA (ours) → BGRA (encoder), cropping the odd edge if any.
        for (int y = 0; y < im->encH; ++y) {
            const uint8_t* src = rgba + size_t(y) * im->srcW * 4;
            uint8_t* dst = base + size_t(y) * stride;
            for (int x = 0; x < im->encW; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, 0);

        const CMTime t = CMTimeMake(im->frameIdx++, im->fps);
        const BOOL ok = [im->adaptor appendPixelBuffer:pb
                                  withPresentationTime:t];
        CVPixelBufferRelease(pb);
        return ok == YES;
    }
}

bool VideoEncoder::end() {
    auto* im = static_cast<WriterImpl*>(m_impl);
    if (!im) return false;
    bool ok = false;
    @autoreleasepool {
        [im->input markAsFinished];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [im->writer finishWritingWithCompletionHandler:^{
          dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        ok = im->writer.status == AVAssetWriterStatusCompleted;
    }
    delete im;
    m_impl = nullptr;
    return ok;
}

} // namespace materializr

#endif // MZ_IOS
