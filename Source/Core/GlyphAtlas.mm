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

std::shared_ptr<const GlyphAtlas> GetAtlasImpl(CharSet set, int font, const std::string& customUtf8, const std::string* message, int cellW, int cellH);

namespace
{

// PostScript names, in the same order as the Font popup.  CoreText substitutes a
// default face if one is missing, so a stripped-down system still renders.
const char* const kFontNames[kFontCount] = {
    "Menlo-Regular", "Menlo-Bold", "Monaco", "CourierNewPSMT", "CourierNewPS-BoldMT", "AndaleMono", "HelveticaNeue-Bold",
};

// Distinct, printable characters from the user's text (composed sequences, so emoji and
// accented letters stay whole), always preceded by a space as the lightest "glyph".
NSString* charactersForCustom(const std::string& utf8)
{
    NSString* text = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
    NSMutableString* out = [NSMutableString stringWithString:@" "];
    if (text)
    {
        NSMutableSet<NSString*>* seen = [NSMutableSet setWithObject:@" "];
        NSCharacterSet* blank = [NSCharacterSet whitespaceAndNewlineCharacterSet];
        [text enumerateSubstringsInRange:NSMakeRange(0, text.length)
                                 options:NSStringEnumerationByComposedCharacterSequences
                              usingBlock:^(NSString* sub, NSRange, NSRange, BOOL* stop) {
                                  if ([sub rangeOfCharacterFromSet:blank.invertedSet].location == NSNotFound)
                                      return;  // whitespace only
                                  if ([seen containsObject:sub])
                                      return;
                                  [seen addObject:sub];
                                  [out appendString:sub];
                                  if (seen.count >= 128)
                                      *stop = YES;
                              }];
    }
    return out;
}

NSString* charactersFor(CharSet set, const std::string& custom)
{
    switch (set)
    {
    case CharSet::Custom:
    {
        NSString* c = charactersForCustom(custom);
        if (c.length >= 2)
        {
            // need at least two distinct glyphs (space + one more) to make a ramp
            return c;
        }
        return @" .:-=+*#%@";
    }
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

CTFontRef createMonoFont(CGFloat size, int fontIndex)
{
    fontIndex = std::max(0, std::min(fontIndex, kFontCount - 1));
    CFStringRef name = CFStringCreateWithCString(nullptr, kFontNames[fontIndex], kCFStringEncodingUTF8);
    CTFontRef font = CTFontCreateWithName(name, size, nullptr);
    CFRelease(name);
    if (!font)
        font = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, size, nullptr);
    return font;
}

// `message` non-null = message mode: glyphs stay in order of first use (no ink sort), a solid cell is
// appended, and atlas->seq records the glyph for each character of the repeating message.
std::shared_ptr<GlyphAtlas> buildAtlas(CharSet set, int fontIndex, const std::string& custom, const std::string* message, int cw, int ch)
{
    @autoreleasepool
    {
        NSMutableArray<NSString*>* glyphs = [NSMutableArray array];
        std::vector<uint16_t> seq;
        const bool ordered = message != nullptr;
        if (!ordered)
        {
            NSString* chars = charactersFor(set, custom);
            [chars enumerateSubstringsInRange:NSMakeRange(0, chars.length)
                                      options:NSStringEnumerationByComposedCharacterSequences
                                   usingBlock:^(NSString* sub, NSRange, NSRange, BOOL*) { [glyphs addObject:sub]; }];
        }
        else
        {
            NSString* text = [[NSString alloc] initWithBytes:message->data() length:message->size() encoding:NSUTF8StringEncoding];
            if (!text || text.length == 0)
                text = @" ";
            NSCharacterSet* ws = [NSCharacterSet whitespaceAndNewlineCharacterSet];
            // Make sure the repeat is separated by a gap.
            if ([text rangeOfCharacterFromSet:ws.invertedSet options:NSBackwardsSearch].location == text.length - 1)
                text = [text stringByAppendingString:@" "];
            NSMutableDictionary<NSString*, NSNumber*>* index = [NSMutableDictionary dictionary];
            NSMutableArray<NSString*>* g = glyphs;
            std::vector<uint16_t>* seqP = &seq;
            [text enumerateSubstringsInRange:NSMakeRange(0, text.length)
                                     options:NSStringEnumerationByComposedCharacterSequences
                                  usingBlock:^(NSString* sub, NSRange, NSRange, BOOL*) {
                                      NSString* key = [sub rangeOfCharacterFromSet:ws.invertedSet].location == NSNotFound ? @" " : sub;
                                      NSNumber* found = index[key];
                                      if (!found)
                                      {
                                          found = @(g.count);
                                          index[key] = found;
                                          [g addObject:key];
                                      }
                                      seqP->push_back(uint16_t(found.intValue));
                                  }];
        }

        const CGFloat baseSize = 100.0;
        CTFontRef font = createMonoFont(baseSize, fontIndex);

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
        const int glyphCount = int(glyphs.count);
        const int n = glyphCount + (ordered ? 1 : 0);  // + solid cell in message mode
        const size_t cellBytes = size_t(cw) * ch;
        std::vector<uint8_t> raw(cellBytes * n, 0);
        std::vector<float> rawInk(size_t(n), 0.0f);

        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        for (int i = 0; i < glyphCount; ++i)
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

        if (ordered)
        {
            const int solid = n - 1;
            std::fill_n(raw.data() + cellBytes * size_t(solid), cellBytes, uint8_t(255));
            atlas->count = n;
            atlas->pixels = std::move(raw);
            atlas->solid = solid;
            atlas->seq = std::move(seq);
            return atlas;
        }

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

std::shared_ptr<const GlyphAtlas> GetAtlas(CharSet set, int font, const std::string& customUtf8, int cellW, int cellH)
{
    return GetAtlasImpl(set, font, customUtf8, nullptr, cellW, cellH);
}

std::shared_ptr<const GlyphAtlas> GetMessageAtlas(int font, const std::string& message, int cellW, int cellH)
{
    return GetAtlasImpl(CharSet::Classic, font, std::string(), &message, cellW, cellH);
}

std::shared_ptr<const GlyphAtlas> GetAtlasImpl(CharSet set, int font, const std::string& customUtf8, const std::string* message, int cellW, int cellH)
{
    using Key = std::tuple<int, int, int, int, std::string>;
    static std::mutex mutex;
    static std::map<Key, std::shared_ptr<const GlyphAtlas>> cache;
    static std::deque<Key> order;

    cellW = std::max(2, std::min(cellW, 256));
    cellH = std::max(2, std::min(cellH, 512));
    // Message atlases use a negative set id so they never collide with ramp atlases.
    const Key key{message ? -1 : int(set), std::max(0, std::min(font, kFontCount - 1)), cellW, cellH,
                  message ? *message : (set == CharSet::Custom ? customUtf8 : std::string())};

    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
    }

    // Build outside the lock; a rare duplicate build is harmless.
    std::shared_ptr<const GlyphAtlas> built = buildAtlas(set, std::get<1>(key), customUtf8, message, cellW, cellH);

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
