// Command-line harness for the ASCII Depth core (no After Effects needed).
//
//   asciidepth-harness input.png output.png [key=value ...]
//
// Keys: cell, aspect, charset(0-3), invertchars, brightness, contrast, gamma,
//       depth(0 auto,1 layer,2 subject), depthmap=<png>, invertdepth,
//       depthcontrast, smooth, near, far, focus, height, bright,
//       font(0-6), custom=<chars> (use with charset=4),
//       message=<text> (turns on message mode), fill(0 none,1 bright,2 dark,3 near), threshold, scroll,
//       bg(0 black,1 transparent,2 dimmed), deep(0/1: run the 16-bit path)

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Core/AsciiCore.h"

namespace
{

struct Bitmap
{
    int w = 0, h = 0;
    std::vector<uint8_t> argb;  // A,R,G,B
};

bool loadPNG(const char* path, Bitmap& bm)
{
    CGImageSourceRef src = CGImageSourceCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]], nullptr);
    if (!src)
        return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img)
        return false;
    bm.w = int(CGImageGetWidth(img));
    bm.h = int(CGImageGetHeight(img));
    bm.argb.assign(size_t(bm.w) * bm.h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // Straight (non-premultiplied) isn't available for drawing; use premultiplied.
    CGContextRef ctx = CGBitmapContextCreate(bm.argb.data(), size_t(bm.w), size_t(bm.h), 8, size_t(bm.w) * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Big);
    CGContextDrawImage(ctx, CGRectMake(0, 0, bm.w, bm.h), img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    return true;
}

bool savePNG(const char* path, const uint8_t* argb, int w, int h, size_t rowBytes)
{
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(const_cast<uint8_t*>(argb), size_t(w), size_t(h), 8, rowBytes, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Big);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    bool ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return ok;
}

std::vector<uint16_t> to16(const Bitmap& b)
{
    std::vector<uint16_t> out(b.argb.size());
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = uint16_t((b.argb[i] * 32768 + 127) / 255);
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: %s input.png output.png [key=value ...]\n", argv[0]);
            return 2;
        }
        std::map<std::string, std::string> kv;
        for (int i = 3; i < argc; ++i)
        {
            const char* eq = strchr(argv[i], '=');
            if (eq)
                kv[std::string(argv[i], size_t(eq - argv[i]))] = eq + 1;
        }
        auto f = [&](const char* k, float d) { return kv.count(k) ? float(atof(kv[k].c_str())) : d; };

        Bitmap in;
        if (!loadPNG(argv[1], in))
        {
            fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }

        ascii::Settings s;
        s.cellWidth = f("cell", 12);
        s.cellAspect = f("aspect", 1.7f);
        s.charSet = ascii::CharSet(int(f("charset", 0)));
        s.invertChars = f("invertchars", 0) != 0;
        s.font = int(f("font", 0));
        if (kv.count("message"))
        {
            s.messageMode = true;
            s.message = kv["message"];
        }
        s.fillMode = int(f("fill", 1));
        s.fillThreshold = f("threshold", 0.5f);
        s.scroll = int(f("scroll", 0));
        if (kv.count("custom"))
            s.customChars = kv["custom"];
        s.brightness = f("brightness", 0);
        s.contrast = f("contrast", 1);
        s.gamma = f("gamma", 1);
        s.depthSource = ascii::DepthSource(int(f("depth", 0)));
        s.invertDepth = f("invertdepth", 0) != 0;
        s.normalizeDepth = f("normalize", 1) != 0;
        s.depthContrast = f("depthcontrast", 1);
        s.depthSmoothing = f("smooth", 2);
        s.nearLevel = f("near", 1);
        s.farLevel = f("far", 0.25f);
        s.focusWeight = f("focus", 0.6f);
        s.heightWeight = f("height", 0.3f);
        s.brightWeight = f("bright", 0.1f);
        s.background = ascii::Background(int(f("bg", 0)));
        const bool deep = f("deep", 0) != 0;

        std::vector<uint16_t> in16 = deep ? to16(in) : std::vector<uint16_t>();

        ascii::ImageView src;
        src.width = in.w;
        src.height = in.h;
        src.format = deep ? ascii::PixelFormat::ARGB16 : ascii::PixelFormat::ARGB8;
        src.data = deep ? reinterpret_cast<const uint8_t*>(in16.data()) : in.argb.data();
        src.rowBytes = size_t(in.w) * 4 * (deep ? 2 : 1);

        Bitmap depthBm;
        ascii::ImageView depthView;
        std::vector<uint16_t> depth16;
        ascii::Inputs inputs;
        inputs.source = &src;
        if (kv.count("depthmap") && loadPNG(kv["depthmap"].c_str(), depthBm))
        {
            depthView.width = depthBm.w;
            depthView.height = depthBm.h;
            depthView.format = ascii::PixelFormat::ARGB8;
            depthView.data = depthBm.argb.data();
            depthView.rowBytes = size_t(depthBm.w) * 4;
            inputs.depthLayer = &depthView;
        }

        ascii::FloatMap mask;
        if (s.depthSource == ascii::DepthSource::Subject)
        {
            auto t0 = std::chrono::steady_clock::now();
            bool ok = ascii::ComputeSubjectMask(src, mask);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "vision subject mask: %s (%dx%d) in %.0f ms\n", ok ? "ok" : "none", mask.width, mask.height, ms);
            if (ok)
                inputs.subjectMask = &mask;
        }

        int cw, ch;
        ascii::CellPixelSize(s, cw, ch);
        inputs.atlas = s.messageMode ? ascii::GetMessageAtlas(s.font, s.message, cw, ch)
                                     : ascii::GetAtlas(s.charSet, s.font, s.customChars, cw, ch);
        fprintf(stderr, "atlas: %d glyphs @ %dx%d\n", inputs.atlas->count, cw, ch);

        std::vector<uint8_t> outBuf(size_t(in.w) * in.h * 4 * (deep ? 2 : 1));
        ascii::OutputView out;
        out.data = outBuf.data();
        out.width = in.w;
        out.height = in.h;
        out.format = src.format;
        out.rowBytes = src.rowBytes;

        auto t0 = std::chrono::steady_clock::now();
        ascii::Render(inputs, s, out);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "render %dx%d (%s): %.1f ms\n", in.w, in.h, deep ? "16-bit" : "8-bit", ms);

        std::vector<uint8_t> png8;
        const uint8_t* pngData = outBuf.data();
        if (deep)
        {
            png8.resize(size_t(in.w) * in.h * 4);
            const uint16_t* p = reinterpret_cast<const uint16_t*>(outBuf.data());
            for (size_t i = 0; i < png8.size(); ++i)
                png8[i] = uint8_t((p[i] * 255 + 16384) / 32768);
            pngData = png8.data();
        }
        // Transparent backgrounds are straight alpha in the effect; premultiply for the PNG writer.
        std::vector<uint8_t> pm(pngData, pngData + size_t(in.w) * in.h * 4);
        for (size_t i = 0; i < pm.size(); i += 4)
            for (int c = 1; c < 4; ++c)
                pm[i + c] = uint8_t((pm[i + c] * pm[i] + 127) / 255);
        return savePNG(argv[2], pm.data(), in.w, in.h, size_t(in.w) * 4) ? 0 : 1;
    }
}
