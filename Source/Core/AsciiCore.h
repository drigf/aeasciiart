// AsciiCore.h
//
// Platform-independent heart of the AsciiDepth effect.  Nothing in here
// knows about After Effects: it takes pixels in, produces pixels out.  That
// keeps it unit-testable from the command line (see Tools/Harness).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ascii
{

// Channel order is always A,R,G,B (After Effects' native order).
// ARGB16 uses After Effects' 15-bit-plus-one range: 0..32768.
enum class PixelFormat
{
    ARGB8,
    ARGB16
};

struct ImageView
{
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    size_t rowBytes = 0;
    PixelFormat format = PixelFormat::ARGB8;
};

struct OutputView
{
    uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    size_t rowBytes = 0;
    PixelFormat format = PixelFormat::ARGB8;
};

// A small single-channel float image (0..1).  Used for the subject mask.
struct FloatMap
{
    int width = 0;
    int height = 0;
    std::vector<float> values;

    bool empty() const { return values.empty(); }
    // Bilinear sample at normalised coordinates (u,v in 0..1).
    float sample(float u, float v) const;
};

enum class CharSet : int
{
    Classic = 0,
    Simple,
    Binary,
    Blocks,
    Count
};

enum class DepthSource : int
{
    Auto = 0,    // heuristic estimate from the image itself
    Layer,       // user-supplied depth map layer (white = near)
    Subject      // Apple Vision foreground mask blended with the estimate
};

enum class Background : int
{
    Black = 0,
    Transparent,
    DimmedSource
};

// A set of pre-rendered glyph coverage bitmaps, sorted by ink density.
struct GlyphAtlas
{
    int cellW = 0;
    int cellH = 0;
    int count = 0;
    std::vector<uint8_t> pixels;  // count * cellW * cellH coverage, 0..255
    std::vector<float> ink;       // normalised ink per glyph, ascending, 0..1
    std::vector<uint16_t> lut;    // 1024 entries: target density -> glyph index

    const uint8_t* glyph(int index) const { return pixels.data() + size_t(index) * cellW * cellH; }
    int lookup(float density) const;  // density 0..1 -> glyph index
};

struct Settings
{
    // Characters
    float cellWidth = 12.0f;   // output pixels
    float cellAspect = 1.7f;   // cell height / cell width
    CharSet charSet = CharSet::Classic;
    bool invertChars = false;

    // Tone (how bright the *source* is -> which character is picked)
    float brightness = 0.0f;   // -1..1
    float contrast = 1.0f;     // 0..3
    float gamma = 1.0f;        // 0.2..3

    // Depth -> grey level of the glyphs
    DepthSource depthSource = DepthSource::Auto;
    bool invertDepth = false;
    bool normalizeDepth = true;  // stretch a depth layer to the full near..far range
    float depthContrast = 1.0f;  // 0..3
    float depthSmoothing = 2.0f; // grid cells of blur, 0..10
    float nearLevel = 1.0f;      // glyph grey for the closest things (1 = white)
    float farLevel = 0.25f;      // glyph grey for the furthest things (dark grey)

    // Weights for the automatic estimate
    float focusWeight = 0.6f;    // sharp / in-focus = near
    float heightWeight = 0.3f;   // lower in frame = near (ground plane prior)
    float brightWeight = 0.1f;   // brighter = near

    Background background = Background::Black;
};

struct Inputs
{
    const ImageView* source = nullptr;
    const ImageView* depthLayer = nullptr;     // only used for DepthSource::Layer
    const FloatMap* subjectMask = nullptr;     // only used for DepthSource::Subject
    std::shared_ptr<const GlyphAtlas> atlas;   // must match Settings cell size
};

// Cell size in whole pixels for atlas generation (so the host can fetch a
// matching atlas before calling Render).
void CellPixelSize(const Settings& s, int& cellW, int& cellH);

// Renders the ASCII frame.  `out` must be the same logical size as the source
// (if it differs the source is scaled by normalised coordinates).
void Render(const Inputs& in, const Settings& s, const OutputView& out);

// ---- Implemented by platform code (GlyphAtlas.mm / SubjectMask.mm) ----

// Returns a cached atlas for the char set at the given cell size.
std::shared_ptr<const GlyphAtlas> GetAtlas(CharSet set, int cellW, int cellH);

// Runs Apple Vision foreground segmentation.  Returns false if unavailable or
// nothing was found (mask is then left empty).
bool ComputeSubjectMask(const ImageView& src, FloatMap& mask);

}  // namespace ascii
