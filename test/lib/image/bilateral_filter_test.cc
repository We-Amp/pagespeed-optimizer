// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Bilateral Filter Unit Tests

#include "lib/image/bilateral_filter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"

namespace pagespeed {
namespace {

class BilateralFilterTest : public ::testing::Test {
 protected:
  ConsoleMessageHandler handler_;
  BilateralFilterConfig default_config_;
};

// Helper: generate random noise image using LCG PRNG.
std::vector<uint8_t> MakeNoise(uint32_t w, uint32_t h, int bpp,
                               uint32_t seed = 42) {
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * bpp);
  uint32_t state = seed;
  for (size_t i = 0; i < pixels.size(); ++i) {
    state = state * 1103515245 + 12345;
    pixels[i] = static_cast<uint8_t>((state >> 16) & 0xFF);
  }
  return pixels;
}

// Helper: generate solid color image.
std::vector<uint8_t> MakeSolid(uint32_t w, uint32_t h, int bpp, uint8_t value) {
  return std::vector<uint8_t>(static_cast<size_t>(w) * h * bpp, value);
}

// Helper: compute variance of pixel values (single channel).
double ComputeVariance(const uint8_t* data, size_t count) {
  double sum = 0.0;
  double sum_sq = 0.0;
  for (size_t i = 0; i < count; ++i) {
    auto v = static_cast<double>(data[i]);
    sum += v;
    sum_sq += v * v;
  }
  double mean = sum / count;
  return (sum_sq / count) - (mean * mean);
}

// Helper: compute mean absolute difference between two buffers.
double MeanAbsDiff(const uint8_t* a, const uint8_t* b, size_t count) {
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
  }
  return sum / count;
}

// --- Invalid input tests ---

TEST_F(BilateralFilterTest, NullInputReturnsFalse) {
  uint8_t output[3] = {};
  EXPECT_FALSE(ApplyBilateralFilter(nullptr, output, 1, 1, 3, default_config_,
                                    &handler_));
}

TEST_F(BilateralFilterTest, NullOutputReturnsFalse) {
  uint8_t input[3] = {128, 128, 128};
  EXPECT_FALSE(ApplyBilateralFilter(input, nullptr, 1, 1, 3, default_config_,
                                    &handler_));
}

TEST_F(BilateralFilterTest, ZeroDimensionsReturnFalse) {
  uint8_t pixel = 0;
  uint8_t out = 0;
  EXPECT_FALSE(
      ApplyBilateralFilter(&pixel, &out, 0, 0, 1, default_config_, &handler_));
  EXPECT_FALSE(
      ApplyBilateralFilter(&pixel, &out, 0, 1, 1, default_config_, &handler_));
  EXPECT_FALSE(
      ApplyBilateralFilter(&pixel, &out, 1, 0, 1, default_config_, &handler_));
}

TEST_F(BilateralFilterTest, InvalidBppReturnsFalse) {
  uint8_t input[4] = {128, 128, 128, 128};
  uint8_t output[4] = {};
  EXPECT_FALSE(
      ApplyBilateralFilter(input, output, 1, 1, 0, default_config_, &handler_));
  EXPECT_FALSE(
      ApplyBilateralFilter(input, output, 1, 1, 2, default_config_, &handler_));
  EXPECT_FALSE(
      ApplyBilateralFilter(input, output, 1, 1, 5, default_config_, &handler_));
}

// --- Boundary condition tests ---

TEST_F(BilateralFilterTest, OneByOnePixelGrayscale) {
  uint8_t input[1] = {200};
  uint8_t output[1] = {};
  EXPECT_TRUE(
      ApplyBilateralFilter(input, output, 1, 1, 1, default_config_, &handler_));
  // Single pixel: output must equal input (only weight is the center).
  EXPECT_EQ(200, output[0]);
}

TEST_F(BilateralFilterTest, OneByOnePixelRGB) {
  uint8_t input[3] = {100, 150, 200};
  uint8_t output[3] = {};
  EXPECT_TRUE(
      ApplyBilateralFilter(input, output, 1, 1, 3, default_config_, &handler_));
  EXPECT_EQ(100, output[0]);
  EXPECT_EQ(150, output[1]);
  EXPECT_EQ(200, output[2]);
}

TEST_F(BilateralFilterTest, TwoByTwoPixelSucceeds) {
  // 2x2 RGB, all same color.
  std::vector<uint8_t> input(2 * 2ULL * 3, 128);
  std::vector<uint8_t> output(2 * 2ULL * 3, 0);
  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), 2, 2, 3,
                                   default_config_, &handler_));
  // Uniform input → uniform output.
  for (size_t i = 0; i < output.size(); ++i) {
    EXPECT_EQ(128, output[i]);
  }
}

