// GlyphAtlas.mm
//
// Builds glyph coverage bitmaps with CoreText and caches them per
// (character set, cell size).  Glyphs are sorted by how much ink they carry, so
// any character set automatically becomes a smooth dark-to-light ramp.

#include "AsciiCore.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <numeric>
#include <tuple>

namespace ascii
{

namespace
{

NSString* charactersFor(CharSet set)
{
    switch (set)
    {
    case CharSet::Simple:
        return @" .:-=+*#%@";
    case CharSet::Binary:
        return @" 01";
    case CharSet::Blocks:
        return @" ░▒▓█";
    case CharSet::Classic:
    default:
        return @" .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
    }
}

CTFontRef createMonoFont(CGFloat size)
{
    CTFontRef font = CTFontCreateWithName(CFSTR("Menlo-Regular"), size, nullptr);
    if (!font)
        font = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, size, nullptr);
    return font;
}

std::shared_ptr<GlyphAtlas> buildAtlas(CharSet set, int cw, int ch)
{
    @autoreleasepool
    {
        NSMutableArray<NSString*>* glyphs = [NSMutableArray array];
        NSString* chars = charactersFor(set);
        [chars enumerateSubstringsInRange:NSMakeRange(0, chars.length)
                                  options:NSStringEnumerationByComposedCharacterSequences
                               usingBlock:^(NSString* sub, NSRange, NSRange, BOOL*) { [glyphs addObject:sub]; }];

        const CGFloat baseSize = 100.0;
        CTFontRef font = createMonoFont(baseSize);

        // Fit the font's line box to the cell: horizontally by the advance of
        // 'M', vertically by ascent + descent.  The glyphs are stretched to the
        // cell's aspect, which is what makes tall thin ASCII cells look right.
        UniChar m = 'M';
        CGGlyph mg = 0;
        CTFontGetGlyphsForCharacters(font, &m, &mg, 1);
        CGSize adv{};
        CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &mg, &adv, 1);
        const CGFloat ascent = CTFontGetAscent(font), descent = CTFontGetDescent(font);
        const CGFloat sx = cw / std::max<CGFloat>(adv.width, 1.0);
        const CGFloat sy = ch / std::max<CGFloat>(ascent + descent, 1.0);

        auto atlas = std::make_shared<GlyphAtlas>();
        atlas->cellW = cw;
        atlas->cellH = ch;
        const int n = int(glyphs.count);
        const size_t cellBytes = size_t(cw) * ch;
        std::vector<uint8_t> raw(cellBytes * n, 0);
        std::vector<float> rawInk(size_t(n), 0.0f);

        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        for (int i = 0; i < n; ++i)
        {
            uint8_t* dst = raw.data() + cellBytes * size_t(i);
            CGContextRef ctx = CGBitmapContextCreate(dst, size_t(cw), size_t(ch), 8, size_t(cw), gray, kCGImageAlphaNone);
            CGContextSetShouldAntialias(ctx, true);
            CGContextSetShouldSmoothFonts(ctx, false);
            CGContextSetGrayFillColor(ctx, 1.0, 1.0);

            NSDictionary* attrs = @{
                (__bridge id)kCTFontAttributeName : (__bridge id)font,
                (__bridge id)kCTForegroundColorFromContextAttributeName : @YES
            };
            NSAttributedString* str = [[NSAttributedString alloc] initWithString:glyphs[size_t(i)] attributes:attrs];
            CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)str);

            CGContextScaleCTM(ctx, sx, sy);
            CGContextSetTextPosition(ctx, 0, descent);
            CTLineDraw(line, ctx);

            CFRelease(line);
            CGContextRelease(ctx);

            double sum = 0;
            for (size_t p = 0; p < cellBytes; ++p)
                sum += dst[p];
            rawInk[size_t(i)] = float(sum / (255.0 * double(cellBytes)));
        }
        CGColorSpaceRelease(gray);
        CFRelease(font);

        // Sort by ink, ascending.
        std::vector<int> order(static_cast<size_t>(n));
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return rawInk[size_t(a)] < rawInk[size_t(b)]; });

        const float maxInk = std::max(1e-6f, rawInk[size_t(order.back())]);
        atlas->count = n;
        atlas->pixels.resize(raw.size());
        atlas->ink.resize(size_t(n));
        for (int k = 0; k < n; ++k)
        {
            const int srcIdx = order[size_t(k)];
            std::copy_n(raw.data() + cellBytes * size_t(srcIdx), cellBytes, atlas->pixels.data() + cellBytes * size_t(k));
            atlas->ink[size_t(k)] = rawInk[size_t(srcIdx)] / maxInk;
        }

        // density -> nearest glyph by ink.
        atlas->lut.resize(1024);
        for (int d = 0; d < 1024; ++d)
        {
            const float target = float(d) / 1023.0f;
            auto it = std::lower_bound(atlas->ink.begin(), atlas->ink.end(), target);
            int hi = int(it - atlas->ink.begin());
            int lo = std::max(0, hi - 1);
            hi = std::min(hi, n - 1);
            atlas->lut[size_t(d)] = uint16_t(std::fabs(atlas->ink[size_t(lo)] - target) <= std::fabs(atlas->ink[size_t(hi)] - target) ? lo : hi);
        }
        return atlas;
    }
}

}  // namespace

std::shared_ptr<const GlyphAtlas> GetAtlas(CharSet set, int cellW, int cellH)
{
    using Key = std::tuple<int, int, int>;
    static std::mutex mutex;
    static std::map<Key, std::shared_ptr<const GlyphAtlas>> cache;
    static std::deque<Key> order;

    cellW = std::max(2, std::min(cellW, 256));
    cellH = std::max(2, std::min(cellH, 512));
    const Key key{int(set), cellW, cellH};

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
    }

    // Build outside the lock; a rare duplicate build is harmless.
    std::shared_ptr<const GlyphAtlas> built = buildAtlas(set, cellW, cellH);

    std::lock_guard<std::mutex> lock(mutex);
    auto inserted = cache.emplace(key, built);
    if (inserted.second)
    {
        order.push_back(key);
        while (order.size() > 24)  // keep memory bounded while animating cell size
        {
            cache.erase(order.front());
            order.pop_front();
        }
    }
    return inserted.first->second;
}

}  // namespace ascii
