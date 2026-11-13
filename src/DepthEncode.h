#pragma once

#include <QImage>
#include <cstdint>
#include <vector>

// ===== Depth encoding =====
//
// A depth-encoded C-scan colors every en-face pixel by WHERE its signal came
// from along the depth axis instead of by how strong it is. The data behind it
// is one number per pixel:
//
//     depth index = position of this A-line's envelope peak, relative to the
//                   signal window start (windowOffset), in samples
//
// index 0 therefore means "peak at the top of the displayed window", which is
// also exactly how the on-screen depth ruler and the saved B-scan rows are
// numbered. Nothing here needs the sample rate or a speed of sound: the depth
// axis is in samples, and the raw data saved alongside can always be replayed
// later if a physical scale is wanted.
//
// The extraction and the rendering are deliberately free functions: the same
// code runs on the processing thread (raster accumulation), on the save thread
// (trajectory frames) and on the GUI thread (live display), so it must not
// carry state between calls or between threads.

enum class DepthDisplayMode {
    Pure = 0,     ///< hue = depth only
    Modulated = 1 ///< hue = depth, brightness = normalized intensity
};

/// Display-side settings for depth rendering. These are global display
/// preferences (like the colormap), not per-scan acquisition parameters.
struct DepthConfig {
    DepthDisplayMode mode = DepthDisplayMode::Modulated;

    /// Depth index mapped to the far end of the colormap. <= 0 means "use the
    /// full window depth" (see effectiveDepthRange()).
    int maxIndex = 0;

    /// Below this fraction of the en-face maximum the pixel is treated as
    /// background and rendered black, so noise does not paint a random depth
    /// over the whole image. 0 disables the gate.
    double minIntensityFraction = 0.05;

    /// 0 = turbo (recommended for depth), 1 = jet, 2 = viridis.
    // 0 Turbo, 1 Viridis, 2 Plasma, 3 Magma, 4 Inferno, 5 Jet, 6 red-to-cyan.
    int colorMap = 0;

    /// Optional hint for the automatic range: the depth window in use. The
    /// scan itself passes its own depth at render time; this only lets the
    /// settings dialog preview labels without a scan.
    int fallbackDepth = 0;
};

/// Highest displayable depth index for the given window depth: maxIndex when
/// set, otherwise depth-1. Never below 0.
int effectiveDepthRange(const DepthConfig &config, int windowDepth);

/// One A-line's depth: index of the envelope peak inside the window, relative
/// to the window start. 0 when the window has no positive sample.
int alinePeakDepth(const double *envelope, int windowDepth);

/// Paint a depth image. \a depth holds one index per pixel in row-major order;
/// \a intensity is the matching en-face intensity plane (used only by
/// DepthDisplayMode::Modulated) and may be empty in Pure mode.
///
/// \a windowDepth is the DEPTH of the signal window in samples (the same number
/// as ScanParams::depth), NOT the height of the image. The color scale has to
/// come from the acquisition window so that every frame, every view and the
/// saved PNG map the same depth to the same color; deriving it from a image
/// dimension instead would silently give the raster view, the trajectory view
/// and the color bar three different scales. The deepest valid index is
/// windowDepth - 1, which is what an unset config.maxIndex resolves to.
///
/// Depths above the configured range are clamped, pixels whose intensity falls
/// below the configured fraction are black, and a fully black plane is returned
/// when the inputs do not match \a imageW x \a imageH.
void encodeDepthImage(const float *depth, const float *intensity,
                      int imageW, int imageH, int windowDepth,
                      const DepthConfig &config,
                      QImage &outImage);

/// Magnitude of the depth colormap's dynamic range, for a color bar.
void depthLut(int colorMap, uint32_t out[256]);

/// Sensible default configuration for a given window depth.
DepthConfig defaultDepthConfig(int windowDepth);
