#pragma once
#include <cstdint>

// ===== Color LUTs =====
// hotLUT: black → red → yellow → white (classic "hot" colormap)
// grayLUT: black → white (linear grayscale)

extern uint32_t hotLUT[256];
extern uint32_t grayLUT[256];

/// Initialize the LUTs (idempotent). Call once at startup.
void initColorMap();

/// Pick the LUT for a colormap value. 0 = hot, 1 = grayscale.
const uint32_t *colorLut(int colormap);

/// Shared value→LUT-index mapping (linear/log), returns 0..255.
int mapToIndex(double value, double maxVal, int mapping, bool clipping);
