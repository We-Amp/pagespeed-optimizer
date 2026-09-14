// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// EXIF orientation handling implementation (issue #1005). See the header
// for the design overview.

#include "lib/image/exif_orientation.h"

#include <cstring>

#include "lib/base/message_handler.h"

namespace pagespeed::image_compression {

namespace {

using net_instaweb::SCANLINE_JPEGREADER;
using net_instaweb::SCANLINE_STATUS_INTERNAL_ERROR;
using net_instaweb::SCANLINE_STATUS_INVOCATION_ERROR;
using net_instaweb::SCANLINE_STATUS_SUCCESS;
using net_instaweb::SCANLINE_STATUS_UNSUPPORTED_FEATURE;

constexpr uint16_t kOrientationTag = 0x0112;
constexpr uint16_t kTypeShort = 3;

// Reads a big- or little-endian 16-bit value. |p| must have 2 valid bytes.
uint16_t Read16(const uint8_t* p, bool big_endian) {
  return big_endian ? static_cast<uint16_t>((p[0] << 8) | p[1])
                    : static_cast<uint16_t>((p[1] << 8) | p[0]);
}

// Reads a big- or little-endian 32-bit value. |p| must have 4 valid bytes.
uint32_t Read32(const uint8_t* p, bool big_endian) {
  return big_endian ? (static_cast<uint32_t>(p[0]) << 24) |
                          (static_cast<uint32_t>(p[1]) << 16) |
                          (static_cast<uint32_t>(p[2]) << 8) | p[3]
                    : (static_cast<uint32_t>(p[3]) << 24) |
                          (static_cast<uint32_t>(p[2]) << 16) |
                          (static_cast<uint32_t>(p[1]) << 8) | p[0];
}

// Parses the TIFF structure inside an EXIF APP1 payload (after the
// "Exif\0\0" prefix) and returns the Orientation tag value, or 0 when not
// found / malformed.
uint8_t ParseTiffOrientation(const uint8_t* tiff, size_t length) {
  if (length < 8) return 0;
  bool big_endian;
  if (tiff[0] == 'M' && tiff[1] == 'M') {
    big_endian = true;
  } else if (tiff[0] == 'I' && tiff[1] == 'I') {
    big_endian = false;
  } else {
    return 0;
  }
  if (Read16(tiff + 2, big_endian) != 42) return 0;
  uint32_t ifd_offset = Read32(tiff + 4, big_endian);
  if (ifd_offset < 8 || ifd_offset > length - 2) return 0;
  uint16_t num_entries = Read16(tiff + ifd_offset, big_endian);
  const uint8_t* entry = tiff + ifd_offset + 2;
  for (uint16_t i = 0; i < num_entries; ++i, entry += 12) {
    // Each IFD entry is 12 bytes: tag(2) type(2) count(4) value(4).
    if (static_cast<size_t>(entry - tiff) + 12 > length) return 0;
    if (Read16(entry, big_endian) != kOrientationTag) continue;
    if (Read16(entry + 2, big_endian) != kTypeShort) return 0;
    if (Read32(entry + 4, big_endian) != 1) return 0;
    // A SHORT value is stored inline in the first 2 bytes of the value
    // field, in the stream's byte order.
    uint16_t value = Read16(entry + 8, big_endian);
    return (value >= 1 && value <= 8) ? static_cast<uint8_t>(value) : 0;
  }
  return 0;
}

// Storage for the test-overridable bake ceiling (see header).
size_t& OrientationBakeByteLimitStorage() {
  static size_t limit = kMaxOrientationBakeBytes;
  return limit;
}

}  // namespace

size_t OrientationBakeByteLimit() { return OrientationBakeByteLimitStorage(); }

void SetOrientationBakeByteLimitForTesting(size_t limit) {
  OrientationBakeByteLimitStorage() =
      limit == 0 ? kMaxOrientationBakeBytes : limit;
}

uint8_t ReadJpegExifOrientation(const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  if (bytes == nullptr || length < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
    return 1;  // Not a JPEG stream.
  }
  size_t pos = 2;
  while (pos + 4 <= length) {
    if (bytes[pos] != 0xFF) return 1;  // Lost marker sync.
    // Skip fill bytes.
    while (pos < length && bytes[pos] == 0xFF) ++pos;
    if (pos >= length) return 1;
    const uint8_t marker = bytes[pos++];
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) ||
        marker == 0x01) {
      continue;  // Standalone markers carry no length field.
    }
    if (marker == 0xD9 || marker == 0xDA) {
      // EOI / start of scan: EXIF APP1 must precede image data.
      return 1;
    }
    if (pos + 2 > length) return 1;
    const size_t segment_length = (bytes[pos] << 8) | bytes[pos + 1];
    if (segment_length < 2 || pos + segment_length > length) return 1;
    if (marker == 0xE1 && segment_length >= 2 + 6 + 8) {
      const uint8_t* payload = bytes + pos + 2;
      const size_t payload_length = segment_length - 2;
      if (memcmp(payload, "Exif\0\0", 6) == 0) {
        uint8_t orientation =
            ParseTiffOrientation(payload + 6, payload_length - 6);
        return orientation != 0 ? orientation : 1;
      }
    }
    pos += segment_length;
  }
  return 1;
}

