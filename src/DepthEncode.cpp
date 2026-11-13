#include "DepthEncode.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace {

// ---------------------------------------------------------------------------
// Colormaps
//
// Every LUT is built from a small table of anchors plus linear interpolation
// rather than a copied 256-entry table, so the source stays readable and a test
// can still assert exact entries. The anchor values themselves come from the
// authors' published tables (all CC0 / public domain) — 33 anchors reproduce a
// 256-entry table to within ~1-3 of 255 per channel, i.e. about one 8-bit step.
//
//   Turbo      — rainbow-like, high contrast; the historical default here.
//   Viridis    — perceptually uniform and colorblind safe, monotone in
//                lightness, and it stays readable in grayscale: the safest
//                choice for quantitative depth work.
//   Plasma     — same family, warm magenta→yellow; more contrast than viridis
//                in the deep end, less in the middle.
//   Magma      — near-black through purple to white; good when the depth map
//                should fade into a black background.
//   Inferno    — like magma with a stronger yellow-orange midrange.
//   Jet        — classic blue→cyan→yellow→red.
//   CyanMagenta— a simple magenta→cyan sweep with a wide hue span.
//
// Indices are part of the persisted settings (Display/depth_color_map), so the
// numbering below must stay stable.
// ---------------------------------------------------------------------------

struct Rgb { double r, g, b; };