TEST_F(BilateralFilterTest, NarrowImage1xN) {
  // 1x32 RGB
  uint32_t w = 1, h = 32;
  auto input = MakeSolid(w, h, 3, 100);
  std::vector<uint8_t> output(input.size(), 0);
  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), w, h, 3,
                                   default_config_, &handler_));
  for (size_t i = 0; i < output.size(); ++i) {
    EXPECT_EQ(100, output[i]);
  }
}

TEST_F(BilateralFilterTest, NarrowImageNx1) {
  // 32x1 RGB
  uint32_t w = 32, h = 1;
  auto input = MakeSolid(w, h, 3, 100);
  std::vector<uint8_t> output(input.size(), 0);
  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), w, h, 3,
                                   default_config_, &handler_));
  for (size_t i = 0; i < output.size(); ++i) {
    EXPECT_EQ(100, output[i]);
  }
}

// --- Noise reduction tests ---

TEST_F(BilateralFilterTest, ReducesNoiseInNoisyImage) {
  uint32_t w = 64, h = 64;
  auto noisy = MakeNoise(w, h, 3);
  std::vector<uint8_t> output(noisy.size(), 0);

  EXPECT_TRUE(ApplyBilateralFilter(noisy.data(), output.data(), w, h, 3,
                                   default_config_, &handler_));

  // The filtered output should have lower variance (smoother).
  double input_var = ComputeVariance(noisy.data(), noisy.size());
  double output_var = ComputeVariance(output.data(), output.size());
  EXPECT_LT(output_var, input_var);
}

TEST_F(BilateralFilterTest, GrayscaleNoiseReduction) {
  uint32_t w = 64, h = 64;
  auto noisy = MakeNoise(w, h, 1);
  std::vector<uint8_t> output(noisy.size(), 0);

  EXPECT_TRUE(ApplyBilateralFilter(noisy.data(), output.data(), w, h, 1,
                                   default_config_, &handler_));

  double input_var = ComputeVariance(noisy.data(), noisy.size());
  double output_var = ComputeVariance(output.data(), output.size());
  EXPECT_LT(output_var, input_var);
}

// --- Edge preservation tests ---

TEST_F(BilateralFilterTest, PreservesSharpEdge) {
  // Create a 64x64 image with a sharp vertical edge at x=32:
  // left half = 40, right half = 200.
  uint32_t w = 64, h = 64;
  std::vector<uint8_t> input(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t val = (x < 32) ? 40 : 200;
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      input[idx] = val;
      input[idx + 1] = val;
      input[idx + 2] = val;
    }
  }

  std::vector<uint8_t> output(input.size(), 0);
  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), w, h, 3,
                                   default_config_, &handler_));

  // Check center of left half (far from edge): should stay near 40.
  size_t left_idx = (static_cast<size_t>(32) * w + 10) * 3;
  EXPECT_NEAR(40, output[left_idx], 2);

  // Check center of right half (far from edge): should stay near 200.
  size_t right_idx = (static_cast<size_t>(32) * w + 50) * 3;
  EXPECT_NEAR(200, output[right_idx], 2);

  // The edge itself: pixels at x=31 and x=32 should differ
  // significantly (edge preserved).
  size_t edge_left = (static_cast<size_t>(32) * w + 31) * 3;
  size_t edge_right = (static_cast<size_t>(32) * w + 32) * 3;
  int edge_diff = std::abs(static_cast<int>(output[edge_left]) -
                           static_cast<int>(output[edge_right]));
  EXPECT_GT(edge_diff, 100);  // Edge should remain strong
}

TEST_F(BilateralFilterTest, CheckerboardNotSmoothed) {
  // High-frequency structure (checkerboard) should be partially
  // preserved by the range weight — neighboring pixels of very
  // different intensity get low weight.
  uint32_t w = 32, h = 32;
  std::vector<uint8_t> input(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t val = ((x + y) % 2 == 0) ? 0 : 255;
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      input[idx] = val;
      input[idx + 1] = val;
      input[idx + 2] = val;
    }
  }

  std::vector<uint8_t> output(input.size(), 0);
  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), w, h, 3,
                                   default_config_, &handler_));

  // Check a center pixel: the range sigma (25) means neighbors with
  // 255 intensity difference get very low range weight
  // (exp(-255^2/(2*25^2)) ≈ 0), so the checkerboard pattern should
  // be largely preserved.
  size_t center_idx = (static_cast<size_t>(16) * w + 16) * 3;
  // The original value at (16,16) is 0 (even+even=even).
  // With bilateral filter, only same-valued neighbors contribute
  // significantly, so the output should stay close to 0.
  EXPECT_LT(output[center_idx], 30);
}