void BuildExifOrientationApp1(uint8_t orientation,
                              uint8_t out[kExifOrientationApp1Size]) {
  static constexpr uint8_t kTemplate[kExifOrientationApp1Size] = {
      'E',  'x',  'i',  'f',  0x00, 0x00,  // EXIF identifier.
      'I',  'I',  0x2A, 0x00,              // TIFF header, little-endian.
      0x08, 0x00, 0x00, 0x00,              // Offset of IFD0.
      0x01, 0x00,                          // IFD0: one entry.
      0x12, 0x01,                          // Tag 0x0112 (Orientation).
      0x03, 0x00,                          // Type SHORT.
      0x01, 0x00, 0x00, 0x00,              // Count 1.
      0x00, 0x00, 0x00, 0x00,              // Value (patched below).
      0x00, 0x00, 0x00, 0x00,              // No next IFD.
  };
  memcpy(out, kTemplate, kExifOrientationApp1Size);
  out[24] = orientation;
}

void MapUprightToStored(uint8_t orientation, size_t x, size_t y,
                        size_t stored_width, size_t stored_height,
                        size_t* stored_x, size_t* stored_y) {
  switch (orientation) {
    case 2:  // Mirrored horizontally.
      *stored_x = stored_width - 1 - x;
      *stored_y = y;
      break;
    case 3:  // Rotated 180 degrees.
      *stored_x = stored_width - 1 - x;
      *stored_y = stored_height - 1 - y;
      break;
    case 4:  // Mirrored vertically.
      *stored_x = x;
      *stored_y = stored_height - 1 - y;
      break;
    case 5:  // Transposed (mirrored along top-left diagonal).
      *stored_x = y;
      *stored_y = x;
      break;
    case 6:  // Stored rotated 90 CCW; display by rotating 90 CW.
      *stored_x = y;
      *stored_y = stored_height - 1 - x;
      break;
    case 7:  // Transverse (mirrored along top-right diagonal).
      *stored_x = stored_width - 1 - y;
      *stored_y = stored_height - 1 - x;
      break;
    case 8:  // Stored rotated 90 CW; display by rotating 90 CCW.
      *stored_x = stored_width - 1 - y;
      *stored_y = x;
      break;
    default:  // 1 or invalid: identity.
      *stored_x = x;
      *stored_y = y;
      break;
  }
}

bool ApplyExifOrientationToRaster(uint8_t orientation, const uint8_t* src,
                                  size_t width, size_t height,
                                  size_t components, uint8_t* dst) {
  if (orientation < 1 || orientation > 8 || src == nullptr || dst == nullptr ||
      width == 0 || height == 0 || components == 0) {
    return false;
  }
  if (orientation == 1) {
    memcpy(dst, src, width * height * components);
    return true;
  }
  const bool swaps = ExifOrientationSwapsDimensions(orientation);
  const size_t out_width = swaps ? height : width;
  const size_t out_height = swaps ? width : height;
  const size_t src_row_bytes = width * components;
  uint8_t* out = dst;
  for (size_t y = 0; y < out_height; ++y) {
    for (size_t x = 0; x < out_width; ++x, out += components) {
      size_t sx = 0;
      size_t sy = 0;
      MapUprightToStored(orientation, x, y, width, height, &sx, &sy);
      memcpy(out, src + sy * src_row_bytes + sx * components, components);
    }
  }
  return true;
}

ExifOrientedScanlineReader::ExifOrientedScanlineReader(
    ScanlineReaderInterface* inner, MessageHandler* handler)
    : inner_(inner), message_handler_(handler) {}

ExifOrientedScanlineReader::~ExifOrientedScanlineReader() = default;

bool ExifOrientedScanlineReader::Reset() {
  orientation_ = 1;
  stored_width_ = 0;
  stored_height_ = 0;
  out_width_ = 0;
  out_height_ = 0;
  components_ = 0;
  row_ = 0;
  buffered_ = false;
  stored_.clear();
  stored_.shrink_to_fit();
  out_row_.clear();
  out_row_.shrink_to_fit();
  return inner_->Reset();
}

