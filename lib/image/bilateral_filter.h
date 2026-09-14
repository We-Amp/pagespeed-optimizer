// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Edge-Preserving Bilateral Filter
//
// Applies bilateral filtering to reduce noise while preserving edges.
// Used by Phase 3a noise-adaptive denoising: images classified as kNoisy
// by the content analyzer are filtered before encoding, reducing encoded
// size by ~10-28% without perceptible quality loss.

#ifndef PAGESPEED_LIB_IMAGE_BILATERAL_FILTER_H_
#define PAGESPEED_LIB_IMAGE_BILATERAL_FILTER_H_

#include <cstdint>

namespace pagespeed {

class MessageHandler;

// Configuration for bilateral filter denoising.
struct BilateralFilterConfig {
  // Spatial sigma (kernel radius in pixels). Clamped to [0.5, 10.0].
  // Larger = more smoothing. Kernel radius = ceil(3 * sigma_spatial).
  float sigma_spatial = 3.0f;

  // Range sigma (intensity similarity). Must be > 0.0.
  // Larger = more aggressive. Edges preserved because distant
  // intensities contribute less.
  float sigma_range = 25.0f;
};

// Apply edge-preserving bilateral filter to pixel buffer.
//
// The filter cannot operate in-place because output pixels would
// corrupt the neighborhood read for subsequent pixels.
//
// Parameters:
//   input   - Input pixel buffer (RGB/RGBA/Gray, 8-bit). NOT modified.
//   output  - Output pixel buffer (must be pre-allocated, same size
//             as input: width * height * bpp bytes).
//   width   - Image width
//   height  - Image height
//   bpp     - Bytes per pixel (1, 3, or 4)
//   config  - Filter parameters (sigma values clamped internally)
//   handler - Message handler (may be null)
//
// Returns: true on success, false on failure (invalid dimensions/bpp).
bool ApplyBilateralFilter(const uint8_t* input, uint8_t* output, uint32_t width,
                          uint32_t height, int bpp,
                          const BilateralFilterConfig& config,
                          MessageHandler* handler);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_BILATERAL_FILTER_H_