inline double clamp01(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

inline uint32_t packRgb(double r, double g, double b)
{
    const int ri = static_cast<int>(clamp01(r) * 255.0 + 0.5);
    const int gi = static_cast<int>(clamp01(g) * 255.0 + 0.5);
    const int bi = static_cast<int>(clamp01(b) * 255.0 + 0.5);
    // Same byte order the rest of the app uses: 0xAARRGGBB.
    return (0xFFu << 24) | (static_cast<uint32_t>(bi) << 16)
           | (static_cast<uint32_t>(gi) << 8) | static_cast<uint32_t>(ri);
}

/// Piecewise-linear interpolation through explicit anchor colors, evenly spaced
/// from t=0 to t=1.
///
/// `scale` converts the anchors to 0..1: pass 255 for the palettes whose
/// tables are quoted as 0..255 components (the perceptual maps, taken verbatim
/// from their published tables) and 1 for the ones defined directly in 0..1.
/// Getting this wrong clips the whole ramp to white, so it is spelled out at
/// every call site rather than guessed.
Rgb sampleAnchors(const Rgb *anchors, int count, double t, double scale)
{
    t = clamp01(t);
    const double pos = t * (count - 1);
    int i = static_cast<int>(pos);
    if (i >= count - 1) i = count - 2;
    const double f = pos - i;
    const Rgb &a = anchors[i];
    const Rgb &b = anchors[i + 1];
    return {(a.r + (b.r - a.r) * f) / scale,
            (a.g + (b.g - a.g) * f) / scale,
            (a.b + (b.b - a.b) * f) / scale};
}

Rgb sampleTurbo(double t)
{
    static const Rgb anchors[] = {
        { 34,  23,  27},   // dark red      — shallowest
        { 73,  87, 220},   // blue
        { 47, 157, 244},   // cyan
        { 39, 214, 195},
        { 76, 248, 131},   // green
        {148, 250,  80},
        {222, 220,  50},   // yellow
        {255, 164,  34},
        {245,  94,  23},
        {186,  33,   8},
        {144,  12,   0},   // dark red      — deepest
    };
    return sampleAnchors(anchors, 11, t, 255.0);
}

Rgb sampleViridis(double t)
{
    static const Rgb anchors[] = {
        { 68,  1, 84}, { 71, 13, 96}, { 72, 24,106},
        { 72, 35,116}, { 71, 45,123}, { 69, 55,129},
        { 66, 64,134}, { 62, 73,137}, { 59, 82,139},
        { 55, 91,141}, { 51, 99,141}, { 47,107,142},
        { 44,114,142}, { 41,122,142}, { 38,130,142},
        { 35,137,142}, { 33,145,140}, { 31,151,139},
        { 31,159,136}, { 33,166,133}, { 39,173,129},
        { 49,181,123}, { 61,188,116}, { 76,194,108},
        { 92,200, 99}, {110,206, 88}, {129,211, 77},
        {149,216, 64}, {170,220, 50}, {192,223, 37},
        {213,226, 26}, {234,229, 26}, {253,231, 37},
    };
    return sampleAnchors(anchors, 33, t, 255.0);
}

Rgb samplePlasma(double t)
{
    static const Rgb anchors[] = {
        { 13,  8,135}, { 34,  6,144}, { 49,  5,151},
        { 63,  4,156}, { 76,  2,161}, { 89,  1,165},
        {102,  0,167}, {114,  1,168}, {126,  3,168},
        {138,  9,165}, {149, 17,161}, {160, 26,156},
        {170, 35,149}, {179, 44,142}, {188, 53,135},
        {196, 62,127}, {204, 71,120}, {210, 79,113},
        {217, 88,106}, {223, 98, 99}, {229,107, 93},
        {235,117, 86}, {240,127, 79}, {244,137, 72},
        {248,148, 65}, {251,159, 58}, {253,171, 51},
        {254,183, 45}, {253,195, 40}, {252,208, 37},
        {249,221, 37}, {245,235, 39}, {240,249, 33},
    };
    return sampleAnchors(anchors, 33, t, 255.0);
}

Rgb sampleMagma(double t)
{
    static const Rgb anchors[] = {
        {  0,  0,  4}, {  3,  3, 18}, { 10,  8, 34},
        { 19, 13, 52}, { 29, 17, 71}, { 41, 17, 90},
        { 54, 16,107}, { 68, 15,118}, { 81, 18,124},
        { 93, 23,127}, {106, 28,129}, {118, 33,129},
        {131, 38,129}, {144, 42,129}, {156, 46,127},
        {170, 51,125}, {183, 55,121}, {194, 59,117},
        {207, 64,112}, {219, 71,106}, {229, 80,100},
        {238, 91, 94}, {244,105, 92}, {249,120, 93},
        {251,135, 97}, {253,150,104}, {254,165,113},
        {254,180,123}, {254,194,135}, {254,209,148},
        {253,224,161}, {252,238,176}, {252,253,191},
    };
    return sampleAnchors(anchors, 33, t, 255.0);
}

Rgb sampleInferno(double t)
{
    static const Rgb anchors[] = {
        {  0,  0,  4}, {  4,  3, 18}, { 11,  7, 36},
        { 21, 11, 55}, { 33, 12, 74}, { 47, 10, 91},
        { 61,  9,101}, { 74, 12,107}, { 87, 16,110},
        {100, 21,110}, {113, 25,110}, {125, 30,109},
        {138, 34,106}, {151, 39,102}, {163, 44, 97},
        {176, 49, 91}, {188, 55, 84}, {198, 61, 77},
        {208, 69, 69}, {218, 78, 60}, {227, 89, 51},
        {235,100, 41}, {241,113, 31}, {246,126, 20},
        {249,140, 10}, {251,155,  6}, {252,170, 15},
        {251,186, 31}, {249,201, 50}, {245,217, 73},
        {242,232,101}, {243,245,134}, {252,255,164},
    };
    return sampleAnchors(anchors, 33, t, 255.0);
}

Rgb sampleJet(double t)
{
    static const Rgb anchors[] = {
        {0.0,   0.0,   0.5  },   // dark blue  — shallowest
        {0.0,   0.0,   1.0  },   // blue
        {0.0,   1.0,   1.0  },   // cyan
        {1.0,   1.0,   0.0  },   // yellow
        {1.0,   0.0,   0.0  },   // red
        {0.5,   0.0,   0.0  },   // dark red   — deepest
    };
    return sampleAnchors(anchors, 6, t, 1.0);
}

Rgb sampleCyanMagenta(double t)
{
    static const Rgb anchors[] = {
        {0.839, 0.153, 0.157},   // red      — shallowest
        {0.812, 0.478, 0.220},
        {0.580, 0.404, 0.741},
        {0.302, 0.745, 0.933},
        {0.180, 0.980, 0.910},   // bright cyan — deepest
    };
    return sampleAnchors(anchors, 5, t, 1.0);
}

Rgb sampleDepthColormap(int colorMap, double t)
{
    switch (colorMap) {
    case 1:  return sampleViridis(t);
    case 2:  return samplePlasma(t);
    case 3:  return sampleMagma(t);
    case 4:  return sampleInferno(t);
    case 5:  return sampleJet(t);
    case 6:  return sampleCyanMagenta(t);
    default: return sampleTurbo(t);
    }
}

constexpr int kLutCount = 7;

const uint32_t *lutFor(int colorMap)
{
    // Function-local static initialization is synchronized by C++11. The GUI
    // and save threads can request their first depth image concurrently, so a
    // separate unsynchronized "ready" flag would be a real data race.
    static const auto luts = [] {
        std::array<std::array<uint32_t, 256>, kLutCount> tables{};
        for (int c = 0; c < kLutCount; ++c) {
            for (int i = 0; i < 256; ++i) {
                const Rgb rgb = sampleDepthColormap(c, static_cast<double>(i) / 255.0);
                tables[c][i] = packRgb(rgb.r, rgb.g, rgb.b);
            }
        }
        return tables;
    }();
    return luts[std::clamp(colorMap, 0, kLutCount - 1)].data();
}

}  // namespace