ScanlineStatus ExifOrientedScanlineReader::InitializeWithStatus(
    const void* image_buffer, size_t buffer_length) {
  if (buffered_ || orientation_ != 1) {
    Reset();
  }
  ScanlineStatus status =
      inner_->InitializeWithStatus(image_buffer, buffer_length);
  if (!status.Success()) {
    return status;
  }
  orientation_ = ReadJpegExifOrientation(image_buffer, buffer_length);
  if (orientation_ == 1) {
    return status;  // Passthrough; no further state needed.
  }
  stored_width_ = inner_->GetImageWidth();
  stored_height_ = inner_->GetImageHeight();
  const size_t bytes_per_scanline = inner_->GetBytesPerScanline();
  components_ = stored_width_ != 0 ? bytes_per_scanline / stored_width_ : 0;
  if (components_ == 0 || components_ * stored_width_ != bytes_per_scanline) {
    // Padded or otherwise non-interleaved scanlines: cannot transform.
    // Serve pixels untransformed rather than failing outright.
    orientation_ = 1;
    return status;
  }
  const bool swaps = ExifOrientationSwapsDimensions(orientation_);
  out_width_ = swaps ? stored_height_ : stored_width_;
  out_height_ = swaps ? stored_width_ : stored_height_;
  return status;
}

size_t ExifOrientedScanlineReader::GetBytesPerScanline() {
  return orientation_ == 1 ? inner_->GetBytesPerScanline()
                           : out_width_ * components_;
}

bool ExifOrientedScanlineReader::HasMoreScanLines() {
  return orientation_ == 1 ? inner_->HasMoreScanLines() : row_ < out_height_;
}

size_t ExifOrientedScanlineReader::GetImageHeight() {
  return orientation_ == 1 ? inner_->GetImageHeight() : out_height_;
}

size_t ExifOrientedScanlineReader::GetImageWidth() {
  return orientation_ == 1 ? inner_->GetImageWidth() : out_width_;
}

ScanlineStatus ExifOrientedScanlineReader::BufferStoredRaster() {
  const uint64_t total =
      static_cast<uint64_t>(stored_width_) * stored_height_ * components_;
  if (total == 0 || total > OrientationBakeByteLimit()) {
    // Refuse to buffer an oversized raster. Callers fall back to their
    // serve-original paths, which keep the tag and render upright.
    return PS_LOGGED_STATUS(
        PS_LOG_INFO, message_handler_, SCANLINE_STATUS_UNSUPPORTED_FEATURE,
        SCANLINE_JPEGREADER,
        "raster too large to bake EXIF orientation (%llu bytes)",
        static_cast<unsigned long long>(total));
  }
  stored_.clear();  // A prior failed attempt may have left partial rows.
  stored_.reserve(static_cast<size_t>(total));
  const size_t row_bytes = stored_width_ * components_;
  while (inner_->HasMoreScanLines()) {
    void* scanline = nullptr;
    ScanlineStatus status = inner_->ReadNextScanlineWithStatus(&scanline);
    if (!status.Success()) {
      return status;
    }
    stored_.append(static_cast<const char*>(scanline), row_bytes);
  }
  if (stored_.size() != total) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler_,
                            SCANLINE_STATUS_INTERNAL_ERROR, SCANLINE_JPEGREADER,
                            "decoded raster size mismatch");
  }
  out_row_.resize(out_width_ * components_);
  buffered_ = true;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus ExifOrientedScanlineReader::ReadNextScanlineWithStatus(
    void** out_scanline_bytes) {
  if (orientation_ == 1) {
    return inner_->ReadNextScanlineWithStatus(out_scanline_bytes);
  }
  if (row_ >= out_height_) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler_,
                            SCANLINE_STATUS_INVOCATION_ERROR,
                            SCANLINE_JPEGREADER, "no more scanlines to read");
  }
  if (!buffered_) {
    ScanlineStatus status = BufferStoredRaster();
    if (!status.Success()) {
      return status;
    }
  }
  const uint8_t* src = reinterpret_cast<const uint8_t*>(stored_.data());
  uint8_t* out = reinterpret_cast<uint8_t*>(out_row_.data());
  const size_t src_row_bytes = stored_width_ * components_;
  for (size_t x = 0; x < out_width_; ++x) {
    size_t sx = 0;
    size_t sy = 0;
    MapUprightToStored(orientation_, x, row_, stored_width_, stored_height_,
                       &sx, &sy);
    memcpy(out + x * components_, src + sy * src_row_bytes + sx * components_,
           components_);
  }
  ++row_;
  *out_scanline_bytes = out_row_.data();
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

}  // namespace pagespeed::image_compression
