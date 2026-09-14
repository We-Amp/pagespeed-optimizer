// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Bilateral Filter Implementation

#include "lib/image/bilateral_filter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "lib/base/message_handler.h"

namespace pagespeed {

namespace {

// Maximum spatial sigma (prevents pathological kernel sizes).
constexpr float kMaxSigmaSpatial = 10.0f;
constexpr float kMinSigmaSpatial = 0.5f;

// Minimum range sigma (prevents division by zero).
constexpr float kMinSigmaRange = 0.01f;

// Pre-compute spatial weight lookup table for a given kernel radius.
// Returns the kernel radius used.
int BuildSpatialLUT(float sigma_spatial, std::vector<float>& lut) {
  int radius = static_cast<int>(std::ceil(3.0f * sigma_spatial));
  int diameter = 2 * radius + 1;
  lut.resize(static_cast<size_t>(diameter) * diameter);

  float inv_2_sigma2 = -0.5f / (sigma_spatial * sigma_spatial);
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      auto d2 = static_cast<float>(dx * dx + dy * dy);
      int idx = (dy + radius) * diameter + (dx + radius);
      lut[idx] = std::exp(d2 * inv_2_sigma2);
    }
  }
  return radius;
}

// Pre-compute range weight lookup table (256 entries for intensity
// differences 0..255).
void BuildRangeLUT(float sigma_range, float range_lut[256]) {
  float inv_2_sigma2 = -0.5f / (sigma_range * sigma_range);
  for (int i = 0; i < 256; ++i) {
    auto d = static_cast<float>(i);
    range_lut[i] = std::exp(d * d * inv_2_sigma2);
  }
}

// Clamp coordinate to [0, max-1].
inline int ClampCoord(int v, int max_val) {
  return v < 0 ? 0 : (v >= max_val ? max_val - 1 : v);
}

}  // namespace

bool ApplyBilateralFilter(const uint8_t* input, uint8_t* output, uint32_t width,
                          uint32_t height, int bpp,
                          const BilateralFilterConfig& config,
                          MessageHandler* handler) {
  if (input == nullptr || output == nullptr) {
    return false;
  }
  if (width == 0 || height == 0) {
    return false;
  }
  if (bpp != 1 && bpp != 3 && bpp != 4) {
    if (handler != nullptr) {
      handler->Warning("BilateralFilter: unsupported bpp %d", bpp);
    }
    return false;
  }

  // Clamp sigma values for safety.
  float sigma_spatial =
      std::clamp(config.sigma_spatial, kMinSigmaSpatial, kMaxSigmaSpatial);
  float sigma_range = std::max(config.sigma_range, kMinSigmaRange);

  // Build lookup tables.
  std::vector<float> spatial_lut;
  int radius = BuildSpatialLUT(sigma_spatial, spatial_lut);
  int diameter = 2 * radius + 1;

  float range_lut[256];
  BuildRangeLUT(sigma_range, range_lut);

  int w = static_cast<int>(width);
  int h = static_cast<int>(height);

  // Number of color channels to filter (alpha passed through).
  int color_channels = (bpp == 4) ? 3 : bpp;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      size_t center_idx = (static_cast<size_t>(y) * w + x) * bpp;
      const uint8_t* center_pixel = input + center_idx;

      // Filter each color channel.
      for (int c = 0; c < color_channels; ++c) {
        float weight_sum = 0.0f;
        float value_sum = 0.0f;
        uint8_t center_val = center_pixel[c];

        for (int dy = -radius; dy <= radius; ++dy) {
          int ny = ClampCoord(y + dy, h);
          for (int dx = -radius; dx <= radius; ++dx) {
            int nx = ClampCoord(x + dx, w);
            size_t neighbor_idx = (static_cast<size_t>(ny) * w + nx) * bpp;
            uint8_t neighbor_val = input[neighbor_idx + c];

            // Spatial weight from LUT.
            int spatial_idx = (dy + radius) * diameter + (dx + radius);
            float spatial_w = spatial_lut[spatial_idx];

            // Range weight from LUT.
            int diff = std::abs(static_cast<int>(center_val) -
                                static_cast<int>(neighbor_val));
            float range_w = range_lut[diff];

            float w_total = spatial_w * range_w;
            weight_sum += w_total;
            value_sum += w_total * static_cast<float>(neighbor_val);
          }
        }

        float filtered_val =
            (weight_sum > 0.0f) ? (value_sum / weight_sum) : 0.0f;
        uint8_t filtered = static_cast<uint8_t>(
            std::clamp(std::round(filtered_val), 0.0f, 255.0f));
        output[center_idx + c] = filtered;
      }

      // Pass through alpha channel unchanged.
      if (bpp == 4) {
        output[center_idx + 3] = center_pixel[3];
      }
    }
  }

  return true;
}

}  // namespace pagespeed
