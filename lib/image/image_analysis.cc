// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

#include "lib/image/image_analysis.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "lib/base/message_handler.h"
#include "lib/image/image_frame_interface.h"
#include "lib/image/jpeg_utils.h"
#include "lib/image/pixel_format_optimizer.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_utils.h"

namespace pagespeed {

namespace {

// Threshold for histogram. The histogram bins with values less than
// (max_hist_bin * kHistogramThreshold) will be ignored in computing
// the photo metric.
const float kHistogramThreshold = 0.01;

// Minimum metric value in order to be treated as a photo.
const float kPhotoMetricThreshold = 16;

template <class T>
inline T AbsDif(T v1, T v2) {
  return (v1 >= v2 ? v1 - v2 : v2 - v1);
}

}  // namespace

namespace image_compression {

using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;

// Compute the gradient by Sobel filter.
template <class T>
void ComputeGradientFromLuminance(const T* luminance, int width, int height,
                                  int elements_per_line, float norm_factor,
                                  uint8_t* gradient) {
  memset(gradient, 0,
         static_cast<size_t>(width) * height * sizeof(gradient[0]));
  // Remove the magnification factor of Sobel filter (4).
  norm_factor *= 0.25;
  for (int y = 1; y < height - 1; ++y) {
    size_t in_idx = static_cast<size_t>(y) * elements_per_line + 1;
    size_t out_idx = static_cast<size_t>(y) * width + 1;
    for (int x = 1; x < width - 1; ++x, ++in_idx, ++out_idx) {
      int32_t dif_y =
          static_cast<int32_t>(luminance[in_idx - elements_per_line - 1]) +
          (static_cast<int32_t>(luminance[in_idx - elements_per_line]) << 1) +
          static_cast<int32_t>(luminance[in_idx - elements_per_line + 1]) -
          static_cast<int32_t>(luminance[in_idx + elements_per_line - 1]) -
          (static_cast<int32_t>(luminance[in_idx + elements_per_line]) << 1) -
          static_cast<int32_t>(luminance[in_idx + elements_per_line + 1]);

      int32_t dif_x =
          static_cast<int32_t>(luminance[in_idx - 1 - elements_per_line]) +
          (static_cast<int32_t>(luminance[in_idx - 1]) << 1) +
          static_cast<int32_t>(luminance[in_idx - 1 + elements_per_line]) -
          static_cast<int32_t>(luminance[in_idx + 1 - elements_per_line]) -
          (static_cast<int32_t>(luminance[in_idx + 1]) << 1) -
          static_cast<int32_t>(luminance[in_idx + 1 + elements_per_line]);

      auto dif2 = static_cast<float>(dif_x * dif_x + dif_y * dif_y);
      float dif = std::sqrt(dif2) * norm_factor + 0.5f;
      gradient[out_idx] = static_cast<uint8_t>(std::min(255.0f, dif));
    }
  }
}

bool SobelGradient(const uint8_t* image, int width, int height,
                   int bytes_per_line, PixelFormat pixel_format,
                   MessageHandler* handler, uint8_t* gradient) {
  if (width < 3 || height < 3 ||
      (pixel_format != GRAY_8 && pixel_format != RGB_888 &&
       pixel_format != RGBA_8888)) {
    return false;
  }

  // Guard against overflow in width * height multiplication.
  if (static_cast<uint64_t>(width) * height > SIZE_MAX / sizeof(int32_t)) {
    return false;
  }

  if (pixel_format == GRAY_8) {
    const float norm_factor = 1.0f;
    ComputeGradientFromLuminance(image, width, height, bytes_per_line,
                                 norm_factor, gradient);
  } else {
    auto* luminance = static_cast<int32_t*>(
        malloc(static_cast<size_t>(width) * height * sizeof(int32_t)));
    if (luminance == nullptr) {
      return false;
    }

    const int num_channels =
        GetNumChannelsFromPixelFormat(pixel_format, handler);

    // Compute the luminance which is simply the average of R, G, and
    // B after applying the normalization factor.
    int32_t* out_pixel = luminance;
    for (int y = 0; y < height; ++y) {
      const uint8_t* in_channel =
          image + static_cast<ptrdiff_t>(y * bytes_per_line);
      for (int x = 0; x < width; ++x) {
        *out_pixel = static_cast<int32_t>(in_channel[0]) +
                     static_cast<int32_t>(in_channel[1]) +
                     static_cast<int32_t>(in_channel[2]);
        ++out_pixel;
        in_channel += num_channels;
      }
    }

    const float norm_factor = 1.0f / 3.0f;
    ComputeGradientFromLuminance(luminance, width, height, width, norm_factor,
                                 gradient);
    free(luminance);
  }
  return true;
}

void Histogram(const uint8_t* image, int width, int height, int bytes_per_line,
               int x0, int y0, float* hist) {
  assert(bytes_per_line >= width);

  uint32_t hist_int[kNumColorHistogramBins];
  memset(hist_int, 0, kNumColorHistogramBins * sizeof(hist_int[0]));

  // Aggregate the histogram.
  for (int y = y0; y < y0 + height; ++y) {
    size_t i = static_cast<size_t>(y) * bytes_per_line + x0;
    for (int x = 0; x < width; ++x, ++i) {
      ++hist_int[image[i]];
    }
  }

  for (int i = 0; i < kNumColorHistogramBins; ++i) {
    hist[i] = static_cast<float>(hist_int[i]);
  }
}

float WidestPeakWidth(const float* hist, float threshold) {
  float max_hist = *std::max_element(hist, hist + kNumColorHistogramBins);
  float threshold_hist = threshold * max_hist;

  int widest_peak = 0;
  int i = 0;
  while (i < kNumColorHistogramBins) {
    // Skip all bins which are smaller than the threshold.
    for (; i < kNumColorHistogramBins && hist[i] < threshold_hist; ++i) {
    }
    // Now we have a bin which meets the threshold, or we have
    // finished all of the bins.
    int first_significant_bin = i;
    for (; i < kNumColorHistogramBins && hist[i] >= threshold_hist; ++i) {
    }
    // Now we have gone through a peak or we have run out of bins.
    float width = i - first_significant_bin;
    if (widest_peak < width) {
      widest_peak = width;
    }
  }

  return widest_peak;
}

float PhotoMetric(const uint8_t* image, int width, int height,
                  int bytes_per_line, PixelFormat pixel_format, float threshold,
                  MessageHandler* handler) {
  const float KMinMetric = 0;

  // Guard against negative dimensions or overflow in allocation size.
  if (width <= 0 || height <= 0) {
    return KMinMetric;
  }
  auto* gradient = static_cast<uint8_t*>(
      malloc(static_cast<size_t>(width) * height * sizeof(uint8_t)));
  if (gradient == nullptr) {
    return KMinMetric;
  }

  if (!SobelGradient(image, width, height, bytes_per_line, pixel_format,
                     handler, gradient)) {
    free(gradient);
    return KMinMetric;
  }

  float hist[kNumColorHistogramBins];
  Histogram(gradient, width - 2, height - 2, width, 1, 1, hist);
  free(gradient);
  return WidestPeakWidth(hist, threshold);
}

bool IsPhoto(ScanlineReaderInterface* reader, MessageHandler* handler) {
  // Pretend that the image is not a photo if we cannot process it.
  bool kDefaultReturnValue = false;

  if (reader->GetPixelFormat() == UNSUPPORTED ||
      reader->GetPixelFormat() == RGBA_8888 || reader->GetImageWidth() == 0 ||
      reader->GetImageHeight() == 0) {
    return kDefaultReturnValue;
  }

  const int width = reader->GetImageWidth();
  const int height = reader->GetImageHeight();
  const PixelFormat pixel_format = reader->GetPixelFormat();
  const size_t bytes_per_line =
      static_cast<size_t>(width) *
      GetNumChannelsFromPixelFormat(pixel_format, handler);

  auto* image =
      static_cast<uint8_t*>(malloc(bytes_per_line * height * sizeof(uint8_t)));
  if (image == nullptr) {
    return kDefaultReturnValue;
  }

  for (int y = 0; y < height; ++y) {
    uint8_t* scanline = nullptr;
    if (!reader->HasMoreScanLines() ||
        !reader->ReadNextScanline(reinterpret_cast<void**>(&scanline))) {
      free(image);
      return kDefaultReturnValue;
    }
    memcpy(image + static_cast<size_t>(y) * bytes_per_line, scanline,
           bytes_per_line);
  }

  int bpl_int = static_cast<int>(bytes_per_line);
  float metric = PhotoMetric(image, width, height, bpl_int, pixel_format,
                             kHistogramThreshold, handler);
  free(image);
  return metric >= kPhotoMetricThreshold;
}

bool AnalyzeImage(ImageFormat image_type, const void* image_buffer,
                  size_t buffer_length, int* width, int* height,
                  bool* is_progressive, bool* is_animated,
                  bool* has_transparency, bool* is_photo, int* quality,
                  ScanlineReaderInterface** reader, MessageHandler* handler) {
  std::unique_ptr<ScanlineReaderInterface> sf_reader;
  std::unique_ptr<PixelFormatOptimizer> optimizer;
  bool image_is_animated = false;
  int image_width = 0;
  int image_height = 0;
  bool image_is_progressive = false;

  // PNG, JPEG, and WebP images only have a single frame (we skip GIF
  // entirely).
  sf_reader.reset(
      CreateScanlineReader(image_type, image_buffer, buffer_length, handler));
  if (sf_reader == nullptr) {
    return false;
  }

  image_width = sf_reader->GetImageWidth();
  image_height = sf_reader->GetImageHeight();
  image_is_progressive = sf_reader->IsProgressive();

  if (is_animated != nullptr) {
    *is_animated = image_is_animated;
  }
  if (width != nullptr) {
    *width = image_width;
  }
  if (height != nullptr) {
    *height = image_height;
  }
  if (is_progressive != nullptr) {
    *is_progressive = image_is_progressive;
  }

  // Finding whether the image is transparent or photo requires
  // processing the entire image.
  if (sf_reader != nullptr &&
      (has_transparency != nullptr || is_photo != nullptr)) {
    // Initialize the optimizer which will remove alpha channel if it
    // is completely opaque.
    optimizer = std::make_unique<PixelFormatOptimizer>(handler);
    if (!optimizer->Initialize(sf_reader.release()).Success()) {
      return false;
    }

    // Report the interesting information of the optimized image.
    if (has_transparency != nullptr) {
      *has_transparency = (optimizer->GetPixelFormat() == RGBA_8888);
    }
    if (is_photo != nullptr) {
      if (image_type == IMAGE_JPEG) {
        // Assume all JPEG images are photos.
        *is_photo = true;
      } else {
        // IsPhoto will read all scanlines of the image, so optimizer
        // cannot be used anymore.
        *is_photo = IsPhoto(optimizer.get(), handler);
        optimizer.reset();
      }
    }
  }

  if (quality != nullptr && image_type == IMAGE_JPEG) {
    *quality = JpegUtils::GetImageQualityFromImage(image_buffer, buffer_length,
                                                   handler);
  }

  // If "reader" has been requested, the caller is responsible for
  // destroying it.
  if (reader != nullptr) {
    if (optimizer != nullptr) {
      *reader = optimizer.release();
    } else if (sf_reader != nullptr) {
      *reader = sf_reader.release();
    } else {
      *reader = nullptr;
    }
  }

  return true;
}

}  // namespace image_compression

}  // namespace pagespeed
