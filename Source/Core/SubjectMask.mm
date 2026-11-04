// SubjectMask.mm
//
// Foreground ("who/what is the subject?") segmentation with Apple Vision.
// Needs macOS 14+; on older systems ComputeSubjectMask just returns false and
// the effect falls back to the automatic depth estimate.

#include "AsciiCore.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <mutex>

namespace ascii
{

namespace
{

// Vision is heavy and shares the Neural Engine / GPU; run one request at a time
// even when After Effects renders several frames concurrently.
std::mutex gVisionMutex;

constexpr int kMaxAnalysisSize = 640;

// Nearest-neighbour downscale of an ARGB source into a premultiplied RGBA CGImage.
CGImageRef createDownscaledImage(const ImageView& src, int& outW, int& outH)
{
    const float scale = std::min(1.0f, float(kMaxAnalysisSize) / float(std::max(src.width, src.height)));
    outW = std::max(8, int(float(src.width) * scale));
    outH = std::max(8, int(float(src.height) * scale));

    std::vector<uint8_t> rgba(size_t(outW) * outH * 4);
    for (int y = 0; y < outH; ++y)
    {
        const int sy = std::min(src.height - 1, int((float(y) + 0.5f) / float(outH) * float(src.height)));
        const uint8_t* row = src.data + size_t(sy) * src.rowBytes;
        for (int x = 0; x < outW; ++x)
        {
            const int sx = std::min(src.width - 1, int((float(x) + 0.5f) / float(outW) * float(src.width)));
            float a, r, g, b;
            if (src.format == PixelFormat::ARGB8)
            {
                const uint8_t* p = row + size_t(sx) * 4;
                a = p[0] / 255.0f; r = p[1] / 255.0f; g = p[2] / 255.0f; b = p[3] / 255.0f;
            }
            else
            {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + size_t(sx) * 4;
                a = p[0] / 32768.0f; r = p[1] / 32768.0f; g = p[2] / 32768.0f; b = p[3] / 32768.0f;
            }
            uint8_t* d = &rgba[(size_t(y) * outW + x) * 4];
            d[0] = uint8_t(std::min(1.0f, r * a) * 255.0f + 0.5f);  // premultiplied
            d[1] = uint8_t(std::min(1.0f, g * a) * 255.0f + 0.5f);
            d[2] = uint8_t(std::min(1.0f, b * a) * 255.0f + 0.5f);
            d[3] = uint8_t(std::min(1.0f, a) * 255.0f + 0.5f);
        }
    }

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // Copy the pixels out of `rgba` so the image owns its data.
    CFDataRef copy = CFDataCreate(nullptr, rgba.data(), CFIndex(rgba.size()));
    CGDataProviderRef owned = CGDataProviderCreateWithCFData(copy);
    CGImageRef image = CGImageCreate(size_t(outW), size_t(outH), 8, 32, size_t(outW) * 4, cs,
                          kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault, owned, nullptr, false,
                          kCGRenderingIntentDefault);
    CGDataProviderRelease(owned);
    CFRelease(copy);
    CGColorSpaceRelease(cs);
    return image;
}

API_AVAILABLE(macos(14.0))
bool runVision(const ImageView& src, FloatMap& mask)
{
    @autoreleasepool
    {
        int w = 0, h = 0;
        CGImageRef image = createDownscaledImage(src, w, h);
        if (!image)
            return false;

        VNGenerateForegroundInstanceMaskRequest* request = [[VNGenerateForegroundInstanceMaskRequest alloc] init];
        VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
        NSError* error = nil;
        bool ok = [handler performRequests:@[ request ] error:&error];
        CGImageRelease(image);
        if (!ok)
            return false;

        VNInstanceMaskObservation* obs = request.results.firstObject;
        if (!obs || obs.allInstances.count == 0)
            return false;

        CVPixelBufferRef buffer = [obs generateScaledMaskForImageForInstances:obs.allInstances
                                                           fromRequestHandler:handler
                                                                        error:&error];
        if (!buffer)
            return false;

        CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        const size_t bw = CVPixelBufferGetWidth(buffer), bh = CVPixelBufferGetHeight(buffer);
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        const OSType fmt = CVPixelBufferGetPixelFormatType(buffer);
        const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));

        bool good = false;
        if (base && bw > 0 && bh > 0 && (fmt == kCVPixelFormatType_OneComponent32Float || fmt == kCVPixelFormatType_OneComponent8))
        {
            mask.width = int(bw);
            mask.height = int(bh);
            mask.values.resize(bw * bh);
            for (size_t y = 0; y < bh; ++y)
                for (size_t x = 0; x < bw; ++x)
                {
                    float v = fmt == kCVPixelFormatType_OneComponent32Float
                                  ? reinterpret_cast<const float*>(base + y * stride)[x]
                                  : base[y * stride + x] / 255.0f;
                    mask.values[y * bw + x] = std::min(1.0f, std::max(0.0f, v));
                }
            good = true;
        }
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(buffer);
        return good;
    }
}

}  // namespace

bool ComputeSubjectMask(const ImageView& src, FloatMap& mask)
{
    mask = FloatMap();
    if (!src.data || src.width <= 0 || src.height <= 0)
        return false;
    if (@available(macOS 14.0, *))
    {
        std::lock_guard<std::mutex> lock(gVisionMutex);
        return runVision(src, mask);
    }
    return false;
}

}  // namespace ascii