void depthLut(int colorMap, uint32_t out[256])
{
    const uint32_t *lut = lutFor(colorMap);
    std::copy(lut, lut + 256, out);
}

int effectiveDepthRange(const DepthConfig &config, int windowDepth)
{
    if (config.maxIndex > 0) return config.maxIndex;
    const int depth = windowDepth > 0 ? windowDepth : config.fallbackDepth;
    return std::max(1, depth - 1);
}

int alinePeakDepth(const double *envelope, int windowDepth)
{
    if (!envelope || windowDepth <= 0) return 0;
    int best = 0;
    double bestValue = envelope[0];
    for (int i = 1; i < windowDepth; ++i) {
        if (envelope[i] > bestValue) {
            bestValue = envelope[i];
            best = i;
        }
    }
    return best;
}

void encodeDepthImage(const float *depth, const float *intensity,
                      int imageW, int imageH, int windowDepth,
                      const DepthConfig &config,
                      QImage &outImage)
{
    if (imageW <= 0 || imageH <= 0) {
        outImage = QImage();
        return;
    }
    if (outImage.width() != imageW || outImage.height() != imageH
        || outImage.format() != QImage::Format_RGB888) {
        outImage = QImage(imageW, imageH, QImage::Format_RGB888);
    }
    if (!depth) {
        outImage.fill(0);
        return;
    }

    // The depth axis is the acquisition window, never an image dimension.
    const int maxIndex = effectiveDepthRange(config, windowDepth);
    const double maxDepth = static_cast<double>(maxIndex);

    // Background gate. A depth map is only meaningful where there is signal,
    // so the gate is on by default: without it, detector noise would paint a
    // full-image random depth that looks like structure.
    double intensityMax = 0.0;
    if (intensity) {
        for (int i = 0; i < imageW * imageH; ++i)
            if (intensity[i] > intensityMax) intensityMax = intensity[i];
    }
    // An intensity plane that is entirely dark means there is no signal at all,
    // so the whole image is background. This has to be explicit: with a maximum
    // of zero the fractional gate below can never fire, and the frame would be
    // painted with the full depth palette.
    const bool emptyFrame = intensity && intensityMax <= 1e-12;
    double gate = 0.0;
    if (intensity && !emptyFrame && config.minIntensityFraction > 0.0)
        gate = config.minIntensityFraction * intensityMax;

    uint32_t lut[256];
    depthLut(config.colorMap, lut);
    const bool modulate = (config.mode == DepthDisplayMode::Modulated);

    const int total = imageW * imageH;
    // Normalization of the brightness: intensity / intensityMax. Kept as a
    // division on purpose — the reciprocal would turn the brightest pixel into
    // intensity * (1/intensity) == 1 for EVERY input, which silently makes the
    // modulation a no-op.
    const bool canModulate = modulate && intensity && !emptyFrame;

    for (int i = 0; i < total; ++i) {
        const int y = i / imageW;
        const int x = i - y * imageW;
        uchar *pixel = outImage.scanLine(y) + x * 3;

        double v = static_cast<double>(depth[i]);
        if (!std::isfinite(v) || v < 0.0 || emptyFrame) {
            pixel[0] = pixel[1] = pixel[2] = 0;
            continue;
        }
        if (gate > 0.0 && (!intensity || intensity[i] < gate)) {
            pixel[0] = pixel[1] = pixel[2] = 0;
            continue;
        }
        if (v > maxDepth) v = maxDepth;

        const int index = static_cast<int>(v / maxDepth * 255.0 + 0.5);
        const uint32_t c = lut[std::clamp(index, 0, 255)];
        int r = static_cast<int>(c & 0xFF);
        int g = static_cast<int>((c >> 8) & 0xFF);
        int b = static_cast<int>((c >> 16) & 0xFF);

        if (canModulate) {
            const double brightness =
                clamp01(static_cast<double>(intensity[i]) / intensityMax);
            r = static_cast<int>(r * brightness + 0.5);
            g = static_cast<int>(g * brightness + 0.5);
            b = static_cast<int>(b * brightness + 0.5);
        }
        pixel[0] = static_cast<uchar>(std::clamp(r, 0, 255));
        pixel[1] = static_cast<uchar>(std::clamp(g, 0, 255));
        pixel[2] = static_cast<uchar>(std::clamp(b, 0, 255));
    }
}

DepthConfig defaultDepthConfig(int windowDepth)
{
    DepthConfig config;
    config.maxIndex = 0;   // full window
    config.fallbackDepth = windowDepth;
    return config;
}
