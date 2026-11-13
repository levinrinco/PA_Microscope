#include "ColorMap.h"
#include <cmath>

uint32_t hotLUT[256];
uint32_t grayLUT[256];
static bool s_lutReady = false;

void initColorMap() {
    if (s_lutReady) return;
    for (int i = 0; i < 256; ++i) {
        // Hot: black → red → yellow → white
        float t = static_cast<float>(i) / 255.0f;
        int r = (t < 0.33f) ? static_cast<int>(t / 0.33f * 255) : 255;
        int g = (t < 0.33f) ? 0 : (t < 0.66f) ? static_cast<int>((t - 0.33f) / 0.33f * 255) : 255;
        int b = (t < 0.66f) ? 0 : static_cast<int>((t - 0.66f) / 0.34f * 255);
        hotLUT[i] = (0xFFu << 24) | (b << 16) | (g << 8) | r;

        // Grayscale: black → white (R = G = B = i)
        int v = i;
        grayLUT[i] = (0xFFu << 24) | (v << 16) | (v << 8) | v;
    }
    s_lutReady = true;
}

const uint32_t *colorLut(int colormap) {
    initColorMap();
    return (colormap == 1) ? grayLUT : hotLUT;
}

// Shared mapping: value/displayMax → 0..255 LUT index
int mapToIndex(double value, double maxVal, int mapping, bool clipping) {
    initColorMap();
    if (maxVal < 1e-9) return 0;
    double displayMax = clipping ? (maxVal * 1.1) : maxVal;
    if (displayMax < 1e-9) return 0;
    double norm = value / displayMax;
    if (norm <= 0.0) return 0;

    if (mapping == 1) {  // log compression, 40 dB range
        const double K = 100.0;
        double logNorm = std::log10(1.0 + norm * (K - 1.0)) / std::log10(K);
        int idx = static_cast<int>(logNorm * 255.0);
        return (idx > 255) ? 255 : idx;
    } else {  // linear
        int idx = static_cast<int>(norm * 255.0);
        return (idx > 255) ? 255 : idx;
    }
}
