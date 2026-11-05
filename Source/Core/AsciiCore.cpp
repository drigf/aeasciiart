// AsciiCore.cpp
//
// Cell analysis -> depth estimate -> glyph selection -> rasterisation.

#include "AsciiCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dispatch/dispatch.h>

namespace ascii
{

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace
{

template <class F>
void parallelFor(int n, F f)
{
    if (n <= 0)
        return;
    dispatch_apply(static_cast<size_t>(n),
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                   ^(size_t i) { f(static_cast<int>(i)); });
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Premultiplied Rec.709 luma of one source pixel (transparent = black).
inline float premultLuma(const ImageView& im, int x, int y)
{
    x = clampi(x, 0, im.width - 1);
    y = clampi(y, 0, im.height - 1);
    const uint8_t* row = im.data + size_t(y) * im.rowBytes;
    float a, r, g, b;
    if (im.format == PixelFormat::ARGB8)
    {
        const uint8_t* p = row + size_t(x) * 4;
        const float k = 1.0f / 255.0f;
        a = p[0] * k;
        r = p[1] * k;
        g = p[2] * k;
        b = p[3] * k;
    }
    else
    {
        const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + size_t(x) * 4;
        const float k = 1.0f / 32768.0f;
        a = p[0] * k;
        r = p[1] * k;
        g = p[2] * k;
        b = p[3] * k;
    }
    return clamp01((0.2126f * r + 0.7152f * g + 0.0722f * b) * a);
}

float percentile(std::vector<float> v, float p)
{
    if (v.empty())
        return 0.0f;
    size_t k = static_cast<size_t>(clamp01(p) * float(v.size() - 1) + 0.5f);
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Separable box blur on the cell grid, edge-clamped, applied `passes` times.
void boxBlur(std::vector<float>& g, int cols, int rows, int r, int passes = 2)
{
    if (r <= 0 || cols <= 0 || rows <= 0)
        return;
    std::vector<float> tmp(g.size());
    const float inv = 1.0f / float(2 * r + 1);
    for (int pass = 0; pass < passes; ++pass)
    {
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i)
            {
                float sum = 0;
                for (int k = -r; k <= r; ++k)
                    sum += g[size_t(j) * cols + clampi(i + k, 0, cols - 1)];
                tmp[size_t(j) * cols + i] = sum * inv;
            }
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i)
            {
                float sum = 0;
                for (int k = -r; k <= r; ++k)
                    sum += tmp[size_t(clampi(j + k, 0, rows - 1)) * cols + i];
                g[size_t(j) * cols + i] = sum * inv;
            }
    }
}

// Stretch values so the lo..hi percentiles map to 0..1.  Flat input -> 0.5.
void normalise(std::vector<float>& v, float loP, float hiP)
{
    float lo = percentile(v, loP);
    float hi = percentile(v, hiP);
    if (hi - lo < 1e-5f)
    {
        std::fill(v.begin(), v.end(), 0.5f);
        return;
    }
    const float inv = 1.0f / (hi - lo);
    for (float& x : v)
        x = clamp01((x - lo) * inv);
}

struct CellGrid
{
    int cols = 0;
    int rows = 0;
    float cellW = 0;
    float cellH = 0;
    std::vector<float> luma;   // mean source luma per cell
    std::vector<float> sharp;  // mean local gradient per cell (focus measure)
};

}  // namespace

// ---------------------------------------------------------------------------
// Public small pieces
// ---------------------------------------------------------------------------

float FloatMap::sample(float u, float v) const
{
    if (values.empty() || width <= 0 || height <= 0)
        return 0.0f;
    float x = u * float(width) - 0.5f;
    float y = v * float(height) - 0.5f;
    x = std::min(std::max(x, 0.0f), float(width - 1));
    y = std::min(std::max(y, 0.0f), float(height - 1));
    int x0 = int(x), y0 = int(y);
    int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    float fx = x - float(x0), fy = y - float(y0);
    float a = values[size_t(y0) * width + x0], b = values[size_t(y0) * width + x1];
    float c = values[size_t(y1) * width + x0], d = values[size_t(y1) * width + x1];
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

int GlyphAtlas::lookup(float density) const
{
    if (lut.empty())
        return 0;
    int k = clampi(int(clamp01(density) * 1023.0f + 0.5f), 0, 1023);
    return lut[size_t(k)];
}

void CellPixelSize(const Settings& s, int& cellW, int& cellH)
{
    float w = std::max(2.0f, s.cellWidth);
    float h = std::max(2.0f, w * std::max(0.25f, s.cellAspect));
    cellW = std::max(2, int(std::lround(w)));
    cellH = std::max(2, int(std::lround(h)));
}

// ---------------------------------------------------------------------------
// Depth
// ---------------------------------------------------------------------------

namespace
{

// Heuristic depth estimate, 0 = far .. 1 = near.  A single 2D frame has no true
// depth, so this blends three weak monocular cues:
//   focus  - things in focus (high local detail) tend to be the subject
//   height - lower in frame tends to be closer to the camera
//   bright - brighter things tend to be closer / better lit
std::vector<float> estimateAuto(const CellGrid& g, const Settings& s, int smoothR)
{
    const size_t n = size_t(g.cols) * g.rows;
    std::vector<float> depth(n, 0.5f);

    const float wF = std::max(0.0f, s.focusWeight);
    const float wH = std::max(0.0f, s.heightWeight);
    const float wB = std::max(0.0f, s.brightWeight);
    const float wSum = wF + wH + wB;
    if (wSum < 1e-4f)
        return depth;

    // Detail is only meaningful over an area, so spread it wider than the
    // final smoothing, and compress it so a few hard edges don't dominate.
    std::vector<float> focus(n);
    for (size_t i = 0; i < n; ++i)
        focus[i] = std::sqrt(std::max(0.0f, g.sharp[i]));
    boxBlur(focus, g.cols, g.rows, std::max(2, smoothR + 1));
    normalise(focus, 0.05f, 0.95f);

    std::vector<float> bright = g.luma;
    boxBlur(bright, g.cols, g.rows, 1);
    normalise(bright, 0.05f, 0.95f);

    for (int j = 0; j < g.rows; ++j)
    {
        const float height = g.rows > 1 ? float(j) / float(g.rows - 1) : 0.5f;
        for (int i = 0; i < g.cols; ++i)
        {
            size_t k = size_t(j) * g.cols + i;
            depth[k] = (wF * focus[k] + wH * height + wB * bright[k]) / wSum;
        }
    }

    boxBlur(depth, g.cols, g.rows, smoothR);
    normalise(depth, 0.02f, 0.98f);
    return depth;
}

// Average of a 3x3 spread of probes across a cell, through `fn(u,v)`.
template <class F>
float probe3x3(float cx0, float cy0, float cx1, float cy1, F fn)
{
    float sum = 0;
    for (int b = 0; b < 3; ++b)
        for (int a = 0; a < 3; ++a)
            sum += fn(cx0 + (cx1 - cx0) * (float(a) + 0.5f) / 3.0f, cy0 + (cy1 - cy0) * (float(b) + 0.5f) / 3.0f);
    return sum / 9.0f;
}

}  // namespace

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void Render(const Inputs& in, const Settings& s, const OutputView& out)
{
    if (!in.source || !in.source->data || !in.atlas || in.atlas->count <= 0 || !out.data || out.width <= 0 ||
        out.height <= 0)
        return;

    const ImageView& src = *in.source;
    const GlyphAtlas& atlas = *in.atlas;
    const int W = out.width, H = out.height;

    // --- 1. Cell grid ------------------------------------------------------
    CellGrid g;
    g.cellW = std::max(2.0f, s.cellWidth);
    g.cellH = std::max(2.0f, g.cellW * std::max(0.25f, s.cellAspect));
    g.cols = std::max(1, int(std::ceil(float(W) / g.cellW)));
    g.rows = std::max(1, int(std::ceil(float(H) / g.cellH)));
    const size_t nCells = size_t(g.cols) * g.rows;
    g.luma.assign(nCells, 0.0f);
    g.sharp.assign(nCells, 0.0f);

    const float sxScale = float(src.width) / float(W);
    const float syScale = float(src.height) / float(H);

    parallelFor(g.rows, [&](int j) {
        const float y0 = float(j) * g.cellH, y1 = std::min(float(H), y0 + g.cellH);
        const int ny = clampi(int(y1 - y0), 1, 16);
        for (int i = 0; i < g.cols; ++i)
        {
            const float x0 = float(i) * g.cellW, x1 = std::min(float(W), x0 + g.cellW);
            const int nx = clampi(int(x1 - x0), 1, 16);

            float samples[16 * 16];
            float sum = 0;
            for (int b = 0; b < ny; ++b)
            {
                const int sy = int((y0 + (y1 - y0) * (float(b) + 0.5f) / float(ny)) * syScale);
                for (int a = 0; a < nx; ++a)
                {
                    const int sx = int((x0 + (x1 - x0) * (float(a) + 0.5f) / float(nx)) * sxScale);
                    float l = premultLuma(src, sx, sy);
                    samples[b * nx + a] = l;
                    sum += l;
                }
            }
            float grad = 0;
            int gn = 0;
            for (int b = 0; b < ny; ++b)
                for (int a = 0; a < nx; ++a)
                {
                    float c = samples[b * nx + a];
                    if (a + 1 < nx)
                    {
                        grad += std::fabs(samples[b * nx + a + 1] - c);
                        ++gn;
                    }
                    if (b + 1 < ny)
                    {
                        grad += std::fabs(samples[(b + 1) * nx + a] - c);
                        ++gn;
                    }
                }
            const size_t k = size_t(j) * g.cols + i;
            g.luma[k] = sum / float(nx * ny);
            g.sharp[k] = gn ? grad / float(gn) : 0.0f;
        }
    });

    // --- 2. Depth per cell (0 far .. 1 near) ---------------------------------
    const int smoothR = clampi(int(std::lround(s.depthSmoothing)), 0, 10);
    std::vector<float> depth;

    auto cellRect = [&](int i, int j, float& u0, float& v0, float& u1, float& v1) {
        u0 = float(i) * g.cellW / float(W);
        u1 = std::min(1.0f, float(i + 1) * g.cellW / float(W));
        v0 = float(j) * g.cellH / float(H);
        v1 = std::min(1.0f, float(j + 1) * g.cellH / float(H));
    };

    if (s.depthSource == DepthSource::Layer && in.depthLayer && in.depthLayer->data)
    {
        const ImageView& dl = *in.depthLayer;
        depth.assign(nCells, 0.5f);
        parallelFor(g.rows, [&](int j) {
            for (int i = 0; i < g.cols; ++i)
            {
                float u0, v0, u1, v1;
                cellRect(i, j, u0, v0, u1, v1);
                depth[size_t(j) * g.cols + i] = probe3x3(u0, v0, u1, v1, [&](float u, float v) {
                    return premultLuma(dl, int(u * float(dl.width)), int(v * float(dl.height)));
                });
            }
        });
        boxBlur(depth, g.cols, g.rows, smoothR);
        if (s.normalizeDepth)
            normalise(depth, 0.02f, 0.98f);  // use the full near..far range even for low-contrast maps
    }
    else if (s.depthSource == DepthSource::Subject && in.subjectMask && !in.subjectMask->empty())
    {
        std::vector<float> autoD = estimateAuto(g, s, smoothR);
        depth.assign(nCells, 0.5f);
        parallelFor(g.rows, [&](int j) {
            for (int i = 0; i < g.cols; ++i)
            {
                float u0, v0, u1, v1;
                cellRect(i, j, u0, v0, u1, v1);
                const float m = clamp01(probe3x3(u0, v0, u1, v1, [&](float u, float v) { return in.subjectMask->sample(u, v); }));
                const float a = autoD[size_t(j) * g.cols + i];
                // Subject lives in the near half of the range, background in the far part.
                depth[size_t(j) * g.cols + i] = m * (0.45f + 0.55f * a) + (1.0f - m) * (0.40f * a);
            }
        });
        boxBlur(depth, g.cols, g.rows, std::max(1, smoothR));
    }
    else
    {
        depth = estimateAuto(g, s, smoothR);
    }

    // --- 3. Per-cell glyph and grey level ---------------------------------------
    std::vector<uint16_t> cellGlyph(nCells);
    std::vector<float> cellLevel(nCells);
    const float invGamma = 1.0f / std::max(0.05f, s.gamma);
    const float nearL = clamp01(s.nearLevel), farL = clamp01(s.farLevel);
    const bool msgMode = s.messageMode && !atlas.seq.empty() && atlas.solid >= 0;

    for (size_t k = 0; k < nCells; ++k)
    {
        float t = (g.luma[k] - 0.5f) * s.contrast + 0.5f + s.brightness;
        t = std::pow(clamp01(t), invGamma);
        if (s.invertChars)
            t = 1.0f - t;
        cellGlyph[k] = uint16_t(atlas.lookup(t));

        float d = clamp01(depth[k]);
        if (s.invertDepth)
            d = 1.0f - d;
        d = clamp01(0.5f + (d - 0.5f) * s.depthContrast);
        cellLevel[k] = farL + (nearL - farL) * d;

        if (msgMode)
        {
            // Reading order across the whole frame: the message just keeps repeating.
            const long seqN = long(atlas.seq.size());
            long idx = (long(k) + long(s.scroll)) % seqN;
            if (idx < 0)
                idx += seqN;
            uint16_t glyph = atlas.seq[size_t(idx)];

            bool fill = false;
            switch (s.fillMode)
            {
            case 1: fill = t > s.fillThreshold; break;
            case 2: fill = t < s.fillThreshold; break;
            case 3: fill = d > s.fillThreshold; break;
            default: break;
            }
            cellGlyph[k] = fill ? uint16_t(atlas.solid) : glyph;
        }
    }

    // --- 4. Rasterise ---------------------------------------------------------------
    const int acw = atlas.cellW, ach = atlas.cellH;

    struct Axis
    {
        int cell, i0, i1;
        float f;
    };
    auto buildAxis = [&](int length, float cellSize, int cells, int atlasSize) {
        std::vector<Axis> ax;
        ax.resize(size_t(length));
        for (int p = 0; p < length; ++p)
        {
            float c = (float(p) + 0.5f) / cellSize;
            int cell = std::min(int(c), cells - 1);
            float u = c - float(cell);
            float a = std::min(std::max(u * float(atlasSize) - 0.5f, 0.0f), float(atlasSize - 1));
            int i0 = int(a);
            ax[size_t(p)] = {cell, i0, std::min(i0 + 1, atlasSize - 1), a - float(i0)};
        }
        return ax;
    };
    const std::vector<Axis> xs = buildAxis(W, g.cellW, g.cols, acw);
    const std::vector<Axis> ys = buildAxis(H, g.cellH, g.rows, ach);

    const bool deep = out.format == PixelFormat::ARGB16;
    const float outMax = deep ? 32768.0f : 255.0f;

    parallelFor((H + 15) / 16, [&](int blk) {
        const int yEnd = std::min(H, (blk + 1) * 16);
        for (int y = blk * 16; y < yEnd; ++y)
        {
            const Axis& ay = ys[size_t(y)];
            uint8_t* row = out.data + size_t(y) * out.rowBytes;
            for (int x = 0; x < W; ++x)
            {
                const Axis& ax = xs[size_t(x)];
                const size_t ci = size_t(ay.cell) * g.cols + ax.cell;
                const uint8_t* gl = atlas.glyph(cellGlyph[ci]);

                const float c00 = gl[ay.i0 * acw + ax.i0], c10 = gl[ay.i0 * acw + ax.i1];
                const float c01 = gl[ay.i1 * acw + ax.i0], c11 = gl[ay.i1 * acw + ax.i1];
                const float cov =
                    ((c00 + (c10 - c00) * ax.f) * (1.0f - ay.f) + (c01 + (c11 - c01) * ax.f) * ay.f) * (1.0f / 255.0f);

                float bg = 0.0f, alpha = 1.0f;
                if (s.background == Background::Transparent)
                    alpha = cov;
                else if (s.background == Background::DimmedSource)
                    bg = 0.18f * premultLuma(src, int(float(x) * sxScale), int(float(y) * syScale));

                const float grey = (bg * (1.0f - cov) + cellLevel[ci] * cov);
                const float gv = clamp01(grey) * outMax + 0.5f;
                const float av = clamp01(alpha) * outMax + 0.5f;

                if (deep)
                {
                    uint16_t* p = reinterpret_cast<uint16_t*>(row) + size_t(x) * 4;
                    p[0] = uint16_t(av);
                    p[1] = p[2] = p[3] = uint16_t(gv);
                }
                else
                {
                    uint8_t* p = row + size_t(x) * 4;
                    p[0] = uint8_t(av);
                    p[1] = p[2] = p[3] = uint8_t(gv);
                }
            }
        }
    });
}

}  // namespace ascii