// --- Solid image identity test ---

TEST_F(BilateralFilterTest, SolidImageUnchanged) {
  uint32_t w = 32, h = 32;
  auto input = MakeSolid(w, h, 3, 128);
  std::vector<uint8_t> output(input.size(), 0);

  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), w, h, 3,
                                   default_config_, &handler_));

  // Solid input → identical output.
  for (size_t i = 0; i < output.size(); ++i) {
    EXPECT_EQ(128, output[i]);
  }
}

TEST_F(BilateralFilterTest, CleanImageMinimalChange) {
  // Smooth gradient (photo-like, no noise): bilateral filter should
  // produce only minimal changes.
  uint32_t w = 64, h = 64;
  std::vector<uint8_t> input(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      auto val = static_cast<uint8_t>((x * 255) / 63);
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      input[idx] = val;
      input[idx + 1] = val;
      input[idx + 2] = val;
    }
  }

  std::vector<uint8_t> output(input.size(), 0);
  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), w, h, 3,
                                   default_config_, &handler_));

  // Mean absolute difference should be small for a clean gradient.
  double mad = MeanAbsDiff(input.data(), output.data(), input.size());
  EXPECT_LT(mad, 3.0);
}

// --- RGBA alpha pass-through test ---

TEST_F(BilateralFilterTest, RGBAAlphaPassedThrough) {
  uint32_t w = 16, h = 16;
  std::vector<uint8_t> input(static_cast<size_t>(w) * h * 4);
  // Set specific alpha values and noisy RGB.
  uint32_t prng = 99;
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
    prng = prng * 1103515245 + 12345;
    input[i * 4] = static_cast<uint8_t>((prng >> 16) & 0xFF);
    prng = prng * 1103515245 + 12345;
    input[i * 4 + 1] = static_cast<uint8_t>((prng >> 16) & 0xFF);
    prng = prng * 1103515245 + 12345;
    input[i * 4 + 2] = static_cast<uint8_t>((prng >> 16) & 0xFF);
    input[i * 4 + 3] = static_cast<uint8_t>(i % 256);  // Unique alpha
  }

  std::vector<uint8_t> output(input.size(), 0);
  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), w, h, 4,
                                   default_config_, &handler_));

  // Verify alpha channel is unchanged.
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
    EXPECT_EQ(input[i * 4 + 3], output[i * 4 + 3])
        << "Alpha mismatch at pixel " << i;
  }
}

// --- Sigma configuration tests ---

TEST_F(BilateralFilterTest, LargerSpatialSigmaMoreSmoothing) {
  uint32_t w = 64, h = 64;
  auto noisy = MakeNoise(w, h, 3);

  BilateralFilterConfig small_sigma;
  small_sigma.sigma_spatial = 1.0f;
  small_sigma.sigma_range = 50.0f;

  BilateralFilterConfig large_sigma;
  large_sigma.sigma_spatial = 5.0f;
  large_sigma.sigma_range = 50.0f;

  std::vector<uint8_t> out_small(noisy.size(), 0);
  std::vector<uint8_t> out_large(noisy.size(), 0);

  EXPECT_TRUE(ApplyBilateralFilter(noisy.data(), out_small.data(), w, h, 3,
                                   small_sigma, &handler_));
  EXPECT_TRUE(ApplyBilateralFilter(noisy.data(), out_large.data(), w, h, 3,
                                   large_sigma, &handler_));

  double var_small = ComputeVariance(out_small.data(), out_small.size());
  double var_large = ComputeVariance(out_large.data(), out_large.size());

  // Larger spatial sigma → more smoothing → lower variance.
  EXPECT_LT(var_large, var_small);
}

TEST_F(BilateralFilterTest, NullHandlerSucceeds) {
  uint32_t w = 8, h = 8;
  auto input = MakeSolid(w, h, 3, 100);
  std::vector<uint8_t> output(input.size(), 0);

  EXPECT_TRUE(ApplyBilateralFilter(input.data(), output.data(), w, h, 3,
                                   default_config_, nullptr));
}

}  // namespace
}  // namespace pagespeed
