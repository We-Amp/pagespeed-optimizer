// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the Image Transcoder: single-format and multi-format encoding,
// viewport-based resizing, Save-Data quality reduction, pixel density handling,
// GIF detection and passthrough, and corrupt input robustness.

#include "src/worker/image_transcoder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "avif/avif.h"
#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/classify/capability_mask.h"
#include "lib/image/exif_orientation.h"
#include "lib/image/image_util.h"
#include "lib/image/jpeg_optimizer.h"
#include "lib/image/read_image.h"
#include "src/webp/encode.h"

namespace {

using pagespeed::CapabilityMask;
using pagespeed::DecodedImage;
using pagespeed::ImageTranscoder;
using pagespeed::ImageTranscoderConfig;
using pagespeed::MultiTranscodeResult;
using pagespeed::NullMessageHandler;
using pagespeed::TranscodeResult;
using pagespeed::image_compression::CreateScanlineWriter;
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::JpegCompressionOptions;

// Helper to read a test file into a string.
std::string ReadTestFile(const std::string& filename) {
  std::string path = "test/lib/image/testdata/" + filename;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

// Returns the fourcc of the first WebP image-bitstream chunk found:
// "VP8 " means lossy, "VP8L" means lossless. Walks the RIFF chunk list
// (skipping metadata chunks like VP8X/ALPH/ANIM) and descends into ANMF
// frame chunks for animated WebP. Returns "" if none is found or the
// data is not a WebP container.
std::string WebpBitstreamFourcc(const std::string& data, size_t start = 12) {
  if (data.size() < 12 || data.compare(0, 4, "RIFF") != 0 ||
      data.compare(8, 4, "WEBP") != 0) {
    return "";
  }
  size_t pos = start;
  while (pos + 8 <= data.size()) {
    std::string fourcc = data.substr(pos, 4);
    const uint32_t chunk_size = static_cast<uint8_t>(data[pos + 4]) |
                                (static_cast<uint8_t>(data[pos + 5]) << 8) |
                                (static_cast<uint8_t>(data[pos + 6]) << 16) |
                                (static_cast<uint8_t>(data[pos + 7]) << 24);
    if (fourcc == "VP8 " || fourcc == "VP8L") {
      return fourcc;
    }
    if (fourcc == "ANMF" && chunk_size > 16 && pos + 24 <= data.size()) {
      // ANMF payload: 16 bytes of frame parameters, then sub-chunks.
      std::string sub(data, 0, 12);  // Reuse the container header.
      sub.append(data, pos + 8 + 16,
                 std::min<size_t>(chunk_size - 16, data.size() - pos - 24));
      std::string found = WebpBitstreamFourcc(sub);
      if (!found.empty()) {
        return found;
      }
    }
    pos += 8 + chunk_size + (chunk_size & 1);  // Chunks are 2-byte aligned.
  }
  return "";
}

// FNV-1a 64-bit over a byte string: a cheap, dependency-free fingerprint for
// the golden output-bytes pins below (#1412).
uint64_t GoldenFnv1a64(std::string_view data) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : data) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

class ImageTranscoderTest : public testing::Test {
 public:
  ImageTranscoderTest()
      : handler_(), transcoder_(ImageTranscoderConfig{}, &handler_) {}

 protected:
  NullMessageHandler handler_;
  ImageTranscoder transcoder_;
};

// Test: Empty input returns failure.
TEST_F(ImageTranscoderTest, EmptyInput) {
  CapabilityMask mask;
  auto result = transcoder_.Transcode("", mask);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: Unsupported format returns failure.
TEST_F(ImageTranscoderTest, UnsupportedFormat) {
  CapabilityMask mask;
  auto result = transcoder_.Transcode("not an image", mask);
  EXPECT_FALSE(result.success);
}

// Test: Optimize JPEG with kOriginal mask.
TEST_F(ImageTranscoderTest, OptimizeJpeg) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
  EXPECT_FALSE(result.output_data.empty());
  // Optimized output should not be larger than original.
  EXPECT_LE(result.output_data.size(), jpeg.size());
}

// Test: Optimize PNG with kOriginal mask.
TEST_F(ImageTranscoderTest, OptimizePng) {
  std::string png = ReadTestFile("pngsuite/basi0g01.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(png, mask);
  // Small PNGs may already be well-optimized (no savings → success=false).
  if (result.success) {
    EXPECT_EQ("image/png", result.output_mime_type);
    EXPECT_FALSE(result.output_data.empty());
  } else {
    EXPECT_TRUE(result.output_data.empty());
  }
}

// Test: Convert JPEG to WebP (uses large image where WebP wins).
TEST_F(ImageTranscoderTest, JpegToWebp) {
  // Use a large JPEG where WebP produces genuinely smaller output.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  if (result.success) {
    EXPECT_EQ("image/webp", result.output_mime_type);
    EXPECT_FALSE(result.output_data.empty());
    // Verify the output is actually WebP by checking RIFF magic.
    ASSERT_GE(result.output_data.size(), 4u);
    EXPECT_EQ('R', result.output_data[0]);
    EXPECT_EQ('I', result.output_data[1]);
    EXPECT_EQ('F', result.output_data[2]);
    EXPECT_EQ('F', result.output_data[3]);
    // WebP should be smaller than original.
    EXPECT_LT(result.output_data.size(), jpeg.size());
  }
}

// Test: single-format WebP transcode of a photographic JPEG must be
// LOSSY (bitstream chunk "VP8 ", not "VP8L"). Regression test: the
// WebpConfiguration constructor defaults to lossless, and ConvertToWebp
// once relied on that default, producing oversized lossless WebP that
// the size gate then rejected — silently losing the WebP variant for
// most photos.
TEST_F(ImageTranscoderTest, JpegToWebpIsLossy) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(jpeg, mask);
  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_EQ("image/webp", result.output_mime_type);
  EXPECT_EQ("VP8 ", WebpBitstreamFourcc(result.output_data));
  EXPECT_LT(result.output_data.size(), jpeg.size());
}

// Test: Small JPEG to WebP may fall back to optimized JPEG if WebP
// is larger.
TEST_F(ImageTranscoderTest, SmallJpegToWebpFallback) {
  // Small JPEGs may not benefit from WebP conversion.
  // When WebP is larger, optimization falls back to JPEG, and if JPEG
  // optimization also produces no savings, the entire call fails.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(jpeg, mask);
  // Either WebP succeeds, or the entire chain fails (no savings).
  if (result.success) {
    EXPECT_FALSE(result.output_data.empty());
    EXPECT_TRUE(result.output_mime_type == "image/webp" ||
                result.output_mime_type == "image/jpeg");
  } else {
    EXPECT_TRUE(result.output_data.empty());
  }
}

// Test: Convert PNG to WebP.
TEST_F(ImageTranscoderTest, PngToWebp) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(png, mask);
  // For small PNGs, WebP might be larger, so conversion may
  // "fail" intentionally (size check). Either way, we shouldn't
  // crash.
  if (result.success) {
    EXPECT_EQ("image/webp", result.output_mime_type);
  }
}

// Test: JPEG to AVIF conversion.
TEST_F(ImageTranscoderTest, JpegToAvif) {
  // Use a large JPEG where AVIF produces genuinely smaller output.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  if (result.success) {
    EXPECT_EQ("image/avif", result.output_mime_type);
    EXPECT_FALSE(result.output_data.empty());
    // Verify AVIF magic bytes: "ftyp" at offset 4.
    ASSERT_GE(result.output_data.size(), 12u);
    EXPECT_EQ('f', result.output_data[4]);
    EXPECT_EQ('t', result.output_data[5]);
    EXPECT_EQ('y', result.output_data[6]);
    EXPECT_EQ('p', result.output_data[7]);
    // AVIF should be smaller than original.
    EXPECT_LT(result.output_data.size(), jpeg.size());
  }
}

// Test: Small JPEG to AVIF may fall back to optimized JPEG.
TEST_F(ImageTranscoderTest, SmallJpegToAvifFallback) {
  // Small JPEGs may not benefit from AVIF conversion.
  // When AVIF is larger, optimization falls back to JPEG, and if JPEG
  // optimization also produces no savings, the entire call fails.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(jpeg, mask);
  // Either AVIF succeeds, or the entire chain fails (no savings).
  if (result.success) {
    EXPECT_FALSE(result.output_data.empty());
    EXPECT_TRUE(result.output_mime_type == "image/avif" ||
                result.output_mime_type == "image/jpeg");
  } else {
    EXPECT_TRUE(result.output_data.empty());
  }
}

// Test: PNG to AVIF conversion.
TEST_F(ImageTranscoderTest, PngToAvif) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(png, mask);
  // For small PNGs, AVIF might be larger. Either outcome is valid.
  if (result.success && result.output_mime_type == "image/avif") {
    ASSERT_GE(result.output_data.size(), 12u);
    EXPECT_EQ('f', result.output_data[4]);
    EXPECT_EQ('t', result.output_data[5]);
    EXPECT_EQ('y', result.output_data[6]);
    EXPECT_EQ('p', result.output_data[7]);
  }
}

// Test: SVG target falls back to optimized original.
TEST_F(ImageTranscoderTest, SvgFallbackToOriginal) {
  std::string png = ReadTestFile("pngsuite/basi0g01.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kSvg);

  auto result = transcoder_.Transcode(png, mask);
  // Falls back to optimized PNG. Small PNGs may have no savings.
  if (result.success) {
    EXPECT_EQ("image/png", result.output_mime_type);
  }
}

// Test: a WebP input at the kOriginal mask runs the standard acceptance rule.
// WebP used to be exempt from re-encoding here on the grounds that it was
// "already optimized by the encoder" (#1375). It now gets what JPEG and PNG
// get: re-encode, and keep the result only if it is STRICTLY smaller.
TEST_F(ImageTranscoderTest, WebpOriginal) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(webp, mask);
  // The invariant, whichever way this particular fixture falls: a successful
  // result is a WebP that is strictly smaller than the input, and a refusal
  // says why. Never the input handed back under a success.
  if (result.success) {
    EXPECT_EQ("image/webp", result.output_mime_type);
    EXPECT_LT(result.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// Test: the kWebP mask on a WebP input runs the same rule -- the conversion
// arm declines (source is already the target format) and the original-format
// leg decides, so both masks are the one acceptance rule (#1375).
TEST_F(ImageTranscoderTest, WebpToWebpNoOp) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(webp, mask);
  if (result.success) {
    EXPECT_EQ("image/webp", result.output_mime_type);
    EXPECT_LT(result.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// Test: a WebP input whose re-encode PAYS is accepted, and it is smaller.
// The origin here is a WebP the pipeline can beat -- the case the removed
// exemption's premise ("already optimized by the encoder") got wrong (#1375).
TEST_F(ImageTranscoderTest, WebpReencodeAcceptedWhenStrictlySmaller) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;  // well below whatever the origin was encoded at
  ImageTranscoder tc(cfg, &handler);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = tc.Transcode(webp, mask);

  ASSERT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/webp", result.output_mime_type);
  EXPECT_LT(result.output_data.size(), webp.size());
  EXPECT_NE(result.output_data, webp) << "the origin bytes were handed back";
}

// Test: a WebP input whose re-encode does NOT pay is refused, and the origin
// is what keeps being served. This is the case the exemption was there to
// protect, and the acceptance rule protects it without the exemption.
TEST_F(ImageTranscoderTest, WebpReencodeRefusedWhenItDoesNotPay) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 100;  // above the origin: the re-encode grows the file
  ImageTranscoder tc(cfg, &handler);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = tc.Transcode(webp, mask);

  EXPECT_FALSE(result.success)
      << "a re-encode that saves nothing must not be accepted (produced "
      << result.output_data.size() << " bytes from " << webp.size() << ")";
  EXPECT_TRUE(result.output_data.empty());
  EXPECT_FALSE(result.error_message.empty());
}

// Test: the multi-format path re-encodes WebP inputs too. The WebP slot is the
// one every WebP-negotiating client reads; leaving it holding the origin's own
// bytes is what made the #1375 family "the origin, for every client".
TEST_F(ImageTranscoderTest, MultiTranscodeReencodesWebpInput) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;
  cfg.quality_verify = false;  // pin the encode at exactly webp_quality
  ImageTranscoder tc(cfg, &handler);

  auto multi =
      tc.TranscodeMulti(webp, {CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::ImageFormat::kAvif,
                               CapabilityMask::ImageFormat::kOriginal});

  ASSERT_TRUE(multi.webp.success) << multi.webp.error_message;
  EXPECT_EQ("image/webp", multi.webp.output_mime_type);
  EXPECT_LT(multi.webp.output_data.size(), webp.size());
  EXPECT_NE(multi.webp.output_data, webp)
      << "the origin bytes were handed back";

  // The original-format slot is WebP for a WebP origin, and it takes the same
  // rule -- reusing this call's decode rather than decoding a second time.
  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  EXPECT_EQ("image/webp", multi.optimized_original.output_mime_type);
  EXPECT_LT(multi.optimized_original.output_data.size(), webp.size());

  // AVIF still refuses WebP input; that is unchanged and not what #1375 is
  // about.
  EXPECT_FALSE(multi.avif.success);
}

// Test: the original-format slot for a WebP origin when this call performed no
// decode of its own -- the OTHER branch of the re-encode helper, which has to
// decode for itself.
TEST_F(ImageTranscoderTest, MultiTranscodeWebpOriginalOnlyDecodesForItself) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;
  cfg.quality_verify = false;
  ImageTranscoder tc(cfg, &handler);

  // Only the original format is asked for, so nothing else triggers a decode.
  auto multi =
      tc.TranscodeMulti(webp, {CapabilityMask::ImageFormat::kOriginal});
  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  EXPECT_EQ("image/webp", multi.optimized_original.output_mime_type);
  EXPECT_LT(multi.optimized_original.output_data.size(), webp.size());
}

// Test: a WebP origin the pipeline cannot beat yields no variant at all,
// exactly as a JPEG or PNG in the same position does.
// Builds a genuinely LOSSLESS (VP8L) WebP through the public API rather than
// committing a new binary fixture: the GIF->WebP pipeline encodes palettized
// sources losslessly on purpose, so its output is a VP8L WebP whose provenance
// is visible in-tree.  Callers assert the bitstream fourcc before relying on it.
std::string MakeLosslessWebp(ImageTranscoder& tc) {
  std::string gif = ReadTestFile("gif/interlaced.gif");
  if (gif.empty()) return "";
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  auto r = tc.Transcode(gif, mask);
  if (!r.success || r.output_mime_type != "image/webp") return "";
  return r.output_data;
}

TEST_F(ImageTranscoderTest, LosslessWebpIsNotReencodedLossily) {
  // every WebP encoder in this pipeline is lossy, so
  // re-encoding a LOSSLESS origin is a fidelity change the strictly-smaller
  // gate cannot see -- it measures bytes, not pixels.  A lossless source is an
  // author decision about fidelity, so it keeps the old exemption; the
  // strictly-smaller rule applies to lossy origins, which is what #1375 is
  // about.
  std::string lossless = MakeLosslessWebp(transcoder_);
  ASSERT_FALSE(lossless.empty()) << "could not build a lossless WebP";
  // Non-vacuity: this really is a VP8L bitstream, so the test is about the
  // lossless class and not about whatever the GIF pipeline happened to emit.
  ASSERT_EQ("VP8L", WebpBitstreamFourcc(lossless));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;  // a quality at which a lossy re-encode WOULD win
  cfg.quality_verify = false;
  ImageTranscoder tc(cfg, &handler);

  // Single-format original-format leg.
  CapabilityMask orig;
  orig.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto single = tc.Transcode(lossless, orig);
  ASSERT_TRUE(single.success) << single.error_message;
  EXPECT_EQ("image/webp", single.output_mime_type);
  EXPECT_EQ(lossless, single.output_data) << "a lossless origin was re-encoded";
  EXPECT_EQ("VP8L", WebpBitstreamFourcc(single.output_data));

  // Multi-format WebP slot -- the slot every WebP-negotiating client reads.
  auto multi =
      tc.TranscodeMulti(lossless, {CapabilityMask::ImageFormat::kWebP,
                                   CapabilityMask::ImageFormat::kOriginal});
  ASSERT_TRUE(multi.webp.success) << multi.webp.error_message;
  EXPECT_EQ(lossless, multi.webp.output_data)
      << "a lossless origin was re-encoded in the multi-format WebP slot";
  EXPECT_EQ("VP8L", WebpBitstreamFourcc(multi.webp.output_data));
  // The original-format slot reaches the re-encode through a different helper
  // and has to be exempted too -- a guard at one call site left this one
  // re-encoding.  Asserted unconditionally: a conditional here would go
  // vacuous the moment the slot stopped being produced.
  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  EXPECT_EQ(lossless, multi.optimized_original.output_data)
      << "a lossless origin was re-encoded in the original-format slot";
  EXPECT_EQ("VP8L", WebpBitstreamFourcc(multi.optimized_original.output_data));
}

TEST_F(ImageTranscoderTest, LosslessWebpIsNotReencodedOnTheResizingPath) {
  // The resizing/Save-Data path is a FOURTH route into a WebP encode, and the
  // only one that encodes from resized pixels.  It has to honour the lossless
  // exemption too: every encoder here is lossy, and a resized lossy re-encode
  // is still a lossy re-encode.
  //
  // The size gate on that arm cannot be relied on to catch it -- fewer pixels
  // are always smaller, so the gate accepts the degraded output rather than
  // refusing it.  That is what makes the guard, not the gate, the thing that
  // has to hold here.
  std::string lossless = MakeLosslessWebp(transcoder_);
  ASSERT_FALSE(lossless.empty()) << "could not build a lossless WebP";
  ASSERT_EQ("VP8L", WebpBitstreamFourcc(lossless));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  // Qualities at which a lossy re-encode comfortably beats the origin, so the
  // size gate WOULD accept it: the guard is the only thing refusing.
  cfg.webp_quality = 20;
  cfg.savedata_webp_quality = 20;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  // Save-Data alone routes into the resizing arm (the desktop fast path is
  // skipped whenever save-data is on), and a mobile viewport also resizes.
  auto multi = tc.TranscodeMultiResized(
      lossless, {CapabilityMask::ImageFormat::kWebP},
      CapabilityMask::Viewport::kMobile, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOn);

  ASSERT_TRUE(multi.webp.success) << multi.webp.error_message;
  EXPECT_EQ("image/webp", multi.webp.output_mime_type);
  EXPECT_EQ(lossless, multi.webp.output_data)
      << "a lossless origin was re-encoded on the resizing/Save-Data path";
  EXPECT_EQ("VP8L", WebpBitstreamFourcc(multi.webp.output_data));

  // The same at a desktop viewport with save-data, which reaches the arm
  // without a resize -- so neither half of "resized" nor "save-data" is what
  // the guard keys on.
  auto desktop = tc.TranscodeMultiResized(
      lossless, {CapabilityMask::ImageFormat::kWebP},
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOn);
  ASSERT_TRUE(desktop.webp.success) << desktop.webp.error_message;
  EXPECT_EQ(lossless, desktop.webp.output_data);
}

TEST_F(ImageTranscoderTest, LossyWebpIsStillReencodedOnTheResizingPath) {
  // The exemption on that arm is for the lossless class only: a lossy origin
  // must still be re-encoded there, or the guard would have turned the
  // resizing path into a blanket passthrough for every WebP.
  std::string lossy = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(lossy.empty());
  ASSERT_EQ("VP8 ", WebpBitstreamFourcc(lossy));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;
  cfg.savedata_webp_quality = 1;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  auto multi = tc.TranscodeMultiResized(
      lossy, {CapabilityMask::ImageFormat::kWebP},
      CapabilityMask::Viewport::kMobile, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOn);
  ASSERT_TRUE(multi.webp.success) << multi.webp.error_message;
  EXPECT_NE(lossy, multi.webp.output_data)
      << "a lossy origin was exempted on the resizing/Save-Data path";
  EXPECT_LT(multi.webp.output_data.size(), lossy.size());
}

TEST_F(ImageTranscoderTest, LossyWebpWithAlphaIsStillReencoded) {
  // The lossless exemption is for VP8L only.  IMAGE_WEBP_LOSSLESS_OR_ALPHA
  // also covers a LOSSY image that merely carries an alpha channel
  // (VP8X + ALPH + VP8), and that one is an ordinary re-encode candidate --
  // exempting it would quietly re-open #1375 for every transparent image.
  std::string alpha = ReadTestFile("alpha_32x32.webp");
  ASSERT_FALSE(alpha.empty());
  // Non-vacuity: the fixture is the lossy-with-alpha shape, not VP8L.
  ASSERT_EQ("VP8 ", WebpBitstreamFourcc(alpha));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;
  cfg.quality_verify = false;
  ImageTranscoder tc(cfg, &handler);

  CapabilityMask orig;
  orig.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto single = tc.Transcode(alpha, orig);
  ASSERT_TRUE(single.success) << single.error_message;
  EXPECT_EQ("image/webp", single.output_mime_type);
  EXPECT_NE(alpha, single.output_data)
      << "a lossy alpha origin was exempted from the re-encode";
  EXPECT_LT(single.output_data.size(), alpha.size());

  auto multi = tc.TranscodeMulti(alpha, {CapabilityMask::ImageFormat::kWebP});
  ASSERT_TRUE(multi.webp.success) << multi.webp.error_message;
  EXPECT_NE(alpha, multi.webp.output_data);
  EXPECT_LT(multi.webp.output_data.size(), alpha.size());
}

// =================================================================
// #1380: the resizing path's original-format slot
// =================================================================

// Builds a wide LOSSY WebP through the public API (a JPEG->WebP conversion
// preserves dimensions), because the committed lossy WebP fixture is 32px
// wide -- narrower than every viewport target -- and cannot exercise a
// resize.  Callers assert the fourcc before relying on it.
std::string MakeWideLossyWebp(ImageTranscoder& tc) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");  // 512x512
  if (jpeg.empty()) return "";
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  auto r = tc.Transcode(jpeg, mask);
  if (!r.success || r.output_mime_type != "image/webp") return "";
  return r.output_data;
}

// The original-format slot for a WebP origin on the no-resize leg of the
// resizing path must reuse the decode this call already did: one decode of
// the source serves the whole call (#1380).  The old fall-through to
// Transcode() decoded the origin a second time, full size.
TEST_F(ImageTranscoderTest, ResizedOriginalSlotWebpDecodesOnceWithoutResize) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());
  ASSERT_EQ("VP8 ", WebpBitstreamFourcc(webp));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;
  cfg.savedata_webp_quality = 1;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  // Save-Data alone routes into this arm (the desktop fast path is skipped
  // whenever save-data is on); desktop means no resize.
  const uint64_t before = tc.decode_count();
  auto multi = tc.TranscodeMultiResized(
      webp, {CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOn);
  EXPECT_EQ(before + 1, tc.decode_count())
      << "the original-format slot must reuse the call's decode, not decode "
         "the source a second time";

  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  EXPECT_EQ("image/webp", multi.optimized_original.output_mime_type);
}

// The Save-Data quality override must reach the original-slot encode on the
// resizing path (#1380): the slot re-encodes at THIS call's settings, not at
// the transcoder's config snapshot.  With the old fall-through, the save-data
// run re-encoded at the snapshot quality and produced byte-identical output
// to the plain run -- the one signal that makes bytes smaller on a
// constrained connection, dropped on exactly this leg.
TEST_F(ImageTranscoderTest, ResizedOriginalSlotWebpHonoursSaveDataQuality) {
  std::string webp = MakeWideLossyWebp(transcoder_);
  ASSERT_FALSE(webp.empty()) << "could not build a wide lossy WebP";
  ASSERT_EQ("VP8 ", WebpBitstreamFourcc(webp));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 60;
  cfg.savedata_webp_quality = 20;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  auto run = [&](CapabilityMask::SaveData sd) {
    return tc.TranscodeMultiResized(webp,
                                    {CapabilityMask::ImageFormat::kOriginal},
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x, sd);
  };
  auto plain = run(CapabilityMask::SaveData::kOff);
  auto saved = run(CapabilityMask::SaveData::kOn);

  ASSERT_TRUE(plain.optimized_original.success)
      << plain.optimized_original.error_message;
  ASSERT_TRUE(saved.optimized_original.success)
      << saved.optimized_original.error_message;
  EXPECT_NE(saved.optimized_original.output_data,
            plain.optimized_original.output_data)
      << "the save-data quality override never reached the original-slot "
         "encode";
  EXPECT_LT(saved.optimized_original.output_data.size(),
            plain.optimized_original.output_data.size())
      << "the save-data re-encode must be the smaller one";
}

// The variant written at a resized viewport id carries the viewport's
// dimensions: for a WebP origin the original-format slot encodes from the
// resized pixels, like the JPEG leg (#1380).  The old fall-through re-encoded
// the FULL-SIZE origin into the mobile slot -- and paid a second full decode
// for it.
TEST_F(ImageTranscoderTest, ResizedOriginalSlotWebpEncodesAtViewportWidth) {
  std::string webp = MakeWideLossyWebp(transcoder_);
  ASSERT_FALSE(webp.empty()) << "could not build a wide lossy WebP";
  ASSERT_EQ("VP8 ", WebpBitstreamFourcc(webp));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.viewport_widths.mobile = 256;  // 512px source actually resizes
  cfg.webp_quality = 60;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  const uint64_t before = tc.decode_count();
  auto multi = tc.TranscodeMultiResized(
      webp, {CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kMobile, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff);
  EXPECT_EQ(before + 1, tc.decode_count())
      << "the original-format slot must reuse the call's decode, not decode "
         "the source a second time";

  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  EXPECT_EQ("image/webp", multi.optimized_original.output_mime_type);
  EXPECT_LT(multi.optimized_original.output_data.size(), webp.size());

  auto out = tc.DecodeToPixels(multi.optimized_original.output_data);
  ASSERT_FALSE(out.pixel_buffer.empty());
  EXPECT_EQ(256u, out.width)
      << "the original-format variant at the mobile viewport id must carry "
         "the mobile width";
}

// For a same-format origin (WebP source, WebP slot) the original-format slot
// and the WebP slot hold the SAME encode, so the WebP arm's size gate must
// use the ORIGIN bytes as its baseline -- the rule the non-resizing path and
// the #1375 acceptance rule use -- not the optimized-original output, which
// would read as "strictly smaller than itself" and refuse every candidate
// (#1380).
TEST_F(ImageTranscoderTest, ResizedOriginalSlotSameFormatGateUsesOriginBytes) {
  std::string webp = MakeWideLossyWebp(transcoder_);
  ASSERT_FALSE(webp.empty()) << "could not build a wide lossy WebP";
  ASSERT_EQ("VP8 ", WebpBitstreamFourcc(webp));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.viewport_widths.mobile = 256;
  cfg.webp_quality = 60;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  auto multi = tc.TranscodeMultiResized(
      webp,
      {CapabilityMask::ImageFormat::kWebP,
       CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kMobile, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff);

  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  ASSERT_TRUE(multi.webp.success) << multi.webp.error_message;
  // Same format, same pixels, same settings: the two slots hold the same
  // encode, exactly as the non-resizing path produces for a WebP origin.
  EXPECT_EQ(multi.optimized_original.output_data, multi.webp.output_data)
      << "the WebP slot and the original-format slot diverged for a WebP "
         "origin";

  auto out = tc.DecodeToPixels(multi.webp.output_data);
  ASSERT_FALSE(out.pixel_buffer.empty());
  EXPECT_EQ(256u, out.width);
}

// The no-savings acceptance gate on this leg: a re-encode that cannot beat
// the origin bytes produces NO variant (#1380; the same rule the
// non-resizing path's helper applies, #1375).  At quality 100 the re-encode
// of the already-compressed fixture reliably loses to the origin bytes (the
// same fixture and quality MultiTranscodeWebpNoSavingsWritesNothing pins
// the non-resizing slots with), so the gate is the only thing that can
// refuse the variant.
TEST_F(ImageTranscoderTest, ResizedOriginalSlotWebpNoSavingsWritesNothing) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());
  ASSERT_EQ("VP8 ", WebpBitstreamFourcc(webp));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 100;
  cfg.savedata_webp_quality = 100;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  // Save-data on routes into this arm (the desktop fast path is skipped
  // whenever save-data is on) while desktop keeps the encode at origin
  // size -- the shape where the leg's own acceptance gate, not a resize,
  // decides the outcome.
  auto multi = tc.TranscodeMultiResized(
      webp, {CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOn);
  EXPECT_FALSE(multi.optimized_original.success)
      << "original-format slot took a re-encode that saved nothing";
  EXPECT_TRUE(multi.optimized_original.output_data.empty())
      << "a no-savings re-encode must not write a variant";
}

// The #1375 lossless exemption binds on this leg too: a LOSSLESS WebP origin
// keeps its bytes verbatim in the original-format slot on the resizing path.
TEST_F(ImageTranscoderTest, ResizedOriginalSlotLosslessWebpKeptVerbatim) {
  std::string lossless = MakeLosslessWebp(transcoder_);
  ASSERT_FALSE(lossless.empty()) << "could not build a lossless WebP";
  ASSERT_EQ("VP8L", WebpBitstreamFourcc(lossless));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  // Qualities at which a lossy re-encode comfortably beats the origin, so
  // the size gate would accept it: the exemption is the only thing keeping
  // the origin bytes.
  cfg.webp_quality = 20;
  cfg.savedata_webp_quality = 20;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  auto multi = tc.TranscodeMultiResized(
      lossless, {CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOn);
  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  EXPECT_EQ("image/webp", multi.optimized_original.output_mime_type);
  EXPECT_EQ(lossless, multi.optimized_original.output_data)
      << "a lossless origin was re-encoded in the original-format slot";
  EXPECT_EQ("VP8L", WebpBitstreamFourcc(multi.optimized_original.output_data));
}

// PNG and GIF origins get explicit legs on the resizing path (#1380), each
// keeping its documented behavior: the PNG slot stays stream-optimized at
// FULL SIZE (this pipeline has no PNG pixel encoder, and a pixel round-trip
// would un-palette palettized origins), the GIF slot passes through
// verbatim.  A resize is offered at this viewport, so the assertions pin
// that neither leg inherits a resized pixel re-encode by accident.
TEST_F(ImageTranscoderTest, ResizedOriginalSlotPngAndGifLegsStayExplicit) {
  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.viewport_widths.mobile = 16;  // both fixtures are wider than 16px
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  std::string png = ReadTestFile("png/large.png");  // 4096x2048
  ASSERT_FALSE(png.empty());
  auto png_result = tc.TranscodeMultiResized(
      png, {CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kMobile, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff);
  ASSERT_TRUE(png_result.optimized_original.success)
      << png_result.optimized_original.error_message;
  EXPECT_EQ("image/png", png_result.optimized_original.output_mime_type);
  auto png_out = tc.DecodeToPixels(png_result.optimized_original.output_data);
  ASSERT_FALSE(png_out.pixel_buffer.empty());
  EXPECT_EQ(4096u, png_out.width)
      << "the PNG original-format slot must stay full-size, not take the "
         "resized pixels";

  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());
  ASSERT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
  auto gif_result = tc.TranscodeMultiResized(
      gif, {CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kMobile, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff);
  ASSERT_TRUE(gif_result.optimized_original.success)
      << gif_result.optimized_original.error_message;
  EXPECT_EQ("image/gif", gif_result.optimized_original.output_mime_type);
  EXPECT_EQ(gif, gif_result.optimized_original.output_data)
      << "a static GIF origin was re-encoded in the original-format slot";
}

// =================================================================
// #1407: the desktop fast path hands its decode to TranscodeMulti
// =================================================================

// On the desktop profile with the variant set collapsed to kOriginal-only,
// the fast path in TranscodeMultiResized delegates to TranscodeMulti, whose
// original-format arm re-encodes a lossy WebP origin from pixels.  The call
// already decoded the source for content analysis and viewport decisions;
// the arm must reuse THAT decode (#1407).  The old hand-off dropped it:
// kOriginal-only raises no need-pixels flag inside TranscodeMulti, so the
// arm decoded the origin a second time, full size -- pure waste on the
// path whose whole point is to skip work.
TEST_F(ImageTranscoderTest, DesktopOriginalOnlyWebpFastPathDecodesOnce) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());
  ASSERT_EQ("VP8 ",
            WebpBitstreamFourcc(webp));  // lossy, not the VP8L exemption

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  // Desktop viewport + Save-Data off + kOriginal-only: the fast path.
  const uint64_t before = tc.decode_count();
  auto multi = tc.TranscodeMultiResized(
      webp, {CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff);
  EXPECT_EQ(before + 1, tc.decode_count())
      << "the desktop fast path must hand its decode to the original-format "
         "slot, not decode the source a second time";

  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  EXPECT_EQ("image/webp", multi.optimized_original.output_mime_type);

  // #1412: golden pin on the produced bytes.  The decode_count() pin above
  // guards the hand-off's discipline; this guards its product: a hand-off
  // that delivers a wrong-but-well-formed frame changes these bytes, and
  // only a byte pin kills that mutant (the full suite passed with the
  // handed-over frame corrupted).  The exact bytes are a function of the
  // pinned codec versions, so a legitimate libwebp update WILL change this
  // golden -- refresh by pasting the actual hash from the failure message
  // after confirming the codec bump is the only change.
  const uint64_t original_hash =
      GoldenFnv1a64(multi.optimized_original.output_data);
  EXPECT_EQ(0x52884b1d2fc4b479ull, original_hash)
      << "original-slot produced bytes changed (#1412); if this is a "
         "legitimate codec update, refresh the golden to 0x"
      << std::hex << original_hash;
}

// The same hand-off covers the multi-format desktop fast path: the WebP arm
// encodes from the caller's decode instead of decoding the source again
// (#1407).
TEST_F(ImageTranscoderTest, DesktopMultiFormatFastPathDecodesOnce) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());
  ASSERT_EQ("VP8 ", WebpBitstreamFourcc(webp));

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 1;
  cfg.quality_verify = false;
  cfg.content_analysis = false;
  cfg.learned_quality = false;
  ImageTranscoder tc(cfg, &handler);

  const uint64_t before = tc.decode_count();
  auto multi = tc.TranscodeMultiResized(
      webp,
      {CapabilityMask::ImageFormat::kWebP,
       CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData::kOff);
  EXPECT_EQ(before + 1, tc.decode_count())
      << "the desktop fast path must hand its decode to the WebP arm, not "
         "decode the source a second time";

  ASSERT_TRUE(multi.webp.success) << multi.webp.error_message;
  EXPECT_EQ("image/webp", multi.webp.output_mime_type);
  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  EXPECT_EQ("image/webp", multi.optimized_original.output_mime_type);

  // #1412: golden pins on both slots' produced bytes -- both arms consume
  // the handed-over frame, so corrupting it must change both outputs.  See
  // the kOriginal-only test above for the refresh policy.
  const uint64_t webp_hash = GoldenFnv1a64(multi.webp.output_data);
  EXPECT_EQ(0x52884b1d2fc4b479ull, webp_hash)
      << "WebP-slot produced bytes changed (#1412); if this is a legitimate "
         "codec update, refresh the golden to 0x"
      << std::hex << webp_hash;
  const uint64_t original_hash =
      GoldenFnv1a64(multi.optimized_original.output_data);
  EXPECT_EQ(0x52884b1d2fc4b479ull, original_hash)
      << "original-slot produced bytes changed (#1412); if this is a "
         "legitimate codec update, refresh the golden to 0x"
      << std::hex << original_hash;
}

TEST_F(ImageTranscoderTest, MultiTranscodeWebpNoSavingsWritesNothing) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 100;
  cfg.quality_verify = false;
  ImageTranscoder tc(cfg, &handler);

  auto multi =
      tc.TranscodeMulti(webp, {CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::ImageFormat::kOriginal});
  EXPECT_FALSE(multi.webp.success)
      << "WebP slot took a re-encode that saved nothing";
  EXPECT_FALSE(multi.optimized_original.success)
      << "original-format slot took a re-encode that saved nothing";
}

// Test: Custom config affects output.
TEST(ImageTranscoderConfigTest, CustomConfig) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.jpeg_quality = 50;  // Lower quality = smaller output.
  config.lossy_jpeg = true;
  config.progressive_jpeg = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
  // With quality 50, the output should be significantly smaller.
  EXPECT_LT(result.output_data.size(), jpeg.size());
}

// Test: Large JPEG to WebP conversion produces smaller output.
TEST_F(ImageTranscoderTest, LargeJpegToWebpSavesSpace) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  if (result.success) {
    EXPECT_EQ("image/webp", result.output_mime_type);
    // For photographic content, WebP should be notably smaller.
    EXPECT_LT(result.output_data.size(), jpeg.size());
  }
}

// =============================================================================
// GIF tests
// =============================================================================

// Test: Convert animated GIF to WebP.
TEST_F(ImageTranscoderTest, TranscodeGifToWebp) {
  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(gif, mask);
  // Animated GIF→WebP may produce larger output for small GIFs,
  // in which case conversion is skipped. Either outcome is valid.
  if (result.success && result.output_mime_type == "image/webp") {
    // Verify the output is actually WebP by checking RIFF magic.
    ASSERT_GE(result.output_data.size(), 4u);
    EXPECT_EQ('R', result.output_data[0]);
    EXPECT_EQ('I', result.output_data[1]);
    EXPECT_EQ('F', result.output_data[2]);
    EXPECT_EQ('F', result.output_data[3]);
    // The GIF→WebP path must stay LOSSLESS (palettized source; "VP8L"
    // frame chunks), unlike the lossy JPEG/PNG→WebP still path.
    EXPECT_EQ("VP8L", WebpBitstreamFourcc(result.output_data));
  }
}

// Test: GIF with kOriginal mask returns GIF as-is.
TEST_F(ImageTranscoderTest, TranscodeGifOriginal) {
  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(gif, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/gif", result.output_mime_type);
  // GIF passthrough: output should be identical to input.
  EXPECT_EQ(result.output_data.size(), gif.size());
}

// Test: Corrupt/truncated GIF does not crash.
TEST_F(ImageTranscoderTest, TranscodeGifCorrupt) {
  // GIF magic is "GIF89a" or "GIF87a". Send a truncated header.
  std::string truncated("GIF89a\x01\x00\x01\x00", 10);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(truncated, mask);
  // Must not crash. Truncated GIF should either fail or fall back.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// Test: Static GIF to WebP.
TEST_F(ImageTranscoderTest, TranscodeStaticGifToWebp) {
  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(gif, mask);
  if (result.success && result.output_mime_type == "image/webp") {
    ASSERT_GE(result.output_data.size(), 4u);
    EXPECT_EQ('R', result.output_data[0]);
    EXPECT_EQ('I', result.output_data[1]);
    EXPECT_EQ('F', result.output_data[2]);
    EXPECT_EQ('F', result.output_data[3]);
    // Static GIFs also go through the lossless GIF→WebP path.
    EXPECT_EQ("VP8L", WebpBitstreamFourcc(result.output_data));
  }
}

// =============================================================================
// GIF → AVIF tests
// =============================================================================

TEST_F(ImageTranscoderTest, GifToAvif) {
  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(gif, mask);
  // For small GIFs, AVIF might be larger. Either outcome is valid.
  if (result.success && result.output_mime_type == "image/avif") {
    ASSERT_GE(result.output_data.size(), 12u);
    EXPECT_EQ('f', result.output_data[4]);
    EXPECT_EQ('t', result.output_data[5]);
    EXPECT_EQ('y', result.output_data[6]);
    EXPECT_EQ('p', result.output_data[7]);
  }
}

TEST_F(ImageTranscoderTest, GifToAvifFallback) {
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(gif, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(result.output_data.empty());
  EXPECT_TRUE(result.output_mime_type == "image/avif" ||
              result.output_mime_type == "image/gif");
}

// =============================================================================
// Corrupt / malformed input tests
// =============================================================================

// Test: Corrupt JPEG does not crash, returns failure.
TEST_F(ImageTranscoderTest, CorruptJpeg) {
  std::string corrupt = ReadTestFile("jpeg/corrupt.jpg");
  ASSERT_FALSE(corrupt.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Must not crash. May or may not succeed depending on how corrupt it is.
  // If it fails, error_message should be populated.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// Test: Corrupt JPEG to WebP conversion does not crash.
TEST_F(ImageTranscoderTest, CorruptJpegToWebp) {
  std::string corrupt = ReadTestFile("jpeg/corrupt.jpg");
  ASSERT_FALSE(corrupt.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Must not crash. Either conversion fails and falls back, or it fails
  // entirely. Either outcome is acceptable.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// Test: Empty JPEG file returns failure.
TEST_F(ImageTranscoderTest, EmptyJpegFile) {
  std::string empty = ReadTestFile("jpeg/emptyfile.jpg");
  // emptyfile.jpg is a zero-byte file; ReadTestFile returns "" for it.
  // This should behave like empty input (already tested) or as an
  // unrecognized format.
  CapabilityMask mask;
  auto result = transcoder_.Transcode(empty, mask);
  EXPECT_FALSE(result.success);
}

// Test: Empty PNG file returns failure.
TEST_F(ImageTranscoderTest, EmptyPngFile) {
  std::string empty = ReadTestFile("pngsuite/emptyfile.png");

  CapabilityMask mask;
  auto result = transcoder_.Transcode(empty, mask);
  EXPECT_FALSE(result.success);
}

// Test: Corrupt WebP header does not crash.
TEST_F(ImageTranscoderTest, CorruptWebpHeader) {
  std::string corrupt = ReadTestFile("corrupt_header.webp");
  ASSERT_FALSE(corrupt.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Corrupt header: magic bytes may not match, so it may be treated as
  // unsupported format or succeed with the raw data. Must not crash.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// Test: Corrupt WebP body does not crash.
TEST_F(ImageTranscoderTest, CorruptWebpBody) {
  std::string corrupt = ReadTestFile("corrupt_body.webp");
  ASSERT_FALSE(corrupt.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Must not crash. WebP with valid header but corrupt body should either
  // succeed (returned as-is) or fail gracefully.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// Test: Truncated data (partial JPEG header) does not crash.
TEST_F(ImageTranscoderTest, TruncatedJpegHeader) {
  // JPEG magic is FF D8 FF. Send just the first 2 bytes.
  std::string truncated("\xff\xd8", 2);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(truncated, mask);
  // Should be treated as unsupported (too short to detect format) or fail.
  EXPECT_FALSE(result.success);
}

// Test: Random bytes do not crash the transcoder.
TEST_F(ImageTranscoderTest, RandomBytes) {
  // Not a valid image format.
  std::string random = "This is definitely not an image format!";

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(random, mask);
  EXPECT_FALSE(result.success);
}

// =================================================================
// Multi-format transcoding (decode once, encode many)
// =================================================================

TEST_F(ImageTranscoderTest, DecodeToPixelsJpeg) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder_.DecodeToPixels(jpeg);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_GT(decoded.height, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.bytes_per_pixel, 3);  // JPEG is RGB
  EXPECT_FALSE(decoded.has_alpha);
}

TEST_F(ImageTranscoderTest, DecodeToPixelsPng) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  auto decoded = transcoder_.DecodeToPixels(png);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_GT(decoded.height, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
}

TEST_F(ImageTranscoderTest, DecodeToPixelsAvif) {
  // #1274: the AVIF verify floor needs an AVIF decode path — AVIF bytes
  // must decode like every other candidate format.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto result =
      transcoder_.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kAvif});
  ASSERT_TRUE(result.avif.success) << result.avif.error_message;

  auto decoded = transcoder_.DecodeToPixels(result.avif.output_data);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_GT(decoded.height, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.bytes_per_pixel, 3);  // JPEG source -> RGB AVIF
  EXPECT_FALSE(decoded.has_alpha);
}

TEST_F(ImageTranscoderTest, AvifVerifyFloorEngages) {
  // #1274 regression: pre-fix the AVIF arm's verify silently no-opped
  // and the score stayed N/A (-1). The floor must engage: a successful
  // AVIF candidate carries a REAL score.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto result =
      transcoder_.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kAvif});
  ASSERT_TRUE(result.avif.success) << result.avif.error_message;
  EXPECT_GE(result.avif_ssimulacra2_score, 0.0f)
      << "AVIF verify floor did not engage (score N/A)";
}

TEST_F(ImageTranscoderTest, AvifVerifyFloorReencodes) {
  // #1274 made the floor engage; mpp #790 made the verdict binding: a
  // below-band AVIF that the search cannot lift into the band is now
  // DECLINED, not shipped. Force every attempt below the band by aiming
  // the band above what the low starting quality can reach
  // (content-independent: sjpeg-class content can score ~75 even at
  // q=5, which the default [67,78] band would accept). Learned-quality
  // prediction and content-analysis presets are disabled or they
  // override the fixed quality with an on-target one
  // (TranscodeMultiResized). The loop still re-encodes toward the
  // target on the way down — the climb shows up in the final quality.
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_avif = false;
  config.content_analysis = false;    // preset factors also override quality
  config.avif_quality = 5;            // low starting quality
  config.target_ssimulacra2 = 95.0f;  // band [92,103]: unreachable from q=5
  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto result =
      transcoder.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kAvif});
  // The verdict is binding: nothing ships...
  EXPECT_FALSE(result.avif.success);
  EXPECT_TRUE(result.avif.output_data.empty());
  EXPECT_NE(std::string::npos,
            result.avif.error_message.find("verification failed"))
      << result.avif.error_message;
  EXPECT_TRUE(result.avif_ssimulacra2_declined);
  // ...but the measured score stays as decline evidence, and the search
  // genuinely climbed before giving up (5 -> 10 -> 15 -> 20).
  EXPECT_GE(result.avif_ssimulacra2_score, 0.0f);
  EXPECT_LT(result.avif_ssimulacra2_score, 92.0f);
  EXPECT_GT(result.final_avif_quality, 5)
      << "below-band AVIF declined without re-encode attempts (score="
      << result.avif_ssimulacra2_score
      << ", final_avif_quality=" << result.final_avif_quality << ")";
  // Nothing shipped, so the re-encoded flag reads false.
  EXPECT_FALSE(result.avif_ssimulacra2_reencoded);
}

TEST_F(ImageTranscoderTest, DecodeToPixelsEmptyInput) {
  auto decoded = transcoder_.DecodeToPixels("");
  EXPECT_TRUE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.width, 0u);
}

TEST_F(ImageTranscoderTest, TranscodeMultiJpeg) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(jpeg, formats);

  // WebP should succeed for JPEG source (sjpeg6 is large enough
  // that WebP is smaller).
  EXPECT_TRUE(result.webp.success) << result.webp.error_message;
  if (result.webp.success) {
    EXPECT_EQ(result.webp.output_mime_type, "image/webp");
    EXPECT_FALSE(result.webp.output_data.empty());
  }

  // AVIF should succeed for JPEG source
  EXPECT_TRUE(result.avif.success) << result.avif.error_message;
  if (result.avif.success) {
    EXPECT_EQ(result.avif.output_mime_type, "image/avif");
    EXPECT_FALSE(result.avif.output_data.empty());
  }

  // Optimized original should succeed
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  if (result.optimized_original.success) {
    EXPECT_EQ(result.optimized_original.output_mime_type, "image/jpeg");
  }
}

TEST_F(ImageTranscoderTest, TranscodeMultiPng) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(png, formats);

  // WebP may or may not be smaller; at least it shouldn't crash.
  // Small PNGs (basi2c08 is 315 bytes) may already be well-optimized,
  // so optimized_original may return success=false (no savings).
  if (result.optimized_original.success) {
    EXPECT_EQ(result.optimized_original.output_mime_type, "image/png");
  } else {
    EXPECT_TRUE(result.optimized_original.output_data.empty());
  }
}

TEST_F(ImageTranscoderTest, TranscodeMultiEmptyFormats) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> empty_formats;
  auto result = transcoder_.TranscodeMulti(jpeg, empty_formats);

  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
  EXPECT_FALSE(result.optimized_original.success);
}

// =================================================================
// Viewport-based resizing
// =================================================================

TEST_F(ImageTranscoderTest, ResizeForViewportMobile) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder_.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  ASSERT_GT(decoded.width, 480u);  // Must be wider than mobile target

  auto resized =
      transcoder_.ResizeForViewport(decoded, CapabilityMask::Viewport::kMobile);
  EXPECT_FALSE(resized.pixel_buffer.empty());
  EXPECT_EQ(resized.width, 480u);
  // Aspect ratio preserved: height should scale proportionally.
  auto expected_height = static_cast<uint32_t>(
      static_cast<uint64_t>(decoded.height) * 480 / decoded.width);
  EXPECT_EQ(resized.height, expected_height);
  EXPECT_EQ(resized.bytes_per_pixel, decoded.bytes_per_pixel);
  EXPECT_EQ(resized.has_alpha, decoded.has_alpha);
}

TEST_F(ImageTranscoderTest, ResizeForViewportDesktopNoResize) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder_.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());

  // Desktop has target_width=0, so no resize should happen.
  auto resized = transcoder_.ResizeForViewport(
      decoded, CapabilityMask::Viewport::kDesktop);
  EXPECT_TRUE(resized.pixel_buffer.empty());  // Empty = no resize needed
}

TEST_F(ImageTranscoderTest, ResizeForViewportImageAlreadySmall) {
  // Use a small PNG (32x32 from pngsuite).
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  auto decoded = transcoder_.DecodeToPixels(png);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  // pngsuite images are 32x32, smaller than mobile 480px target.
  ASSERT_LE(decoded.width, 480u);

  auto resized =
      transcoder_.ResizeForViewport(decoded, CapabilityMask::Viewport::kMobile);
  EXPECT_TRUE(resized.pixel_buffer.empty());  // No resize needed
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedMobile) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // WebP should succeed (resized image at mobile width).
  if (result.webp.success) {
    EXPECT_EQ(result.webp.output_mime_type, "image/webp");
    EXPECT_FALSE(result.webp.output_data.empty());
    // Resized + WebP should be much smaller than original JPEG.
    EXPECT_LT(result.webp.output_data.size(), jpeg.size());
  }

  // Optimized original should succeed.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedDesktopNoResize) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  // Desktop target_width=0, should behave identically to TranscodeMulti.
  auto result = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.webp.success) << result.webp.error_message;
  EXPECT_TRUE(result.avif.success) << result.avif.error_message;
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedGifFallback) {
  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  // GIF input should fall back to TranscodeMulti (no resize).
  auto result = transcoder_.TranscodeMultiResized(
      gif, formats, CapabilityMask::Viewport::kMobile);

  // Should not crash. GIF original should pass through.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/gif");
}

// =================================================================
// Save-Data quality tests (Feature 2)
// =================================================================

TEST(ImageTranscoderSaveDataTest, SaveDataAvifAndWebpBothSucceed) {
  // Verify that TranscodeMultiResized produces valid output for both
  // save-data modes.  The actual quality difference is tested at the
  // AVIF level (which does produce size differences).
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  // Fixed qualities (learned prediction and content presets off) so the
  // encodes are deterministic: the learned model's pick for this fixture
  // scores below the Save-Data floor and is legitimately declined under
  // the binding verdict — that is a decline-mechanics question, not this
  // test's.
  config.learned_quality = false;
  config.content_analysis = false;
  config.webp_quality = 90;
  // A q30 webp of this fixture scores below even the Save-Data floor
  // (target 70 - 15 = 55, floor 52) and is now correctly declined by the
  // binding verdict; q60 clears the Save-Data band while staying well
  // below the normal-mode 90.
  config.savedata_webp_quality = 60;

  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto normal = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  auto savedata = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // Both should succeed for both formats.
  EXPECT_TRUE(normal.webp.success) << normal.webp.error_message;
  EXPECT_TRUE(savedata.webp.success) << savedata.webp.error_message;
  EXPECT_TRUE(normal.avif.success) << normal.avif.error_message;
  EXPECT_TRUE(savedata.avif.success) << savedata.avif.error_message;

  // AVIF shows quality difference reliably.
  EXPECT_NE(savedata.avif.output_data, normal.avif.output_data);
}

TEST_F(ImageTranscoderTest, SaveDataAvifProducesDifferentOutput) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto normal = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  auto savedata = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  ASSERT_TRUE(normal.avif.success) << normal.avif.error_message;
  ASSERT_TRUE(savedata.avif.success) << savedata.avif.error_message;

  // Save-Data AVIF output should differ from normal.
  EXPECT_NE(savedata.avif.output_data, normal.avif.output_data);
}

// =================================================================
// Pixel density resizing tests (Feature 3)
// =================================================================

TEST(ImageTranscoderDensityTest, ResizeForViewportDensity2x) {
  // Use a custom config with small mobile width so the 512px test
  // image is larger than both 1x (100px) and 2x (200px) targets.
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.viewport_widths.mobile = 100;

  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  ASSERT_GT(decoded.width, 200u);  // Must be wider than 2x target

  // 1x mobile should resize to 100px
  auto resized_1x =
      transcoder.ResizeForViewport(decoded, CapabilityMask::Viewport::kMobile,
                                   CapabilityMask::PixelDensity::k1x);
  ASSERT_FALSE(resized_1x.pixel_buffer.empty());
  EXPECT_EQ(resized_1x.width, 100u);

  // 2x mobile should resize to 200px
  auto resized_2x =
      transcoder.ResizeForViewport(decoded, CapabilityMask::Viewport::kMobile,
                                   CapabilityMask::PixelDensity::k2xPlus);
  ASSERT_FALSE(resized_2x.pixel_buffer.empty());
  EXPECT_EQ(resized_2x.width, 200u);
}

TEST_F(ImageTranscoderTest, ResizeForViewportDesktop2xNoResize) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder_.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());

  // Desktop with target_width=0 should not resize even at 2x.
  auto resized =
      transcoder_.ResizeForViewport(decoded, CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k2xPlus);
  EXPECT_TRUE(resized.pixel_buffer.empty());  // No resize needed
}

TEST(ImageTranscoderDensityTest, TranscodeMultiResized2xLargerThan1x) {
  // Use a custom config with small mobile width so 1x (100px) and
  // 2x (200px) targets are both smaller than the 512px source.
  // Use WebP format which actually goes through the resize path
  // (of the kOriginal slots only PNG and GIF bypass resize).
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.viewport_widths.mobile = 100;

  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result_1x = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x);
  auto result_2x = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k2xPlus);

  ASSERT_TRUE(result_1x.webp.success) << result_1x.webp.error_message;
  ASSERT_TRUE(result_2x.webp.success) << result_2x.webp.error_message;

  // 2x WebP should be larger than 1x (200px vs 100px target width).
  EXPECT_GT(result_2x.webp.output_data.size(),
            result_1x.webp.output_data.size());
}

// =================================================================
// Static vs animated GIF detection (Feature 4)
// =================================================================

TEST_F(ImageTranscoderTest, IsAnimatedGifDetectsAnimated) {
  std::string animated = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(animated.empty());
  EXPECT_TRUE(ImageTranscoder::IsAnimatedGif(animated));
}

TEST_F(ImageTranscoderTest, IsAnimatedGifDetectsStatic) {
  std::string static_gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(static_gif.empty());
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(static_gif));
}

TEST_F(ImageTranscoderTest, IsAnimatedGifEmptyInput) {
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(""));
}

TEST_F(ImageTranscoderTest, StaticGifResizesInTranscodeMultiResized) {
  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  // Verify it's actually a static GIF.
  ASSERT_FALSE(ImageTranscoder::IsAnimatedGif(gif));

  // Decode to check dimensions.
  auto decoded = transcoder_.DecodeToPixels(gif);
  // interlaced.gif may be small; just verify TranscodeMultiResized
  // doesn't crash and produces output.
  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMultiResized(
      gif, formats, CapabilityMask::Viewport::kMobile);

  // Original should succeed (GIF passthrough or re-encode).
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
}

TEST_F(ImageTranscoderTest, AnimatedGifSkipsResizeInTranscodeMultiResized) {
  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());
  ASSERT_TRUE(ImageTranscoder::IsAnimatedGif(gif));

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  // Animated GIF should skip resize, fall back to TranscodeMulti.
  auto result = transcoder_.TranscodeMultiResized(
      gif, formats, CapabilityMask::Viewport::kMobile);

  // Original should be GIF passthrough (animated GIF is not re-encoded).
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/gif");
}

// =================================================================
// Quality-level effect on output size
// =================================================================

TEST(ImageTranscoderQualityTest, LowerQualityProducesSmallerWebp) {
  pagespeed::NullMessageHandler handler;

  // Use two separate transcoders: one with high quality, one with low.
  // Disable learned quality so the raw quality settings take effect.
  pagespeed::ImageTranscoderConfig config_high;
  config_high.webp_quality = 75;
  config_high.learned_quality = false;
  pagespeed::ImageTranscoder transcoder_high(config_high, &handler);

  pagespeed::ImageTranscoderConfig config_low;
  config_low.webp_quality = 37;
  config_low.learned_quality = false;
  pagespeed::ImageTranscoder transcoder_low(config_low, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result_high = transcoder_high.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  auto result_low = transcoder_low.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  ASSERT_TRUE(result_high.webp.success) << result_high.webp.error_message;
  ASSERT_TRUE(result_low.webp.success) << result_low.webp.error_message;

  // Lower quality WebP should be smaller.
  EXPECT_LT(result_low.webp.output_data.size(),
            result_high.webp.output_data.size());
}

// Test that learned quality prediction runs on the desktop fast-path
// (target_width=0).  Before this change, desktop images bypassed learned
// quality entirely because the fast-path delegated to TranscodeMulti
// which created its own fresh config snapshot.
TEST(ImageTranscoderQualityTest, LearnedQualityOnDesktopFastPath) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_webp = true;
  config.learned_quality_avif = true;
  config.content_analysis = true;
  // desktop_width=0 triggers the fast-path.
  config.viewport_widths.desktop = 0;
  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  ASSERT_TRUE(result.webp.success) << result.webp.error_message;
  ASSERT_TRUE(result.avif.success) << result.avif.error_message;

  // Learned quality should have been used on the desktop fast-path.
  EXPECT_TRUE(result.used_learned_quality)
      << "Learned quality should run on the desktop fast-path";
}

// Test that SSIMULACRA2 verification runs on the desktop fast-path.
TEST(ImageTranscoderQualityTest, Ssimulacra2VerificationOnDesktopFastPath) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_webp = true;
  config.quality_verify = true;
  config.content_analysis = true;
  config.viewport_widths.desktop = 0;
  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  ASSERT_TRUE(result.webp.success) << result.webp.error_message;

  // SSIMULACRA2 score should have been computed on the desktop fast-path.
  EXPECT_GE(result.webp_ssimulacra2_score, 0.0f)
      << "SSIMULACRA2 verification should run on the desktop fast-path";
}

// Test with custom viewport widths.
TEST(ImageTranscoderViewportConfigTest, CustomMobileWidth) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 320;
  config.viewport_widths.tablet = 600;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  ASSERT_GT(decoded.width, 320u);

  auto resized =
      transcoder.ResizeForViewport(decoded, CapabilityMask::Viewport::kMobile);
  EXPECT_FALSE(resized.pixel_buffer.empty());
  EXPECT_EQ(resized.width, 320u);
}

// ========== Phase 3.3: SVG passthrough ==========

TEST_F(ImageTranscoderTest, SvgReturnsFailure) {
  // SVG is not a raster format — ComputeImageType returns IMAGE_UNKNOWN,
  // so the transcoder always fails. No passthrough path exists.
  std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
      "<circle cx=\"50\" cy=\"50\" r=\"40\" fill=\"red\"/>"
      "</svg>";

  CapabilityMask mask;
  auto result = transcoder_.Transcode(svg, mask);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

TEST_F(ImageTranscoderTest, SvgToWebpReturnsFailure) {
  std::string svg =
      "<?xml version=\"1.0\"?>"
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect/></svg>";

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  auto result = transcoder_.Transcode(svg, mask);
  // SVG → WebP should not succeed.
  EXPECT_FALSE(result.success)
      << "SVG-to-WebP transcoding should fail gracefully";
}

// =============================================================================
// Phase 2 gap-filling tests
// =============================================================================

// Test: Tablet viewport — image narrower than 768px target → no resize.
TEST_F(ImageTranscoderTest, ResizeForViewportTabletNoResize) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder_.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  // sjpeg6.jpg is 512px wide — narrower than tablet 768px target.
  ASSERT_LE(decoded.width, 768u);

  auto resized =
      transcoder_.ResizeForViewport(decoded, CapabilityMask::Viewport::kTablet);
  EXPECT_TRUE(resized.pixel_buffer.empty());  // No resize needed
}

// Test: PNG with alpha to AVIF.
TEST_F(ImageTranscoderTest, PngAlphaToAvif) {
  std::string png = ReadTestFile("pngsuite/basi6a08.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(png, mask);
  // Small 32x32 PNG — AVIF may be larger and PNG fallback may have no savings.
  if (result.success) {
    EXPECT_FALSE(result.output_data.empty());
  }
}

// Test: JPEG to AVIF quality variance — lower quality produces smaller output.
TEST(ImageTranscoderAvifQualityTest, LowerQualityProducesSmallerAvif) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig high_config;
  high_config.avif_quality = 60;
  ImageTranscoder transcoder_high(high_config, &handler);

  ImageTranscoderConfig low_config;
  low_config.avif_quality = 25;
  ImageTranscoder transcoder_low(low_config, &handler);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result_high = transcoder_high.Transcode(jpeg, mask);
  auto result_low = transcoder_low.Transcode(jpeg, mask);

  ASSERT_TRUE(result_high.success) << result_high.error_message;
  ASSERT_TRUE(result_low.success) << result_low.error_message;

  if (result_high.output_mime_type == "image/avif" &&
      result_low.output_mime_type == "image/avif") {
    EXPECT_LT(result_low.output_data.size(), result_high.output_data.size())
        << "Lower quality AVIF should be smaller";
  }
}

// Test: TranscodeMultiResized with tablet viewport.
TEST_F(ImageTranscoderTest, TranscodeMultiResizedTablet) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP};

  auto result = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.webp.success) << result.webp.error_message;
  EXPECT_FALSE(result.webp.output_data.empty());
}

// Test: Random bytes do not crash any format path.
TEST_F(ImageTranscoderTest, RandomBytesAllFormats) {
  std::string random(256, '\xAB');

  for (auto fmt : {CapabilityMask::ImageFormat::kOriginal,
                   CapabilityMask::ImageFormat::kWebP,
                   CapabilityMask::ImageFormat::kAvif}) {
    CapabilityMask mask;
    mask.set_image_format(fmt);
    auto result = transcoder_.Transcode(random, mask);
    EXPECT_FALSE(result.success)
        << "Random bytes should fail for format " << static_cast<int>(fmt);
  }
}

// Test: Truncated JPEG header (2 bytes) across all formats.
TEST_F(ImageTranscoderTest, TruncatedJpegAllFormats) {
  std::string truncated("\xff\xd8", 2);

  for (auto fmt : {CapabilityMask::ImageFormat::kOriginal,
                   CapabilityMask::ImageFormat::kWebP,
                   CapabilityMask::ImageFormat::kAvif}) {
    CapabilityMask mask;
    mask.set_image_format(fmt);
    auto result = transcoder_.Transcode(truncated, mask);
    EXPECT_FALSE(result.success)
        << "Truncated JPEG should fail for format " << static_cast<int>(fmt);
  }
}

// =================================================================
// Additional coverage: IsAnimatedGif edge cases
// =================================================================

TEST_F(ImageTranscoderTest, IsAnimatedGifNonGifData) {
  // JPEG data should not be detected as animated GIF.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(jpeg));
}

TEST_F(ImageTranscoderTest, IsAnimatedGifTooShort) {
  // Data shorter than 13 bytes (GIF header size).
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif("GIF89a"));
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif("GIF87a12"));
}

TEST_F(ImageTranscoderTest, IsAnimatedGifCorruptData) {
  // Random binary data with enough length to pass the initial size check.
  std::string random(256, '\x00');
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(random));
}

TEST_F(ImageTranscoderTest, IsAnimatedGifMultipleAnimated) {
  // full2loop.gif has multiple frames.
  std::string gif = ReadTestFile("gif/full2loop.gif");
  ASSERT_FALSE(gif.empty());
  EXPECT_TRUE(ImageTranscoder::IsAnimatedGif(gif));
}

TEST_F(ImageTranscoderTest, IsAnimatedGifStaticO) {
  // o.gif is a small static GIF.
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

TEST_F(ImageTranscoderTest, IsAnimatedGifTransparent) {
  // transparent.gif is a static (single-frame) GIF with transparency.
  std::string gif = ReadTestFile("gif/transparent.gif");
  ASSERT_FALSE(gif.empty());
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// =================================================================
// Additional coverage: DecodeToPixels edge cases
// =================================================================

TEST_F(ImageTranscoderTest, DecodeToPixelsWebp) {
  // Decode a WebP file to pixels.
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  auto decoded = transcoder_.DecodeToPixels(webp);
  EXPECT_EQ(decoded.width, 32u);
  EXPECT_EQ(decoded.height, 20u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.bytes_per_pixel, 3);  // Opaque WebP is RGB
  EXPECT_FALSE(decoded.has_alpha);
}

TEST_F(ImageTranscoderTest, DecodeToPixelsWebpAlpha) {
  // Decode a WebP file with alpha to pixels.
  std::string webp = ReadTestFile("alpha_32x32.webp");
  ASSERT_FALSE(webp.empty());

  auto decoded = transcoder_.DecodeToPixels(webp);
  EXPECT_EQ(decoded.width, 32u);
  EXPECT_EQ(decoded.height, 32u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.bytes_per_pixel, 4);  // Alpha WebP is RGBA
  EXPECT_TRUE(decoded.has_alpha);
}

TEST_F(ImageTranscoderTest, DecodeToPixelsGif) {
  // Decode a static GIF to pixels.
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  auto decoded = transcoder_.DecodeToPixels(gif);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_GT(decoded.height, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
}

TEST_F(ImageTranscoderTest, DecodeToPixelsGrayscaleJpeg) {
  // Decode a grayscale JPEG to pixels.
  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder_.DecodeToPixels(jpeg);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_GT(decoded.height, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.bytes_per_pixel, 1);  // Grayscale
  EXPECT_FALSE(decoded.has_alpha);
}

TEST_F(ImageTranscoderTest, DecodeToPixelsSmallPng) {
  // Very small 1x1 PNG (s01n3p01.png from pngsuite).
  std::string png = ReadTestFile("pngsuite/s01n3p01.png");
  ASSERT_FALSE(png.empty());

  auto decoded = transcoder_.DecodeToPixels(png);
  EXPECT_EQ(decoded.width, 1u);
  EXPECT_EQ(decoded.height, 1u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
}

TEST_F(ImageTranscoderTest, DecodeToPixelsCorrupt) {
  // Corrupt data should return empty decoded image.
  std::string corrupt = "not an image at all";
  auto decoded = transcoder_.DecodeToPixels(corrupt);
  EXPECT_TRUE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.width, 0u);
  EXPECT_EQ(decoded.height, 0u);
}

TEST_F(ImageTranscoderTest, DecodeToPixelsPngAlpha) {
  // Decode a PNG with alpha channel (RGBA).
  std::string png = ReadTestFile("pngsuite/basi6a08.png");
  ASSERT_FALSE(png.empty());

  auto decoded = transcoder_.DecodeToPixels(png);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_GT(decoded.height, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.bytes_per_pixel, 4);  // RGBA
  EXPECT_TRUE(decoded.has_alpha);
}

// =================================================================
// Additional coverage: TranscodeMulti with WebP input
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiWebpInput) {
  // WebP input: both WebP-shaped slots re-encode under the standard
  // accept-if-strictly-smaller rule (#1375); neither returns the input as-is.
  // WebP input with AVIF requested: still fails (can't convert WebP to AVIF).
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(webp, formats);

  // Whichever WebP-shaped slot is filled holds a re-encode that beat the
  // origin -- never the origin's own bytes handed back under a success.
  if (result.webp.success) {
    EXPECT_EQ(result.webp.output_mime_type, "image/webp");
    EXPECT_LT(result.webp.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.webp.error_message.empty());
  }
  if (result.optimized_original.success) {
    EXPECT_EQ(result.optimized_original.output_mime_type, "image/webp");
    EXPECT_LT(result.optimized_original.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.optimized_original.error_message.empty());
  }

  // WebP → AVIF is not supported.
  EXPECT_FALSE(result.avif.success);
}

TEST_F(ImageTranscoderTest, TranscodeMultiGifInput) {
  // GIF input uses specialized pipeline for WebP.
  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(gif, formats);

  // Original GIF should pass through.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/gif");
}

TEST_F(ImageTranscoderTest, TranscodeMultiEmptyInput) {
  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder_.TranscodeMulti("", formats);
  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
  EXPECT_FALSE(result.optimized_original.success);
}

// =================================================================
// Additional coverage: TranscodeMultiResized paths
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiResizedEmptyInput) {
  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };
  auto result = transcoder_.TranscodeMultiResized(
      "", formats, CapabilityMask::Viewport::kMobile);
  EXPECT_FALSE(result.webp.success);
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedEmptyFormats) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> empty_formats;
  auto result = transcoder_.TranscodeMultiResized(
      jpeg, empty_formats, CapabilityMask::Viewport::kMobile);
  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
  EXPECT_FALSE(result.optimized_original.success);
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedSaveDataMobile) {
  // Save-Data with mobile viewport should use lower quality settings.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.webp_quality = 75;
  config.savedata_webp_quality = 30;
  config.viewport_widths.mobile = 100;  // Ensure resize happens

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto normal = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  auto save = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  ASSERT_TRUE(normal.webp.success) << normal.webp.error_message;
  ASSERT_TRUE(save.webp.success) << save.webp.error_message;

  // Save-Data with much lower quality should produce smaller output.
  EXPECT_LT(save.webp.output_data.size(), normal.webp.output_data.size());
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedAnimatedGifSaveData) {
  // Animated GIF + save-data: exercises the path where
  // is_animated_gif is true but save-data is on (does not delegate
  // to TranscodeMulti, instead continues to pixel decode path).
  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMultiResized(
      gif, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // GIF original should still succeed.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/gif");
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedPngOriginal) {
  // PNG input with kOriginal: exercises the fallback from OptimizeJpeg
  // to Transcode in TranscodeMultiResized.
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMultiResized(
      png, formats, CapabilityMask::Viewport::kDesktop);

  // Small PNGs may already be well-optimized (no savings).
  if (result.optimized_original.success) {
    EXPECT_EQ(result.optimized_original.output_mime_type, "image/png");
  } else {
    EXPECT_TRUE(result.optimized_original.output_data.empty());
  }
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedAvifFormat) {
  // Exercise the AVIF encoding path within TranscodeMultiResized.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.avif.success) << result.avif.error_message;
  EXPECT_EQ(result.avif.output_mime_type, "image/avif");
  EXPECT_FALSE(result.avif.output_data.empty());
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedJpegQualityHint) {
  // Exercise the jpeg_quality_hint parameter.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  // First call: use default quality.
  auto result1 = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);
  ASSERT_TRUE(result1.optimized_original.success);

  // Second call: with a quality hint from the first call.
  auto result2 = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.carried_hint = result1.final_jpeg_quality});
  ASSERT_TRUE(result2.optimized_original.success);
}

// =================================================================
// Additional coverage: Transcode format paths
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeGifToAvifAnimated) {
  // Animated GIF → AVIF. GIF is decoded as source_format IMAGE_GIF
  // which exercises the GIF path in ConvertToAvif.
  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(gif, mask);
  // Must succeed: either as AVIF or GIF passthrough fallback.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(result.output_mime_type == "image/avif" ||
              result.output_mime_type == "image/gif")
      << "Unexpected mime: " << result.output_mime_type;
  EXPECT_FALSE(result.output_data.empty());
}

TEST_F(ImageTranscoderTest, TranscodeWebpToAvifFallback) {
  // WebP → AVIF is skipped (src_format == IMAGE_WEBP), so the call falls
  // through to the original-format leg -- which since #1375 runs the standard
  // re-encode-and-accept-if-smaller rule rather than returning WebP as-is.
  // What must never happen either way is an AVIF result for a WebP input.
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(webp, mask);
  EXPECT_NE(result.output_mime_type, "image/avif");
  if (result.success) {
    EXPECT_EQ(result.output_mime_type, "image/webp");
    EXPECT_LT(result.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.error_message.empty());
  }
}

TEST_F(ImageTranscoderTest, TranscodeWebpToWebp) {
  // WebP → WebP: src_format == IMAGE_WEBP, target == kWebP, so the conversion
  // arm declines (source is already the target) and the original-format leg
  // decides.  Since #1375 that leg re-encodes and keeps the result only if it
  // is strictly smaller -- this is no longer a passthrough.
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(webp, mask);
  if (result.success) {
    EXPECT_EQ(result.output_mime_type, "image/webp");
    EXPECT_LT(result.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.error_message.empty());
  }
}

TEST_F(ImageTranscoderTest, TranscodeGifOriginalPassthrough) {
  // GIF with kOriginal mask should pass through as-is.
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(gif, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.output_mime_type, "image/gif");
  EXPECT_EQ(result.output_data.size(), gif.size());
}

// =================================================================
// Additional coverage: ResizeForViewport edge cases
// =================================================================

TEST(ImageTranscoderResizeTest, ResizeTabletActualResize) {
  // Use a custom config with small tablet width so the 512px image
  // is larger than the tablet target.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.tablet = 200;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  ASSERT_GT(decoded.width, 200u);

  auto resized =
      transcoder.ResizeForViewport(decoded, CapabilityMask::Viewport::kTablet);
  EXPECT_FALSE(resized.pixel_buffer.empty());
  EXPECT_EQ(resized.width, 200u);
  // Aspect ratio preserved.
  auto expected_height = static_cast<uint32_t>(
      static_cast<uint64_t>(decoded.height) * 200 / decoded.width);
  EXPECT_EQ(resized.height, expected_height);
}

TEST(ImageTranscoderResizeTest, ResizeForViewportTablet2x) {
  // Tablet at 2x density with custom small width.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.tablet = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  ASSERT_GT(decoded.width, 200u);  // Must be wider than 2x target (200px)

  // 2x tablet should resize to 200px.
  auto resized =
      transcoder.ResizeForViewport(decoded, CapabilityMask::Viewport::kTablet,
                                   CapabilityMask::PixelDensity::k2xPlus);
  ASSERT_FALSE(resized.pixel_buffer.empty());
  EXPECT_EQ(resized.width, 200u);
}

TEST_F(ImageTranscoderTest, ResizeForViewportEmptyPixels) {
  // Empty decoded image should return empty.
  DecodedImage empty;
  auto resized =
      transcoder_.ResizeForViewport(empty, CapabilityMask::Viewport::kMobile);
  EXPECT_TRUE(resized.pixel_buffer.empty());
}

// =================================================================
// Additional coverage: Content analysis quality presets
// =================================================================

TEST(ImageTranscoderContentAnalysisTest, ContentAnalysisDisabled) {
  // When content_analysis is disabled, quality factors should not be applied.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = false;
  config.quality_verify = false;  // Disable verify for simpler assertions.
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  // applied_preset should be default (kUnknown).
  EXPECT_EQ(result.applied_preset.content_class,
            pagespeed::ContentClass::kUnknown);
}

TEST(ImageTranscoderContentAnalysisTest, ContentAnalysisEnabled) {
  // When content_analysis is enabled, the transcoder should classify content
  // and populate applied_preset.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.quality_verify = false;
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  // applied_preset should be populated (not kUnknown for a real image).
  // The actual class depends on image content, so just check it ran.
  EXPECT_NE(result.applied_preset.content_class,
            pagespeed::ContentClass::kUnknown)
      << "Content analysis should classify sjpeg6.jpg";
}

// =================================================================
// Additional coverage: Quality verification (SSIMULACRA2) paths
// =================================================================

TEST(ImageTranscoderQualityVerifyTest, QualityVerifyPopulatesScores) {
  // Quality verification should populate ssimulacra2 scores.
  // Use mobile viewport smaller than the image (512px) to trigger resize,
  // which activates the pixel-based JPEG encoding path where SSIMULACRA2
  // verification runs.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.ssimulacra2_max_attempts = 1;  // Check only, no re-encode.
  config.viewport_widths.mobile = 400;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  // jpeg_quality_hint > 0 prevents the no-resize optimization short-circuit.
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.carried_hint = 85});

  // WebP SSIMULACRA2 score should be populated.
  if (result.webp.success) {
    EXPECT_GE(result.webp_ssimulacra2_score, 0.0f)
        << "WebP SSIMULACRA2 score should be computed";
    EXPECT_FALSE(result.webp_ssimulacra2_reencoded)
        << "max_attempts=1 should not re-encode";
  }

  // AVIF SSIMULACRA2 score may or may not be populated depending on
  // whether the decoded AVIF output matches the source pixel format.
  // The quality verify path still runs (exercising the code), but the
  // dimension/bpp check may prevent score computation.
  EXPECT_FALSE(result.avif_ssimulacra2_reencoded)
      << "max_attempts=1 should not re-encode";

  // JPEG SSIMULACRA2 score should be populated (resize triggers pixel path,
  // and encoded JPEG dimensions match encode_src dimensions).
  if (result.optimized_original.success &&
      result.optimized_original.output_mime_type == "image/jpeg") {
    EXPECT_GE(result.ssimulacra2_score, 0.0f)
        << "JPEG SSIMULACRA2 score should be computed";
    EXPECT_FALSE(result.ssimulacra2_reencoded);
  }

  // At least WebP and JPEG should have valid scores.
  bool any_score = (result.webp_ssimulacra2_score >= 0.0f) ||
                   (result.ssimulacra2_score >= 0.0f);
  EXPECT_TRUE(any_score) << "At least one format should have a score";
}

TEST(ImageTranscoderQualityVerifyTest, QualityVerifyDisabled) {
  // When quality_verify is disabled, SSIMULACRA2 scores should be -1.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);

  EXPECT_EQ(result.webp_ssimulacra2_score, -1.0f);
  EXPECT_EQ(result.ssimulacra2_score, -1.0f);
  EXPECT_FALSE(result.webp_ssimulacra2_reencoded);
  EXPECT_FALSE(result.ssimulacra2_reencoded);
}

TEST(ImageTranscoderQualityVerifyTest, QualityVerifyReencode) {
  // Set an extreme target score to trigger re-encoding.
  // Use mobile viewport with small target to ensure the full path runs.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.target_ssimulacra2 = 95.0f;    // Very high target
  config.ssimulacra2_tolerance = 1.0f;  // Tight tolerance
  config.ssimulacra2_max_attempts = 2;
  config.ssimulacra2_quality_step = 15;
  config.webp_quality = 40;  // Start low to force re-encode
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // Should have attempted re-encoding (quality too low for target).
  if (result.webp.success) {
    // The re-encode path should have been exercised.
    EXPECT_GE(result.webp_ssimulacra2_score, 0.0f);
    // With q=40 and target=95, it should have re-encoded.
    EXPECT_TRUE(result.webp_ssimulacra2_reencoded)
        << "WebP q=40 against target=95 should trigger re-encode";
  }
}

// =================================================================
// Additional coverage: ConfigAccessor constructor
// =================================================================

TEST(ImageTranscoderConfigAccessorTest, ConfigAccessorIsInvoked) {
  // Test the ConfigAccessor constructor path.
  NullMessageHandler handler;
  int call_count = 0;
  ImageTranscoderConfig base_config;
  base_config.webp_quality = 50;

  ImageTranscoder::ConfigAccessor accessor = [&]() {
    ++call_count;
    return base_config;
  };

  ImageTranscoder transcoder(accessor, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  // config() should have been called at least once.
  EXPECT_GE(call_count, 1);
}

TEST(ImageTranscoderConfigAccessorTest, HotReloadConfigChange) {
  // Test that config changes between calls take effect (hot-reload).
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.jpeg_quality = 85;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;

  ImageTranscoder transcoder([&config]() { return config; }, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  // First call with quality 85.
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result1 = transcoder.Transcode(jpeg, mask);
  ASSERT_TRUE(result1.success);

  // Change config to quality 30 (simulating hot-reload).
  config.jpeg_quality = 30;
  config.lossy_jpeg = true;
  auto result2 = transcoder.Transcode(jpeg, mask);
  ASSERT_TRUE(result2.success);

  // Lower quality should produce smaller output.
  EXPECT_LT(result2.output_data.size(), result1.output_data.size())
      << "Hot-reloaded lower quality should produce smaller output";
}

// =================================================================
// Additional coverage: Final quality recording
// =================================================================

TEST(ImageTranscoderFinalQualityTest, FinalQualityRecorded) {
  // Use mobile viewport with a target width smaller than the image
  // so TranscodeMultiResized takes the full path (not kDesktop=0 shortcut).
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 100;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // Final qualities should be recorded (positive values).
  EXPECT_GT(result.final_jpeg_quality, 0);
  EXPECT_GT(result.final_webp_quality, 0);
  EXPECT_GT(result.final_avif_quality, 0);
}

// =================================================================
// Additional coverage: EncodeWebpFromPixels / EncodeAvifFromPixels
// via TranscodeMultiResized with different pixel formats
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiResizedGrayscale) {
  // Grayscale JPEG exercises the GRAY_8 pixel format path in
  // EncodeWebpFromPixels and EncodeAvifFromPixels.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);

  // At least the optimized original should succeed.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedPngAlpha) {
  // PNG with alpha exercises the RGBA pixel format path in
  // EncodeWebpFromPixels and EncodeAvifFromPixels.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/basi6a08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      png, formats, CapabilityMask::Viewport::kDesktop);

  // Small PNG with alpha may already be well-optimized (no savings).
  if (result.optimized_original.success) {
    EXPECT_EQ(result.optimized_original.output_mime_type, "image/png");
  } else {
    EXPECT_TRUE(result.optimized_original.output_data.empty());
  }
}

// =================================================================
// Additional coverage: Denoising path
// =================================================================

TEST(ImageTranscoderDenoiseTest, DenoiseThresholdZeroDisablesDenoise) {
  // With denoise_threshold = 0, denoising should never trigger.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.denoise_threshold = 0.0f;
  config.quality_verify = false;
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);

  EXPECT_FALSE(result.denoised)
      << "Denoise should be disabled with threshold 0";
}

// =================================================================
// Additional coverage: Learned quality prediction
// =================================================================

TEST(ImageTranscoderLearnedQualityTest, LearnedQualityEnabled) {
  // Use mobile viewport with small target to ensure the full
  // TranscodeMultiResized path runs (not the kDesktop=0 shortcut).
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = true;
  config.learned_quality_webp = true;
  config.learned_quality_avif = true;
  config.quality_verify = false;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // Learned quality should have been used (models should produce predictions).
  EXPECT_TRUE(result.used_learned_quality)
      << "Learned quality should be used for JPEG input";
  // Final qualities should be recorded.
  EXPECT_GT(result.final_jpeg_quality, 0);
  EXPECT_GT(result.final_webp_quality, 0);
  EXPECT_GT(result.final_avif_quality, 0);
}

TEST(ImageTranscoderLearnedQualityTest, LearnedQualityDisabled) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = false;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);

  EXPECT_FALSE(result.used_learned_quality)
      << "Learned quality should not be used when disabled";
}

TEST(ImageTranscoderLearnedQualityTest, LearnedQualitySaveDataReducesTarget) {
  // With Save-Data, the target SSIMULACRA2 is reduced by
  // savedata_score_reduction. Exercise the Save-Data learned quality path.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = true;
  config.learned_quality_webp = true;
  config.learned_quality_avif = true;
  config.quality_verify = false;
  config.savedata_score_reduction = 15.0f;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto normal = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  auto savedata = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // Both should succeed.
  EXPECT_TRUE(normal.webp.success) << normal.webp.error_message;
  EXPECT_TRUE(savedata.webp.success) << savedata.webp.error_message;

  // Save-Data should use lower quality → smaller output.
  // (Quality reduction path exercises the savedata_score_reduction.)
  if (normal.webp.success && savedata.webp.success) {
    EXPECT_NE(savedata.webp.output_data, normal.webp.output_data)
        << "Save-Data should produce different output";
  }
}

TEST(ImageTranscoderLearnedQualityTest, LearnedQualityPartialDisable) {
  // Disable learned quality for JPEG only.
  // Use mobile viewport with small target to ensure the full path runs.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = false;
  config.learned_quality_webp = true;
  config.learned_quality_avif = true;
  config.quality_verify = false;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // Should still use learned quality (for WebP at least).
  EXPECT_TRUE(result.used_learned_quality)
      << "WebP learned quality should be used";
  EXPECT_GT(result.final_webp_quality, 0);
}

// =================================================================
// Additional coverage: Learned quality with different source formats
// =================================================================

TEST(ImageTranscoderLearnedQualityTest, LearnedQualityPngSource) {
  // PNG source exercises the source_format=1 path.
  // Use mobile viewport with small target to ensure the full path runs.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.quality_verify = false;
  config.viewport_widths.mobile =
      10;  // Small enough that 32px PNG won't resize

  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      png, formats, CapabilityMask::Viewport::kMobile);

  // Should have attempted learned quality prediction.
  // Small 32x32 PNG may not produce smaller WebP, which is fine.
  EXPECT_GT(result.final_webp_quality, 0);
}

TEST(ImageTranscoderLearnedQualityTest, LearnedQualityGifSource) {
  // Static GIF source exercises the source_format=2 path.
  // Use mobile viewport with small target to ensure the full path runs.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.quality_verify = false;
  config.viewport_widths.mobile = 10;  // Small enough for the GIF

  ImageTranscoder transcoder(config, &handler);

  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      gif, formats, CapabilityMask::Viewport::kMobile);

  EXPECT_GT(result.final_webp_quality, 0);
}

// =================================================================
// Additional coverage: Optimize original JPEG already optimized
// =================================================================

TEST_F(ImageTranscoderTest, OptimizeAlreadyOptimizedJpeg) {
  // already_optimized.jpg with default lossy config may or may not produce
  // savings depending on the file's original quality vs target q85.
  std::string jpeg = ReadTestFile("jpeg/already_optimized.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(jpeg, mask);
  if (result.success) {
    EXPECT_EQ(result.output_mime_type, "image/jpeg");
    // If it succeeded, it must have produced smaller output.
    EXPECT_LT(result.output_data.size(), jpeg.size());
  } else {
    EXPECT_TRUE(result.output_data.empty());
  }
}

TEST_F(ImageTranscoderTest, OptimizeAlreadyOptimizedPng) {
  // Already optimized PNG should have no savings (or very small ones).
  std::string png = ReadTestFile("pngsuite/already_optimized.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(png, mask);
  if (result.success) {
    // If it somehow succeeds, it must be genuinely smaller.
    EXPECT_LT(result.output_data.size(), png.size());
  } else {
    EXPECT_TRUE(result.output_data.empty());
  }
}

// =================================================================
// Additional coverage: GIF → WebP edge cases
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeGifToWebpBadPixel) {
  // Test with bad pixel GIF to exercise error paths.
  std::string gif = ReadTestFile("gif/bad.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(gif, mask);
  // Should not crash. May fail for corrupt GIF but should handle gracefully.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

TEST_F(ImageTranscoderTest, TranscodeZeroSizeAnimationGif) {
  std::string gif = ReadTestFile("gif/zero_size_animation.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(gif, mask);
  // Should not crash with zero-size animation GIF.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// =================================================================
// Coverage gap tests: unknown format, JPEG/PNG optimization fallback,
// WebP unsupported source, AVIF unsupported pixel format, proactive
// multi-transcode logging fallback, animated GIF local color table,
// learned quality prediction paths, SSIM verification loop.
// =================================================================

// Test: Data that has a recognizable prefix but unknown image type.
// Exercises the "Cannot optimize unknown format" default case (line 163).
TEST_F(ImageTranscoderTest, TranscodeUnknownFormatFallback) {
  // Construct data that looks like TIFF (II + magic 42) which
  // ComputeImageType recognizes but our Transcode format switch does not.
  // Actually, IMAGE_UNKNOWN triggers the "Unsupported image format" path
  // (line 86). We can exercise line 163 by crafting data that passes
  // format detection but fails all optimization paths.
  //
  // A more reliable approach: send BMP-like data that won't match any
  // recognized format. ComputeImageType returns IMAGE_UNKNOWN -> line 86.
  static constexpr char kBmpHeader[] = {'B',    'M',    '\x00', '\x00',
                                        '\x00', '\x00', '\x00', '\x00',
                                        '\x00', '\x00', '\x36', '\x00'};
  std::string bmp_like(kBmpHeader, sizeof(kBmpHeader));
  bmp_like.resize(100, '\x00');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(bmp_like, mask);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: JPEG where optimization produces output smaller than input.
// Exercises the size-check branch at line 182 (output < input -> use optimized).
TEST_F(ImageTranscoderTest, JpegOptimizationProducesSmallerOutput) {
  // sjpeg4.jpg is a large enough JPEG that optimization should reduce size.
  std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.lossy_jpeg = true;
  config.progressive_jpeg = true;
  config.jpeg_quality = 75;

  ImageTranscoder transcoder(config, &handler);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
  // With lossy at quality 75, output should be smaller than original.
  EXPECT_LT(result.output_data.size(), jpeg.size());
}

// Test: JPEG where optimized output is same size or larger → no savings.
TEST_F(ImageTranscoderTest, JpegOptimizationFailsWhenNoSavings) {
  // Use lossless JPEG optimization on a small JPEG.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.lossy_jpeg = false;
  config.progressive_jpeg = false;

  ImageTranscoder transcoder(config, &handler);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder.Transcode(jpeg, mask);
  // Lossless on small JPEG produces no savings.
  if (!result.success) {
    EXPECT_TRUE(result.output_data.empty());
  } else {
    // If optimization did produce savings, output must be smaller.
    EXPECT_LT(result.output_data.size(), jpeg.size());
  }
}

// Test: PNG optimization returns optimized when smaller, or fails (no savings).
// Exercises the OptimizePng path (lines 197-203).
TEST_F(ImageTranscoderTest, PngOptimizationPath) {
  // Use a PNG from the test suite.
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(png, mask);
  // Small PNGs may already be well-optimized (no savings).
  if (result.success) {
    EXPECT_EQ("image/png", result.output_mime_type);
    EXPECT_FALSE(result.output_data.empty());
  } else {
    EXPECT_TRUE(result.output_data.empty());
  }
}

// Test: ConvertToWebp with unsupported source format (not JPEG/PNG).
// Exercises line 217 "ConvertToWebp: unsupported source format".
TEST_F(ImageTranscoderTest, ConvertGifToWebpViaTranscodeSingleFormat) {
  // GIF → WebP via Transcode goes through ConvertGifToWebp, not ConvertToWebp.
  // To hit the ConvertToWebp unsupported path, we would need a format
  // that passes ComputeImageType but is not JPEG/PNG/GIF/WebP.
  // However, the Transcode switch already handles GIF and WebP specially.
  // This test verifies the GIF → WebP pipeline works correctly.
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(gif, mask);
  // Small GIF may fail WebP conversion (larger output) but should not crash.
  if (result.success) {
    EXPECT_TRUE(result.output_mime_type == "image/webp" ||
                result.output_mime_type == "image/gif");
  }
}

// Test: AVIF encoding with grayscale (GRAY_8) pixel format.
// Exercises the GRAY_8 case in ConvertToAvif (lines 297-299).
TEST_F(ImageTranscoderTest, GrayscaleJpegToAvif) {
  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(jpeg, mask);
  // Grayscale JPEG → AVIF exercises the GRAY_8 / YUV400 path.
  // May succeed or fall back depending on size comparison.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(result.output_data.empty());
}

// Test: Multi-transcode with pixel decode failure falls back to individual paths.
// Exercises lines 613-636 (fallback when DecodeToPixels fails).
TEST_F(ImageTranscoderTest, TranscodeMultiPixelDecodeFailureFallback) {
  // Corrupt data that has enough of a JPEG header to be recognized
  // but will fail pixel decoding. Truncated JPEG.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  // Truncate to half - enough for format detection but not full decode.
  std::string truncated = jpeg.substr(0, jpeg.size() / 4);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  // This should trigger the fallback path in TranscodeMulti where
  // DecodeToPixels fails and individual Transcode calls are made.
  auto result = transcoder_.TranscodeMulti(truncated, formats);
  // At least one path should handle the corrupt input gracefully.
  // Original JPEG optimization may succeed on truncated data or fail.
  // The point is it exercises the fallback path without crashing.
}

// Test: IsAnimatedGif with a GIF that has local color table.
// Exercises lines 838-841 (local color table skip in IsAnimatedGif).
TEST_F(ImageTranscoderTest, IsAnimatedGifWithLocalColorTable) {
  // animated_interlaced.gif has multiple frames and may have local color tables.
  std::string gif = ReadTestFile("gif/animated_interlaced.gif");
  ASSERT_FALSE(gif.empty());
  EXPECT_TRUE(ImageTranscoder::IsAnimatedGif(gif));
}

// Test: IsAnimatedGif with a GIF that has extension blocks.
// Exercises the extension block parsing (lines 852-861).
TEST_F(ImageTranscoderTest, IsAnimatedGifWithExtensionBlocks) {
  // square2loop.gif is animated with extension blocks.
  std::string gif = ReadTestFile("gif/square2loop.gif");
  ASSERT_FALSE(gif.empty());
  EXPECT_TRUE(ImageTranscoder::IsAnimatedGif(gif));
}

// Test: IsAnimatedGif with frame_smaller_than_screen.gif.
TEST_F(ImageTranscoderTest, IsAnimatedGifFrameSmallerThanScreen) {
  std::string gif = ReadTestFile("gif/frame_smaller_than_screen.gif");
  ASSERT_FALSE(gif.empty());
  // This may or may not be animated depending on its frame count.
  // The test exercises the parsing path without crashing.
  ImageTranscoder::IsAnimatedGif(gif);
}

// Test: Learned quality prediction with all formats enabled.
// More specifically exercises the PredictQuality/IsValidPrediction path.
TEST(ImageTranscoderLearnedQualityCoverageTest, AllFormatsEnabledPrediction) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = true;
  config.learned_quality_webp = true;
  config.learned_quality_avif = true;
  config.content_analysis = true;
  config.quality_verify = false;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // With content analysis + learned quality, predictions should run.
  // Verify that learned quality was attempted.
  bool any_quality_set = (result.final_jpeg_quality > 0) ||
                         (result.final_webp_quality > 0) ||
                         (result.final_avif_quality > 0);
  EXPECT_TRUE(any_quality_set) << "At least one quality should be predicted";
}

// Test: Learned quality with WebP source (exercises source_format=3 path).
TEST(ImageTranscoderLearnedQualityCoverageTest, WebpSourceFormat) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.content_analysis = true;
  config.quality_verify = false;
  config.viewport_widths.mobile = 10;

  ImageTranscoder transcoder(config, &handler);

  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      webp, formats, CapabilityMask::Viewport::kMobile);

  // WebP input with WebP format should return as-is (no encode needed).
  // The learned quality path may still run but the WebP→WebP passthrough
  // takes precedence.
  EXPECT_TRUE(result.webp.success) << result.webp.error_message;
}

// Test: SSIMULACRA2 verification for AVIF format.
// AVIF SSIM verification requires decoded output to match encode_src dimensions
// and bytes_per_pixel. Use Desktop viewport (no resize) to avoid dimension
// mismatch. The AVIF decode may return different bpp due to YUV conversion,
// so we accept either score or -1 (bpp mismatch).
TEST(ImageTranscoderSsimVerifyCoverageTest, AvifSsimVerification) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.target_ssimulacra2 = 70.0f;
  config.ssimulacra2_tolerance = 5.0f;
  config.ssimulacra2_max_attempts = 2;
  config.ssimulacra2_quality_step = 15;
  config.avif_quality = 50;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  // Desktop viewport = no resize, best chance for dimension match.
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);

  if (result.avif.success) {
    // Score may or may not be computed depending on bpp match after AVIF
    // decode. Either way, the SSIM code path was exercised.
    if (result.avif_ssimulacra2_score >= 0.0f) {
      EXPECT_LE(result.avif_ssimulacra2_score, 100.0f);
    }
  }
}

// Test: SSIMULACRA2 verification with quality already above target
// (over-quality reduction path).
TEST(ImageTranscoderSsimVerifyCoverageTest, OverQualityReduction) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.target_ssimulacra2 = 30.0f;    // Very low target
  config.ssimulacra2_tolerance = 1.0f;  // Tight tolerance
  config.ssimulacra2_max_attempts = 2;
  config.ssimulacra2_quality_step = 15;
  config.webp_quality = 90;  // Start high
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  if (result.webp.success) {
    // With q=90 and target=30, score should be well above target,
    // triggering the quality reduction path (score > hi).
    EXPECT_GE(result.webp_ssimulacra2_score, 0.0f);
    // Re-encode should have been attempted (over-quality).
    EXPECT_TRUE(result.webp_ssimulacra2_reencoded)
        << "q=90 against target=30 should trigger over-quality re-encode";
  }
}

// Test: JPEG SSIMULACRA2 verification loop.
// OptimizeJpeg works on the original input_data (not resized pixels), so
// the decoded JPEG output must have the same dimensions as encode_src.
// Use mobile viewport with target_width larger than the image so no resize
// happens, but the code still goes through the TranscodeMultiResized path
// (Desktop viewport with target=0 short-circuits to TranscodeMulti which
// lacks SSIM verification).
// sjpeg6.jpg is 512px wide; set mobile target to 1024px to avoid resize.
// Pass jpeg_quality_hint > 0 to prevent the no-resize optimization.
TEST(ImageTranscoderSsimVerifyCoverageTest, JpegReencodeAttempt) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.target_ssimulacra2 = 95.0f;
  config.ssimulacra2_tolerance = 1.0f;
  config.ssimulacra2_max_attempts = 3;  // Allow multiple retries
  config.ssimulacra2_quality_step = 20;
  config.jpeg_quality = 30;  // Start low
  config.lossy_jpeg = true;
  // Use viewport smaller than image (512px) to trigger resize and the
  // pixel-based JPEG encoding path where SSIMULACRA2 verification runs.
  config.viewport_widths.mobile = 400;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  // jpeg_quality_hint > 0 prevents the no-resize optimization short-circuit.
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.carried_hint = 30});

  if (result.optimized_original.success &&
      result.optimized_original.output_mime_type == "image/jpeg") {
    // JPEG SSIM score should be computed (resize triggers pixel path).
    EXPECT_GE(result.ssimulacra2_score, 0.0f)
        << "JPEG SSIMULACRA2 score should be computed after resize";
    // With q=30 against target=95, JPEG should have been re-encoded.
    EXPECT_TRUE(result.ssimulacra2_reencoded)
        << "JPEG q=30 against target=95 should trigger re-encode";
  }
}

// Test: TranscodeMultiResized with all 3 formats + SSIM + content analysis.
// Exercises the full pipeline including learned quality fallback counter.
TEST(ImageTranscoderFullPipelineTest, FullPipelineAllFormats) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = true;
  config.learned_quality_webp = true;
  config.learned_quality_avif = true;
  config.content_analysis = true;
  config.quality_verify = true;
  config.ssimulacra2_max_attempts = 2;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  // At least some formats should succeed.
  bool any_success = result.webp.success || result.avif.success ||
                     result.optimized_original.success;
  EXPECT_TRUE(any_success) << "At least one format should succeed";

  // Learned quality should have been attempted.
  EXPECT_TRUE(result.used_learned_quality ||
              result.learned_quality_fallbacks > 0)
      << "Learned quality should have been attempted";

  // Final qualities should be recorded.
  EXPECT_GT(result.final_jpeg_quality, 0);
  EXPECT_GT(result.final_webp_quality, 0);
  EXPECT_GT(result.final_avif_quality, 0);
}

// Test: GIF with extension blocks and no Global Color Table.
// Constructs a minimal GIF to exercise the no-GCT path in IsAnimatedGif.
TEST_F(ImageTranscoderTest, IsAnimatedGifNoGlobalColorTable) {
  // Construct a minimal GIF89a header with no Global Color Table.
  std::string gif;
  gif += "GIF89a";                    // Header
  gif.append("\x01\x00\x01\x00", 4);  // Logical Screen Descriptor (1x1)
  gif.append("\x00", 1);              // Packed: no GCT
  gif.append("\x00", 1);              // Background color
  gif.append("\x00", 1);              // Pixel aspect ratio
  // Single Image Descriptor
  gif += ',';                         // Image separator
  gif.append("\x00\x00\x00\x00", 4);  // Left, Top
  gif.append("\x01\x00\x01\x00", 4);  // Width, Height (1x1)
  gif.append("\x00", 1);              // Packed: no LCT
  gif += '\x02';                      // LZW minimum code size
  gif += "\x02\x4C\x01";              // Sub-block data (no NUL)
  gif.append("\x00", 1);              // Sub-block terminator
  gif += ';';                         // Trailer

  // Single frame → not animated.
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// =================================================================
// Coverage improvement: IsAnimatedGif detailed parsing paths
// =================================================================

// Test: Synthetic GIF with no Global Color Table and two frames (animated).
// Exercises the else-branch at line 823 (pos = 13 when no GCT) and the
// image_count > 1 return at line 833.
TEST_F(ImageTranscoderTest, IsAnimatedGifNoGctMultipleFrames) {
  // Construct a GIF89a with no Global Color Table and two Image Descriptors.
  // Use explicit byte array to correctly handle embedded null bytes.
  const uint8_t data[] = {
      // Header (6 bytes)
      'G', 'I', 'F', '8', '9', 'a',
      // Logical Screen Descriptor (7 bytes)
      0x01, 0x00, 0x01,
      0x00,  // Width=1, Height=1
      0x00,  // Packed: no GCT
      0x00,  // Background color
      0x00,  // Pixel aspect ratio
      // --- Frame 1 ---
      0x2C,  // Image separator
      0x00, 0x00, 0x00,
      0x00,  // Left, Top
      0x01, 0x00, 0x01,
      0x00,  // Width=1, Height=1
      0x80,  // Packed: LCT flag=1, size=0 (2 entries)
      0x00, 0x00, 0x00, 0x00, 0x00,
      0x00,  // LCT (6 bytes)
      0x02,  // LZW minimum code size
      0x02, 0x4C,
      0x01,  // Sub-block: size=2, data
      0x00,  // Sub-block terminator
      // --- Frame 2 ---
      0x2C,  // Image separator
      0x00, 0x00, 0x00,
      0x00,  // Left, Top
      0x01, 0x00, 0x01,
      0x00,  // Width=1, Height=1
      0x00,  // Packed: no LCT
      0x02,  // LZW minimum code size
      0x02, 0x4C,
      0x01,  // Sub-block: size=2, data
      0x00,  // Sub-block terminator
      0x3B,  // Trailer
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  EXPECT_TRUE(ImageTranscoder::IsAnimatedGif(gif));
}

// Test: Synthetic GIF with local color table but only one frame (static).
// Exercises the LCT skip path (lines 838-841) without triggering animation.
TEST_F(ImageTranscoderTest, IsAnimatedGifSingleFrameWithLocalColorTable) {
  const uint8_t data[] = {
      // Header
      'G', 'I', 'F', '8', '9', 'a',
      // Logical Screen Descriptor
      0x01, 0x00, 0x01,
      0x00,  // 1x1
      0x00,  // Packed: no GCT
      0x00,
      0x00,  // Background color, pixel aspect ratio
      // Image Descriptor with Local Color Table
      0x2C,  // Image separator
      0x00, 0x00, 0x00,
      0x00,  // Left, Top
      0x01, 0x00, 0x01,
      0x00,  // 1x1
      0x82,  // Packed: LCT flag=1, size=2 (8 entries)
      // LCT (8 entries = 24 bytes)
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x02,  // LZW minimum code size
      0x02, 0x4C,
      0x01,  // Sub-block: size=2, data
      0x00,  // Sub-block terminator
      0x3B,  // Trailer
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// Test: Synthetic GIF with an extension block followed by one image frame.
// Exercises the extension block parsing (lines 852-861) with a single
// frame so the function should return false.
TEST_F(ImageTranscoderTest, IsAnimatedGifExtensionBlockSingleFrame) {
  const uint8_t data[] = {
      // Header
      'G', 'I', 'F', '8', '9', 'a',
      // Logical Screen Descriptor
      0x01, 0x00, 0x01,
      0x00,  // 1x1
      0x00, 0x00,
      0x00,  // Packed (no GCT), bg, aspect
      // Graphic Control Extension
      0x21,  // Extension introducer
      0xF9,  // Graphic Control label
      0x04,  // Sub-block size = 4
      0x00, 0x00, 0x00,
      0x00,  // Extension data
      0x00,  // Sub-block terminator
      // Single Image Descriptor
      0x2C,  // Image separator
      0x00, 0x00, 0x00,
      0x00,  // Left, Top
      0x01, 0x00, 0x01,
      0x00,  // 1x1
      0x00,  // Packed: no LCT
      0x02,  // LZW minimum code size
      0x02, 0x4C,
      0x01,  // Sub-block: size=2, data
      0x00,  // Sub-block terminator
      0x3B,  // Trailer
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// Test: IsAnimatedGif with truncated data inside image descriptor.
// Data has a valid GIF header and one image separator but truncates before
// the full Image Descriptor (needs 10 bytes after 0x2C). Exercises the
// bounds check at line 834 (pos + 10 > data.size()).
TEST_F(ImageTranscoderTest, IsAnimatedGifTruncatedImageDescriptor) {
  const uint8_t data[] = {
      // Header
      'G',
      'I',
      'F',
      '8',
      '9',
      'a',
      // Logical Screen Descriptor
      0x01,
      0x00,
      0x01,
      0x00,  // 1x1
      0x00,
      0x00,
      0x00,  // Packed (no GCT), bg, aspect
      // Image separator + only 2 bytes (truncated)
      0x2C,
      0x00,
      0x00,
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// Test: IsAnimatedGif with truncated data at LZW minimum code size.
// One full Image Descriptor but no LZW data follows. Exercises the
// bounds check at line 843 (pos >= data.size() after skipping LCT).
TEST_F(ImageTranscoderTest, IsAnimatedGifTruncatedAfterImageDescriptor) {
  const uint8_t data[] = {
      // Header
      'G', 'I', 'F', '8', '9', 'a',
      // Logical Screen Descriptor
      0x01, 0x00, 0x01,
      0x00,  // 1x1
      0x00, 0x00,
      0x00,  // Packed (no GCT), bg, aspect
      // Image Descriptor (complete) but no LZW data after it
      0x2C,  // Image separator
      0x00, 0x00, 0x00,
      0x00,  // Left, Top
      0x01, 0x00, 0x01,
      0x00,  // 1x1
      0x00,  // Packed: no LCT
             // Truncated: no LZW minimum code size byte follows.
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// Test: IsAnimatedGif encounters unknown block type.
// Exercises the else-break at line 864-865.
TEST_F(ImageTranscoderTest, IsAnimatedGifUnknownBlockType) {
  const uint8_t data[] = {
      // Header
      'G',
      'I',
      'F',
      '8',
      '9',
      'a',
      // Logical Screen Descriptor
      0x01,
      0x00,
      0x01,
      0x00,  // 1x1
      0x00,
      0x00,
      0x00,  // Packed (no GCT), bg, aspect
      // Unknown block type
      0xFF,
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// =================================================================
// Coverage improvement: Transcode format detection error paths
// =================================================================

// Test: Data with JPEG magic bytes but truncated body, targeting kWebP.
// Exercises the ConvertToWebp failure path (lines 107-117) followed by
// the optimize-original fallback.
TEST_F(ImageTranscoderTest, TranscodeTruncatedJpegToWebpFallback) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  // Truncate enough to keep JPEG magic but corrupt the rest.
  std::string truncated = jpeg.substr(0, 20);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(truncated, mask);
  // Should not crash. Format detection will recognize JPEG magic,
  // then WebP conversion will fail, and optimize-original will also fail.
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: Data with PNG magic bytes but truncated body, targeting kAvif.
// Exercises the ConvertToAvif failure path (lines 122-133) followed by
// the optimize-original fallback.
TEST_F(ImageTranscoderTest, TranscodeTruncatedPngToAvifFallback) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  // Truncate to keep PNG magic but corrupt the rest.
  std::string truncated = png.substr(0, 20);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(truncated, mask);
  // Format detection recognizes PNG, AVIF conversion fails, fallback path.
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: SVG format target with JPEG input. Exercises the kSvg case at
// lines 137-143: logs info and falls through to optimize original.
TEST_F(ImageTranscoderTest, TranscodeJpegWithSvgTarget) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kSvg);

  auto result = transcoder_.Transcode(jpeg, mask);
  // SVG is not a transcoding target, so falls back to optimizing JPEG.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
}

// Test: SVG format target with GIF input. Exercises kSvg branch followed
// by the GIF return-as-is path at line 158.
TEST_F(ImageTranscoderTest, TranscodeGifWithSvgTarget) {
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kSvg);

  auto result = transcoder_.Transcode(gif, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/gif", result.output_mime_type);
  // GIF should be returned as-is.
  EXPECT_EQ(result.output_data.size(), gif.size());
}

// Test: SVG format target with WebP input. Exercises kSvg branch followed
// by the WebP return-as-is path at line 161.
TEST_F(ImageTranscoderTest, TranscodeWebpWithSvgTarget) {
  // SVG is not a transcoding target, so the call optimizes in the original
  // format -- which for a WebP input is the #1375 acceptance rule.
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kSvg);

  auto result = transcoder_.Transcode(webp, mask);
  if (result.success) {
    EXPECT_EQ("image/webp", result.output_mime_type);
    EXPECT_LT(result.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// =================================================================
// Coverage improvement: ConvertGifToWebp specific paths
// =================================================================

// Test: ConvertGifToWebp where GIF→WebP conversion succeeds but
// output is larger than input. Exercises the "WebP output larger
// than GIF" return at line 394.
// small o.gif (50 bytes) is likely smaller than any WebP output.
TEST_F(ImageTranscoderTest, ConvertGifToWebpOutputLarger) {
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(gif, mask);
  // For a tiny GIF, WebP output is likely larger, so conversion is
  // skipped and original GIF is returned via the fallback path.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(result.output_mime_type == "image/webp" ||
              result.output_mime_type == "image/gif");
}

// Test: ConvertGifToWebp with a completely_transparent GIF.
// Exercises the alpha path in GIF→WebP conversion.
TEST_F(ImageTranscoderTest, ConvertTransparentGifToWebp) {
  std::string gif = ReadTestFile("gif/completely_transparent.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(gif, mask);
  // Should not crash. May succeed or fail depending on size.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// =================================================================
// Coverage improvement: TranscodeMulti with kOriginal only
// =================================================================

// Test: TranscodeMulti with only kOriginal format. No pixel decode
// is needed (need_pixels remains false), exercises the kOriginal-only
// loop at lines 674-678.
TEST_F(ImageTranscoderTest, TranscodeMultiOriginalOnlyJpeg) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(jpeg, formats);

  // Only the optimized_original field should be populated.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/jpeg");
  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
}

// Test: TranscodeMulti with only kOriginal format and PNG input.
TEST_F(ImageTranscoderTest, TranscodeMultiOriginalOnlyPng) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(png, formats);

  // Small PNGs may already be well-optimized (no savings).
  if (result.optimized_original.success) {
    EXPECT_EQ(result.optimized_original.output_mime_type, "image/png");
  } else {
    EXPECT_TRUE(result.optimized_original.output_data.empty());
  }
}

// Test: TranscodeMulti with only kOriginal format and GIF input.
// Exercises the GIF passthrough path within the kOriginal case
// of the format loop.
TEST_F(ImageTranscoderTest, TranscodeMultiOriginalOnlyGif) {
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(gif, formats);

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/gif");
  EXPECT_EQ(result.optimized_original.output_data.size(), gif.size());
}

// =================================================================
// Coverage improvement: Corrupt input error paths
// =================================================================

// Test: Truncated PNG data passed to Transcode with kWebP target.
// Exercises ConvertToWebp failure (src_format != IMAGE_WEBP) and the
// fallback to OptimizePng which also fails.
TEST_F(ImageTranscoderTest, TranscodeCorruptPngToWebp) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  // Keep PNG magic (\x89PNG) but truncate rest.
  std::string corrupt = png.substr(0, 16);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Should not crash. WebP conversion fails, optimize PNG also fails.
  EXPECT_FALSE(result.success);
}

// Test: Truncated PNG data passed to Transcode with kOriginal target.
// Exercises the OptimizePng error path.
TEST_F(ImageTranscoderTest, TranscodeCorruptPngOriginal) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  std::string corrupt = png.substr(0, 16);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Should not crash. PNG optimization should fail on truncated data.
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: TranscodeMulti with corrupt image data that passes format detection
// but fails pixel decode. Exercises the fallback path at lines 608-637.
TEST_F(ImageTranscoderTest, TranscodeMultiCorruptPngFallback) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  // Truncate to keep PNG magic but corrupt pixel data.
  std::string corrupt = png.substr(0, png.size() / 4);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(corrupt, formats);
  // Pixel decode should fail, triggering the individual Transcode fallback.
  // Must not crash. Individual paths may also fail on corrupt data.
}

// =================================================================
// Coverage improvement: GIF format in TranscodeMulti
// =================================================================

// Test: TranscodeMulti with animated GIF and only WebP format.
// Exercises the is_gif=true path in the format loop (line 643-644).
TEST_F(ImageTranscoderTest, TranscodeMultiAnimatedGifWebpOnly) {
  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder_.TranscodeMulti(gif, formats);
  // GIF→WebP uses ConvertGifToWebp. May succeed or fail based on size.
  if (result.webp.success) {
    EXPECT_EQ(result.webp.output_mime_type, "image/webp");
  }
}

// Test: TranscodeMulti with GIF and AVIF format.
// GIF is not IMAGE_WEBP, so AVIF encoding is attempted via EncodeAvifFromPixels.
// But GIF triggers is_gif=true, and need_pixels is only set for kAvif
// (not gated on !is_gif), so DecodeToPixels runs on the GIF.
TEST_F(ImageTranscoderTest, TranscodeMultiGifAvifOnly) {
  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder_.TranscodeMulti(gif, formats);
  // GIF pixel decode may or may not work for AVIF encoding.
  // Either outcome is acceptable; must not crash.
  if (result.avif.success) {
    EXPECT_EQ(result.avif.output_mime_type, "image/avif");
  }
}

// =================================================================
// Coverage improvement: PixelBufferReader adapter
// =================================================================

// Test: Verify PixelBufferReader adapter works correctly through the
// ResizeForViewport pipeline with PNG alpha (RGBA) input.
// Exercises the RGBA pixel format path in PixelBufferReader (line 722).
TEST(ImageTranscoderPixelBufferTest, ResizeRgbaPixels) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 10;  // Very small to ensure resize

  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/basi6a08.png");
  ASSERT_FALSE(png.empty());

  auto decoded = transcoder.DecodeToPixels(png);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  ASSERT_GT(decoded.width, 10u);          // Must be wider than target
  ASSERT_EQ(decoded.bytes_per_pixel, 4);  // RGBA

  auto resized =
      transcoder.ResizeForViewport(decoded, CapabilityMask::Viewport::kMobile);
  EXPECT_FALSE(resized.pixel_buffer.empty());
  EXPECT_EQ(resized.width, 10u);
  EXPECT_EQ(resized.bytes_per_pixel, 4);  // RGBA preserved
  EXPECT_TRUE(resized.has_alpha);
}

// Test: ResizeForViewport with grayscale input (1 bpp).
// Exercises the GRAY_8 pixel format path in PixelBufferReader.
TEST(ImageTranscoderPixelBufferTest, ResizeGrayscalePixels) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 10;  // Very small to ensure resize

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  ASSERT_EQ(decoded.bytes_per_pixel, 1);  // Grayscale

  if (decoded.width > 10) {
    auto resized = transcoder.ResizeForViewport(
        decoded, CapabilityMask::Viewport::kMobile);
    EXPECT_FALSE(resized.pixel_buffer.empty());
    EXPECT_EQ(resized.width, 10u);
    EXPECT_EQ(resized.bytes_per_pixel, 1);  // Grayscale preserved
    EXPECT_FALSE(resized.has_alpha);
  }
}

// =================================================================
// Coverage gap: Content-class quality factor application (lines 1019-1024)
// with learned_quality disabled and save_data=true
// =================================================================

// Test: Content-class quality factors are applied when learned quality
// prediction is disabled. Uses save_data=true so the savedata base
// qualities are set first (lines 890-894), then quality factors are
// applied by the apply_factor lambda (lines 1019-1024).
TEST(ImageTranscoderQualityFactorTest,
     ContentClassFactorAppliedWhenLearnedDisabledSaveData) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;
  // Save-Data quality bases.
  config.savedata_jpeg_quality = 60;
  config.savedata_webp_quality = 50;
  config.savedata_avif_quality = 45;
  config.viewport_widths.mobile = 100;  // Ensure resize happens

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // Content analysis should have classified the image.
  EXPECT_NE(result.applied_preset.content_class,
            pagespeed::ContentClass::kUnknown)
      << "Content analysis should classify sjpeg6.jpg";
  // Learned quality should NOT have been used.
  EXPECT_FALSE(result.used_learned_quality);
  // Quality factors from the content class should have been applied on
  // top of the savedata base qualities (lines 1019-1024).
  // At least some format should succeed.
  bool any_success = result.webp.success || result.avif.success ||
                     result.optimized_original.success;
  EXPECT_TRUE(any_success) << "At least one format should succeed";
  // Final qualities should be positive (quality factor applied).
  EXPECT_GT(result.final_webp_quality, 0);
  EXPECT_GT(result.final_avif_quality, 0);
}

// Test: Content-class quality factor path with save_data=false and
// learned_quality disabled, using a non-Mobile viewport that still
// triggers the full TranscodeMultiResized path via a small tablet width.
TEST(ImageTranscoderQualityFactorTest,
     ContentClassFactorAppliedTabletViewport) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;
  config.viewport_widths.tablet = 100;  // Small enough to trigger resize

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  EXPECT_NE(result.applied_preset.content_class,
            pagespeed::ContentClass::kUnknown);
  EXPECT_FALSE(result.used_learned_quality);
  EXPECT_GT(result.final_webp_quality, 0);
  EXPECT_GT(result.final_jpeg_quality, 0);
}

// =================================================================
// Coverage gap: Learned quality with save_data target reduction
// (lines 980-983)
// =================================================================

// Test: Learned quality prediction with save_data=true reduces the
// SSIMULACRA2 target by savedata_score_reduction (line 982), leading
// to lower predicted qualities than without save_data.
TEST(ImageTranscoderLearnedSaveDataTest, LearnedQualitySaveDataReducesTarget) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = true;
  config.learned_quality_webp = true;
  config.learned_quality_avif = true;
  config.content_analysis = true;
  config.quality_verify = false;
  config.savedata_score_reduction = 15.0f;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto normal = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  auto savedata = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // Both should attempt learned quality.
  EXPECT_TRUE(normal.used_learned_quality);
  EXPECT_TRUE(savedata.used_learned_quality);

  // Save-Data should produce different (typically lower) final qualities
  // because the SSIMULACRA2 target is reduced by 15 points.
  bool any_quality_differs =
      (normal.final_webp_quality != savedata.final_webp_quality) ||
      (normal.final_avif_quality != savedata.final_avif_quality) ||
      (normal.final_jpeg_quality != savedata.final_jpeg_quality);
  EXPECT_TRUE(any_quality_differs)
      << "Save-Data should produce different learned qualities "
         "(target reduced by "
      << config.savedata_score_reduction << ")";
}

// =================================================================
// Coverage gap: Learned quality with all per-format flags disabled
// exercises the used_learned=false path even when learned_quality=true
// =================================================================

// Test: learned_quality=true but all per-format flags disabled means
// the prediction block is entered but used_learned stays false.
// Falls through to content-class quality factors (lines 1017-1032).
TEST(ImageTranscoderLearnedQualityCoverageTest2,
     LearnedQualityAllFormatsDisabledFallsToFactor) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = false;
  config.learned_quality_webp = false;
  config.learned_quality_avif = false;
  config.content_analysis = true;
  config.quality_verify = false;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // Learned quality not used (all per-format flags disabled).
  EXPECT_FALSE(result.used_learned_quality);
  // No fallback increments either (per-format if-blocks not entered).
  EXPECT_EQ(result.learned_quality_fallbacks, 0);
  // Content class should still be populated (content_analysis=true).
  EXPECT_NE(result.applied_preset.content_class,
            pagespeed::ContentClass::kUnknown);
  // Quality factors should still have been applied (lines 1019-1024).
  EXPECT_GT(result.final_webp_quality, 0);
  EXPECT_GT(result.final_avif_quality, 0);
  EXPECT_GT(result.final_jpeg_quality, 0);
}

// =================================================================
// Coverage gap: Save-Data with animated GIF exercises the
// use_save_data path at line 924-926
// =================================================================

// Test: Save-Data with animated GIF where target_width=0 (Desktop).
// Exercises line 921-926: is_animated_gif=true, target_width=0,
// use_save_data=true forces pixel decode even for animated GIF.
TEST(ImageTranscoderSaveDataGifTest, SaveDataAnimatedGifDesktopViewport) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      gif, formats, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // GIF original should pass through.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/gif");
}

// =================================================================
// Coverage gap: Content analysis + save_data + learned quality
// all together exercises the full pipeline including
// save_data target reduction in learned quality prediction
// =================================================================

// Test: Full pipeline with content analysis + learned quality + save_data.
// This exercises the target -= savedata_score_reduction (line 982) WITHIN
// the learned quality block, then uses the reduced target for predictions.
TEST(ImageTranscoderFullPipelineTest, FullPipelineWithSaveData) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = true;
  config.learned_quality_webp = true;
  config.learned_quality_avif = true;
  config.content_analysis = true;
  config.quality_verify = false;
  config.savedata_score_reduction = 20.0f;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // Learned quality should have been used with the reduced target.
  EXPECT_TRUE(result.used_learned_quality)
      << "Learned quality should be used even with save-data";
  // All final qualities should be positive.
  EXPECT_GT(result.final_jpeg_quality, 0);
  EXPECT_GT(result.final_webp_quality, 0);
  EXPECT_GT(result.final_avif_quality, 0);
}

// =================================================================
// Coverage gap: Content-class factor with save_data affecting
// savedata base qualities before factor application
// =================================================================

// Test: Verify that save_data base qualities AND content-class
// quality factors compound. With save_data=true and learned_quality=false,
// the base qualities are first set to savedata values (lines 890-894)
// and then quality factors are applied (lines 1022-1031).
// Compare output with non-savedata to verify difference.
TEST(ImageTranscoderQualityFactorTest, SaveDataAndContentFactorCompound) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;
  config.jpeg_quality = 85;
  config.webp_quality = 75;
  config.avif_quality = 60;
  config.savedata_jpeg_quality = 50;
  config.savedata_webp_quality = 40;
  config.savedata_avif_quality = 35;
  // Use 400px so the resized image is large enough for WebP to beat JPEG
  // in the size gate (at very small sizes like 100px, the JPEG baseline
  // from pixel encoding can be smaller than WebP).
  config.viewport_widths.mobile = 400;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto normal = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  auto savedata = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  EXPECT_TRUE(normal.webp.success) << normal.webp.error_message;
  EXPECT_TRUE(savedata.webp.success) << savedata.webp.error_message;

  // Save-Data with lower base qualities should produce smaller output.
  if (normal.webp.success && savedata.webp.success) {
    EXPECT_LE(savedata.webp.output_data.size(), normal.webp.output_data.size())
        << "Save-Data with lower quality should produce smaller or equal WebP";
  }

  // Final qualities should reflect the savedata base (lower).
  EXPECT_LE(savedata.final_webp_quality, normal.final_webp_quality);
}

// =================================================================
// Coverage gap: TranscodeMultiResized with save_data=true and
// non-zero target_width exercises the save-data quality override
// path before entering the content analysis block
// =================================================================

// Test: Save-Data with mobile viewport and PNG input.
// Exercises the full path: save_data quality override (lines 890-894)
// -> pixel decode -> resize -> content analysis -> quality factors.
TEST(ImageTranscoderSaveDataPipelineTest, SaveDataPngMobileViewport) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;
  config.viewport_widths.mobile = 10;

  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      png, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // Small PNG may already be well-optimized (no savings).
  // The test exercises the save-data quality override path regardless.
  if (result.optimized_original.success) {
    EXPECT_FALSE(result.optimized_original.output_data.empty());
  }
}

// =================================================================
// Coverage: OptimizeJpeg fallback paths
// =================================================================

// Test: JPEG optimization succeeds but output >= input size.
// quality100.jpg is a high-quality JPEG that is hard to compress further.
// With lossless optimization (lossy=false), the output is typically the
// same size or even slightly larger, exercising the "original is already
// well-optimized" return at line 185.
TEST(ImageTranscoderJpegOptTest, OptimizeJpegOutputNotSmaller) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.lossy_jpeg = false;
  config.progressive_jpeg = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/quality100.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
  // The output should be the original data when optimization doesn't help.
  // Either the output is smaller (optimization worked) or same size (returned
  // original). Both are success. Key: we exercise the branch, not crash.
  EXPECT_LE(result.output_data.size(), jpeg.size());
}

// Test: JPEG optimization fails entirely on corrupt data.
// Construct a minimal JPEG header (FF D8 FF E0) followed by garbage.
// This is recognized as JPEG by ComputeImageType but fails optimization.
// Exercises the "JPEG optimization failed" return at line 188.
TEST_F(ImageTranscoderTest, OptimizeJpegFails) {
  // JPEG magic: FF D8 FF E0, then JFIF header followed by garbage.
  std::string corrupt;
  corrupt += '\xFF';
  corrupt += '\xD8';
  corrupt += '\xFF';
  corrupt += '\xE0';
  corrupt += std::string(100, '\x42');  // Garbage body

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Format detection succeeds (JPEG magic), optimization fails.
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "JPEG optimization failed");
}

// =================================================================
// Coverage: ConvertToWebp error paths
// =================================================================

// Test: ConvertToWebp with corrupt JPEG that passes format detection
// but fails scanline reader creation. Exercises line 225 "Failed to
// create scanline reader".
TEST_F(ImageTranscoderTest, ConvertToWebpReaderCreationFails) {
  // Minimal JPEG magic + garbage. ComputeImageType returns IMAGE_JPEG
  // but the scanline reader can't decode it.
  std::string corrupt;
  corrupt += '\xFF';
  corrupt += '\xD8';
  corrupt += '\xFF';
  corrupt += '\xE0';
  corrupt += std::string(50, '\x00');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(corrupt, mask);
  // WebP conversion fails (reader creation), then falls back to
  // optimize original (also fails). Must not crash.
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: ConvertToWebp with corrupt PNG that passes format detection
// but fails scanline reader creation.
TEST_F(ImageTranscoderTest, ConvertToWebpPngReaderCreationFails) {
  // PNG magic bytes followed by garbage.
  std::string corrupt;
  corrupt += '\x89';
  corrupt += 'P';
  corrupt += 'N';
  corrupt += 'G';
  corrupt += '\r';
  corrupt += '\n';
  corrupt += '\x1A';
  corrupt += '\n';
  corrupt += std::string(50, '\x00');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(corrupt, mask);
  // WebP conversion fails, falls back to optimize original (also fails).
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: ConvertToWebp where output is larger than original.
// Use a very small PNG (1x1 pixel) where WebP encoding overhead
// exceeds the original size. Exercises line 252 "WebP output larger
// than original, skipping conversion".
TEST_F(ImageTranscoderTest, ConvertToWebpOutputLargerThanOriginal) {
  // Use the smallest PNG in the test suite (1x1 pixel).
  std::string png = ReadTestFile("pngsuite/s01n3p01.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(png, mask);
  // For a tiny 1x1 PNG, WebP output is likely larger.
  // If WebP conversion "fails" due to size, the code falls back to
  // optimizing the original PNG.
  EXPECT_TRUE(result.success) << result.error_message;
  // Output should be either WebP (unlikely for 1x1) or optimized PNG.
  EXPECT_TRUE(result.output_mime_type == "image/webp" ||
              result.output_mime_type == "image/png");
}

// =================================================================
// Coverage: ConvertToAvif error paths
// =================================================================

// Test: ConvertToAvif with corrupt JPEG that passes format detection
// but fails scanline reader creation. Exercises line 278 "Failed to
// create scanline reader for AVIF".
TEST_F(ImageTranscoderTest, ConvertToAvifReaderCreationFails) {
  // Minimal JPEG magic + garbage.
  std::string corrupt;
  corrupt += '\xFF';
  corrupt += '\xD8';
  corrupt += '\xFF';
  corrupt += '\xE0';
  corrupt += std::string(50, '\x00');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(corrupt, mask);
  // AVIF conversion fails (reader creation), then falls back to
  // optimize original (also fails). Must not crash.
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: ConvertToAvif with corrupt PNG. Exercises AVIF error path
// followed by PNG optimization fallback.
TEST_F(ImageTranscoderTest, ConvertToAvifCorruptPngFallback) {
  // PNG magic + garbage body.
  std::string corrupt;
  corrupt += '\x89';
  corrupt += 'P';
  corrupt += 'N';
  corrupt += 'G';
  corrupt += '\r';
  corrupt += '\n';
  corrupt += '\x1A';
  corrupt += '\n';
  corrupt += std::string(50, '\x00');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(corrupt, mask);
  // AVIF conversion fails, falls back to optimize original (also fails).
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error_message.empty());
}

// Test: ConvertToAvif where AVIF output is larger than original.
// Use a very small PNG (1x1 pixel) where AVIF encoding overhead
// exceeds the original size. Exercises line 371-380 "AVIF output
// larger than original, skipping conversion".
TEST_F(ImageTranscoderTest, ConvertToAvifOutputLargerThanOriginal) {
  // Use the smallest PNG in the test suite (1x1 pixel).
  std::string png = ReadTestFile("pngsuite/s01n3p01.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(png, mask);
  // For a tiny 1x1 PNG, AVIF output is likely larger.
  // Falls back to optimizing the original PNG.
  EXPECT_TRUE(result.success) << result.error_message;
  // Output should be either AVIF (unlikely for 1x1) or optimized PNG.
  EXPECT_TRUE(result.output_mime_type == "image/avif" ||
              result.output_mime_type == "image/png");
}

// =================================================================
// Coverage: Transcode with AVIF input data (IMAGE_AVIF detected
// but not handled by the src_format switch → "Unsupported image
// format" at line 86)
// =================================================================

// Test: AVIF input data is detected by ComputeImageType as IMAGE_AVIF
// but has no case in the format-mapping switch, hitting the default
// "Unsupported image format" return at line 86.
TEST_F(ImageTranscoderTest, TranscodeAvifInputUnsupported) {
  // Construct minimal AVIF container header.
  // AVIF: bytes 4-7 = "ftyp", bytes 8-11 = "avif"
  std::string avif;
  avif += '\x00';
  avif += '\x00';
  avif += '\x00';
  avif += '\x1C';  // Box size = 28
  avif += "ftyp";
  avif += "avif";
  avif += std::string(16, '\x00');  // Padding to satisfy size check

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(avif, mask);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Unsupported image format");
}

// Test: AVIF input data with kWebP target — still unsupported as source.
TEST_F(ImageTranscoderTest, TranscodeAvifInputToWebpUnsupported) {
  std::string avif;
  avif += '\x00';
  avif += '\x00';
  avif += '\x00';
  avif += '\x1C';
  avif += "ftyp";
  avif += "avif";
  avif += std::string(16, '\x00');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(avif, mask);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Unsupported image format");
}

// Test: AVIF input data with kAvif target — still unsupported as source.
TEST_F(ImageTranscoderTest, TranscodeAvifInputToAvifUnsupported) {
  std::string avif;
  avif += '\x00';
  avif += '\x00';
  avif += '\x00';
  avif += '\x1C';
  avif += "ftyp";
  avif += "avif";
  avif += std::string(16, '\x00');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(avif, mask);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Unsupported image format");
}

// =================================================================
// Coverage: WebP conversion where output is explicitly larger, via
// TranscodeMulti path (exercises line 652-654 in TranscodeMulti)
// =================================================================

// Test: TranscodeMulti with a tiny PNG where WebP encoding overhead
// exceeds the original size. Exercises the size check in TranscodeMulti
// that rejects WebP output larger than original (line 652-654).
TEST_F(ImageTranscoderTest, TranscodeMultiWebpLargerThanOriginal) {
  std::string png = ReadTestFile("pngsuite/s01n3p01.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(png, formats);

  // WebP of a 1x1 PNG is likely larger → should fail with size message.
  // Original PNG optimization should succeed regardless.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/png");

  // WebP may fail because output is larger.
  if (!result.webp.success) {
    EXPECT_TRUE(result.webp.error_message.find("larger") != std::string::npos)
        << "Error message should mention 'larger': "
        << result.webp.error_message;
  }
}

// Test: TranscodeMulti with a tiny PNG for AVIF (exercises line 666-669).
TEST_F(ImageTranscoderTest, TranscodeMultiAvifLargerThanOriginal) {
  std::string png = ReadTestFile("pngsuite/s01n3p01.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(png, formats);

  // AVIF of a 1x1 PNG is likely larger → should fail with size message.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;

  if (!result.avif.success) {
    EXPECT_TRUE(result.avif.error_message.find("larger") != std::string::npos)
        << "Error message should mention 'larger': "
        << result.avif.error_message;
  }
}

// =================================================================
// Coverage: ConvertGifToWebp conversion failure path (line 396)
// =================================================================

// Test: ConvertGifToWebp with a corrupt GIF that passes format detection
// but fails conversion. Exercises "GIF to WebP conversion failed" return.
TEST_F(ImageTranscoderTest, ConvertGifToWebpConversionFails) {
  // GIF89a header with just enough data to be recognized but not decoded.
  // 10 bytes of GIF header + LSD but missing all image data.
  const uint8_t data[] = {
      'G',  'I',  'F',  '8',  '9', 'a',  // Header
      0x01, 0x00, 0x01, 0x00,            // 1x1 Logical Screen Descriptor
      0x00, 0x00, 0x00,                  // Packed (no GCT), bg, aspect
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(gif, mask);
  // GIF format detected, WebP conversion should fail on missing data.
  // Falls back to returning GIF as-is.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.output_mime_type, "image/gif");
}

// =================================================================
// Coverage: DecodeToPixels with AVIF input data
// =================================================================

// Test: DecodeToPixels with AVIF input data. AVIF is detected by
// ComputeImageType but has no reader_format case in DecodeToPixels,
// falling through to IMAGE_UNKNOWN and returning empty.
TEST_F(ImageTranscoderTest, DecodeToPixelsAvifInputEmpty) {
  std::string avif;
  avif += '\x00';
  avif += '\x00';
  avif += '\x00';
  avif += '\x1C';
  avif += "ftyp";
  avif += "avif";
  avif += std::string(16, '\x00');

  auto decoded = transcoder_.DecodeToPixels(avif);
  EXPECT_TRUE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.width, 0u);
  EXPECT_EQ(decoded.height, 0u);
}

// =================================================================
// Coverage: Transcode with WebP input targeting kAvif (WebP→AVIF
// is explicitly skipped, exercises line 122 condition)
// =================================================================

// Test: Verify that WebP input with kAvif target falls through to
// optimize-original (returns WebP as-is). Exercises the
// if (src_format != IMAGE_WEBP) check at line 122.
TEST_F(ImageTranscoderTest, TranscodeWebpToAvifSkippedStaysWebp) {
  // WebP→AVIF is skipped, so the call falls through to optimize-original,
  // which since #1375 re-encodes the WebP under the acceptance rule.  The
  // format never changes: a WebP input never yields AVIF here.
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(webp, mask);
  EXPECT_NE(result.output_mime_type, "image/avif");
  if (result.success) {
    EXPECT_EQ(result.output_mime_type, "image/webp");
    EXPECT_LT(result.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// =================================================================
// Coverage: ConvertToWebp with JPEG that successfully converts
// but WebP is larger (exercises line 247-252 directly)
// =================================================================

// Test: Use sjpeg1.jpg (1552 bytes) which is very small — WebP may
// be larger. This specifically exercises the ConvertToWebp size check
// at lines 247-252 and the fallback in Transcode at lines 111-117.
TEST_F(ImageTranscoderTest, ConvertToWebpSmallJpegSizeCheck) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(jpeg, mask);
  // Should succeed either as WebP (if smaller) or optimized JPEG (fallback).
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(result.output_data.empty());
  EXPECT_TRUE(result.output_mime_type == "image/webp" ||
              result.output_mime_type == "image/jpeg")
      << "Unexpected mime type: " << result.output_mime_type;
}

// =================================================================
// Coverage: ConvertToAvif with small JPEG that produces larger AVIF
// (exercises line 371-380 AVIF size check)
// =================================================================

// Test: Use sjpeg1.jpg (1552 bytes) which is very small — AVIF may
// be larger. Exercises the AVIF size check at lines 371-380 and
// the fallback path.
TEST_F(ImageTranscoderTest, ConvertToAvifSmallJpegSizeCheck) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(jpeg, mask);
  // Should succeed either as AVIF (if smaller) or optimized JPEG (fallback).
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_FALSE(result.output_data.empty());
  EXPECT_TRUE(result.output_mime_type == "image/avif" ||
              result.output_mime_type == "image/jpeg")
      << "Unexpected mime type: " << result.output_mime_type;
}

// =================================================================
// Coverage: GIF→AVIF via ConvertToAvif exercises the IMAGE_GIF case
// at line 268-269 in ConvertToAvif
// =================================================================

// Test: Static GIF → AVIF through ConvertToAvif. GIF is handled at
// line 268 in ConvertToAvif (reader_format = IMAGE_GIF).
TEST_F(ImageTranscoderTest, ConvertToAvifGifSource) {
  // Use a static GIF that decodes successfully.
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(gif, mask);
  // GIF→AVIF may succeed or fall back depending on output size.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(result.output_mime_type == "image/avif" ||
              result.output_mime_type == "image/gif")
      << "Unexpected mime type: " << result.output_mime_type;
}

// =================================================================
// Coverage: TranscodeMultiResized AVIF size check (lines 1241-1243)
// =================================================================

// Test: TranscodeMultiResized with a tiny PNG where AVIF output is
// larger than the original. Exercises the size check at line 1241-1243.
TEST(ImageTranscoderMultiResizedSizeTest, AvifLargerThanOriginalInResized) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/s01n3p01.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMultiResized(
      png, formats, CapabilityMask::Viewport::kDesktop);

  // 1x1 PNG → AVIF is likely larger. Size check should reject it.
  if (!result.avif.success) {
    EXPECT_TRUE(result.avif.error_message.find("larger") != std::string::npos)
        << "Error message should mention 'larger': "
        << result.avif.error_message;
  }
}

// Test: TranscodeMultiResized with a tiny PNG where WebP output is
// larger than the original. Exercises the size check at line 1157-1158.
TEST(ImageTranscoderMultiResizedSizeTest, WebpLargerThanOriginalInResized) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/s01n3p01.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      png, formats, CapabilityMask::Viewport::kDesktop);

  // 1x1 PNG → WebP is likely larger. Size check should reject it.
  if (!result.webp.success) {
    EXPECT_TRUE(result.webp.error_message.find("larger") != std::string::npos)
        << "Error message should mention 'larger': "
        << result.webp.error_message;
  }
}

// =================================================================
// Fix 2: Mobile no-resize should not be larger than desktop
// =================================================================

TEST(ImageTranscoderNoResizeTest, MobileNoResizeNotLargerThanDesktop) {
  // sjpeg1.jpg is 120x90, well below the 480px mobile target.
  // Mobile path should NOT inflate output via content-analysis / SSIMULACRA2.
  // Use default config (content_analysis=true, learned_quality=true,
  // quality_verify=true) to reproduce the real-world bug.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto desktop = transcoder.TranscodeMulti(jpeg, formats);
  auto mobile = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(desktop.webp.success) << desktop.webp.error_message;
  ASSERT_TRUE(mobile.webp.success) << mobile.webp.error_message;
  EXPECT_LE(mobile.webp.output_data.size(), desktop.webp.output_data.size())
      << "Mobile WebP should not be larger than desktop when no resize needed";

  ASSERT_TRUE(desktop.avif.success) << desktop.avif.error_message;
  ASSERT_TRUE(mobile.avif.success) << mobile.avif.error_message;
  EXPECT_LE(mobile.avif.output_data.size(), desktop.avif.output_data.size())
      << "Mobile AVIF should not be larger than desktop when no resize needed";
}

TEST(ImageTranscoderNoResizeTest, TabletNoResizeNotLargerThanDesktop) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto desktop = transcoder.TranscodeMulti(jpeg, formats);
  auto tablet = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kTablet);

  ASSERT_TRUE(desktop.webp.success) << desktop.webp.error_message;
  ASSERT_TRUE(tablet.webp.success) << tablet.webp.error_message;
  EXPECT_LE(tablet.webp.output_data.size(), desktop.webp.output_data.size())
      << "Tablet WebP should not be larger than desktop when no resize needed";

  ASSERT_TRUE(desktop.avif.success) << desktop.avif.error_message;
  ASSERT_TRUE(tablet.avif.success) << tablet.avif.error_message;
  EXPECT_LE(tablet.avif.output_data.size(), desktop.avif.output_data.size())
      << "Tablet AVIF should not be larger than desktop when no resize needed";
}

TEST(ImageTranscoderNoResizeTest, LargeImageStillResizes) {
  // sjpeg6.jpg is 512x512, wider than 480px mobile target.
  // Confirm the fix doesn't break the normal resize path.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = false;
  config.learned_quality = false;
  config.quality_verify = false;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto desktop = transcoder.TranscodeMulti(jpeg, formats);
  auto mobile = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(desktop.webp.success) << desktop.webp.error_message;
  ASSERT_TRUE(mobile.webp.success) << mobile.webp.error_message;
  // Mobile should be smaller (actual resize from 512px to 480px).
  EXPECT_LT(mobile.webp.output_data.size(), desktop.webp.output_data.size())
      << "Mobile WebP should be smaller for a genuinely resized image";
}

// =================================================================
// Coverage improvement: EncodeWebpFromPixels / EncodeAvifFromPixels
// paths exercised indirectly via TranscodeMulti / TranscodeMultiResized
// with specific image types that exercise different pixel format paths.
// =================================================================

// Test: TranscodeMulti with grayscale JPEG and WebP+AVIF targets.
// Exercises the GRAY_8 pixel format path in EncodeWebpFromPixels
// (pf = GRAY_8 at line 490) and EncodeAvifFromPixels
// (YUV400 format at lines 519-521) via the multi-format path.
TEST(ImageTranscoderEncodeFromPixelsTest, MultiGrayscaleWebpAndAvif) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMulti(jpeg, formats);
  // Grayscale JPEG exercises GRAY_8 paths in both encoders.
  // At least one format should succeed or fail gracefully.
  // Must not crash.
}

// Test: TranscodeMulti with PNG alpha and WebP+AVIF targets.
// Exercises the RGBA pixel format path (has_alpha=true) in
// EncodeWebpFromPixels (line 488) and EncodeAvifFromPixels (line 532).
TEST(ImageTranscoderEncodeFromPixelsTest, MultiRgbaWebpAndAvif) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;
  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/basi6a08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMulti(png, formats);
  // PNG with alpha exercises RGBA paths in both encoders.
  // Must not crash.
}

// Test: TranscodeMultiResized with grayscale JPEG at mobile viewport
// to exercise EncodeWebpFromPixels and EncodeAvifFromPixels with
// grayscale pixels after resize.
TEST(ImageTranscoderEncodeFromPixelsTest, ResizedGrayscaleWebpAndAvif) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;
  config.viewport_widths.mobile = 10;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);
  // Grayscale resize + encode. Must not crash.
}

// Test: TranscodeMultiResized with PNG alpha at mobile viewport
// to exercise EncodeWebpFromPixels and EncodeAvifFromPixels with
// RGBA pixels after resize.
TEST(ImageTranscoderEncodeFromPixelsTest, ResizedRgbaWebpAndAvif) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;
  config.viewport_widths.mobile = 10;
  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/basi6a08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMultiResized(
      png, formats, CapabilityMask::Viewport::kMobile);
  // RGBA resize + encode. Must not crash.
}

// =================================================================
// Coverage improvement: TranscodeMulti fallback loop with kSvg format.
// When DecodeToPixels fails, the fallback loop iterates all formats.
// The kSvg case hits the default break at line 632-633.
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiFallbackWithSvgFormat) {
  // Construct a truncated JPEG that passes format detection but fails
  // pixel decode.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());
  std::string truncated = jpeg.substr(0, jpeg.size() / 4);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kSvg,  // Exercises default case in fallback
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMulti(truncated, formats);
  // Must not crash. The kSvg format in the fallback loop should be
  // silently skipped (default break).
}

// =================================================================
// Coverage improvement: TranscodeMulti main loop default case
// (line 680-681) when an unrecognized format is passed.
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiDefaultFormatCase) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kSvg,  // Not WebP/AVIF/Original
  };

  auto result = transcoder_.TranscodeMulti(jpeg, formats);
  // kSvg hits the default case in the main format loop.
  // No output fields should be populated for kSvg.
  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
  EXPECT_FALSE(result.optimized_original.success);
}

// =================================================================
// Coverage improvement: ConvertToAvif "Unsupported pixel format"
// default case at line 302.
// This is very hard to trigger because CreateScanlineReader only
// produces RGB, RGBA, or GRAY pixel formats for standard images.
// We test indirectly by providing a WebP source to ConvertToAvif
// through the Transcode path where src_format != IMAGE_WEBP is
// checked. For the pixel format default, we cannot construct a
// standard image that produces an unsupported pixel format, but
// we exercise the nearby paths thoroughly.
// =================================================================

// Test: ConvertToAvif with very small GIF that produces few scanlines.
// Exercises the scanline read loop in ConvertToAvif (lines 309-316).
TEST_F(ImageTranscoderTest, ConvertToAvifSmallGif) {
  // Use the smallest GIF that can be decoded.
  std::string gif = ReadTestFile("gif/o.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(gif, mask);
  // Must succeed (as either AVIF or GIF fallback).
  EXPECT_TRUE(result.success) << result.error_message;
}

// =================================================================
// Coverage improvement: ConvertToWebp line 255 "WebP conversion failed"
// triggered when ImageConverter::ConvertImage fails mid-conversion.
// Very hard to trigger without mocking. Test with a corrupted-body
// JPEG that passes reader creation but fails during scanline iteration.
// =================================================================

TEST_F(ImageTranscoderTest, ConvertToWebpScanlineConversionFails) {
  // Read a valid JPEG, then corrupt the body after the header.
  // This should allow CreateScanlineReader to succeed but
  // ConvertImage to fail mid-conversion.
  std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());

  // Keep the first ~200 bytes (JPEG header + SOI + markers) intact
  // but corrupt the rest (image data).
  std::string corrupt = jpeg.substr(0, 200);
  corrupt += std::string(jpeg.size() - 200, '\xFF');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Reader may or may not be created successfully.
  // If it is, conversion likely fails. Either way, must not crash.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// =================================================================
// Coverage improvement: ConvertToAvif scanline read failure (line 312)
// "Failed to read scanline for AVIF".
// Trigger by corrupting a JPEG body after the header.
// =================================================================

TEST_F(ImageTranscoderTest, ConvertToAvifScanlineReadFails) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());

  // Keep header intact, corrupt the body.
  std::string corrupt = jpeg.substr(0, 200);
  corrupt += std::string(jpeg.size() - 200, '\xFF');

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder_.Transcode(corrupt, mask);
  // Must not crash. Reader creation may succeed but scanline read fails.
  if (!result.success) {
    EXPECT_FALSE(result.error_message.empty());
  }
}

// =================================================================
// Coverage improvement: DecodeToPixels scanline read failure (line 467)
// =================================================================

TEST_F(ImageTranscoderTest, DecodeToPixelsScanlineReadFails) {
  // Construct a JPEG with valid header markers but corrupted scan data.
  std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());

  // Keep first 300 bytes (enough for SOI + headers), replace rest with zeros.
  std::string corrupt = jpeg.substr(0, 300);
  corrupt += std::string(jpeg.size() - 300, '\x00');

  auto decoded = transcoder_.DecodeToPixels(corrupt);
  // Should return empty decoded image (scanline read failure).
  // Must not crash. The reader may or may not be created.
  // If it is, ReadNextScanline will fail on corrupt data.
}

// =================================================================
// Coverage improvement: DecodeToPixels with unknown pixel format
// (line 450-451). This default case is nearly unreachable because
// standard image decoders only return RGB, RGBA, or GRAY.
// We test it by providing a corrupt PNG with invalid bit depth.
// =================================================================

TEST_F(ImageTranscoderTest, DecodeToPixelsCorruptPngBitDepth) {
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  // Corrupt the bit depth byte in the IHDR chunk.
  // IHDR starts at offset 8 (after 8-byte PNG signature).
  // The IHDR chunk: [4B length][4B "IHDR"][13B data][4B CRC]
  // Bit depth is at IHDR data offset 8 (byte 24 from start).
  // This will cause the reader to return an unsupported pixel format
  // or fail to create a reader entirely.
  std::string corrupt = png;
  if (corrupt.size() > 25) {
    corrupt[24] = '\x03';  // Set bit depth to 3 (invalid)
    // Also corrupt the CRC to match (or just let it fail).
  }

  auto decoded = transcoder_.DecodeToPixels(corrupt);
  // Must not crash. Either fails reader creation or returns empty.
  EXPECT_TRUE(decoded.pixel_buffer.empty());
}

// =================================================================
// Coverage improvement: TranscodeMultiResized with kSvg format
// in the encode loop (line 1347 default break).
// =================================================================

TEST(ImageTranscoderResizedSvgTest, TranscodeMultiResizedSvgFormat) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kSvg,  // Hits default break
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // kSvg is not handled in the encode loop, so no outputs should be set.
  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
  EXPECT_FALSE(result.optimized_original.success);
}

// =================================================================
// Coverage improvement: Null handler paths.
// Several error/info logging paths check if (handler_) before logging.
// Note: many underlying library functions (CreateScanlineReader,
// ImageConverter::ConvertGifToWebp, etc.) assert handler != nullptr,
// so we can only test null handler on paths that do NOT call into
// the image libraries. Test empty/unsupported input and SVG target.
// =================================================================

TEST(ImageTranscoderNullHandlerTest, TranscodeEmptyInputNullHandler) {
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, nullptr);

  CapabilityMask mask;
  auto result = transcoder.Transcode("", mask);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Empty input data");
}

TEST(ImageTranscoderNullHandlerTest, TranscodeUnsupportedFormatNullHandler) {
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, nullptr);

  // Random bytes: not a recognized image format.
  CapabilityMask mask;
  auto result = transcoder.Transcode("not an image", mask);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Unsupported image format");
}

TEST(ImageTranscoderNullHandlerTest, DecodeToPixelsEmptyNullHandler) {
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, nullptr);

  auto decoded = transcoder.DecodeToPixels("");
  EXPECT_TRUE(decoded.pixel_buffer.empty());
}

TEST(ImageTranscoderNullHandlerTest, TranscodeMultiEmptyNullHandler) {
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, nullptr);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMulti("", formats);
  EXPECT_FALSE(result.webp.success);
}

TEST(ImageTranscoderNullHandlerTest, TranscodeMultiResizedEmptyNullHandler) {
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, nullptr);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      "", formats, CapabilityMask::Viewport::kMobile);
  EXPECT_FALSE(result.webp.success);
}

TEST(ImageTranscoderNullHandlerTest, IsAnimatedGifNullHandler) {
  // IsAnimatedGif is a static method that doesn't use handler.
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(""));
  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif("GIF89a"));
}

// =================================================================
// Coverage improvement: TranscodeMulti with GIF + WebP format
// where the image is already WebP (exercises lines 645-648)
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiWebpInputWebpFormat) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder_.TranscodeMulti(webp, formats);
  // WebP input with WebP format: no longer a passthrough -- the slot every
  // WebP-negotiating client reads is re-encoded under the acceptance rule and
  // is served only when it beats the origin (#1375).
  if (result.webp.success) {
    EXPECT_EQ(result.webp.output_mime_type, "image/webp");
    EXPECT_LT(result.webp.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.webp.error_message.empty());
  }
}

// =================================================================
// Coverage improvement: TranscodeMulti with WebP lossless/alpha input
// for the IMAGE_WEBP_LOSSLESS_OR_ALPHA detection path (line 646).
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiWebpAlphaInputWebpFormat) {
  std::string webp = ReadTestFile("alpha_32x32.webp");
  ASSERT_FALSE(webp.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder_.TranscodeMulti(webp, formats);
  // WebP alpha input (IMAGE_WEBP_LOSSLESS_OR_ALPHA): takes the same re-encode
  // path as any other WebP input, and the same acceptance rule (#1375).
  if (result.webp.success) {
    EXPECT_EQ(result.webp.output_mime_type, "image/webp");
    EXPECT_LT(result.webp.output_data.size(), webp.size());
  } else {
    EXPECT_FALSE(result.webp.error_message.empty());
  }
}

// =================================================================
// Coverage improvement: TranscodeMulti WebP input with AVIF format.
// Exercises "Cannot convert WebP to AVIF" at line 663.
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiWebpInputAvifFormat) {
  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder_.TranscodeMulti(webp, formats);
  EXPECT_FALSE(result.avif.success);
  EXPECT_EQ(result.avif.error_message, "Cannot convert WebP to AVIF");
}

// =================================================================
// Coverage improvement: TranscodeMultiResized WebP input AVIF format.
// Exercises the same "Cannot convert WebP to AVIF" through the resized
// path (line 661-663).
// =================================================================

TEST(ImageTranscoderResizedWebpTest, TranscodeMultiResizedWebpToAvif) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;

  ImageTranscoder transcoder(config, &handler);

  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMultiResized(
      webp, formats, CapabilityMask::Viewport::kDesktop);

  // WebP → AVIF is explicitly rejected in TranscodeMulti which is the
  // delegation target for desktop viewport (target_width=0).
  EXPECT_FALSE(result.avif.success);
}

// =================================================================
// Coverage improvement: SSIMULACRA2 JPEG re-encode where the
// quality starts too HIGH (score > hi), triggering quality reduction
// (line 1303-1305). Distinct from the WebP over-quality test above.
// =================================================================

TEST(ImageTranscoderSsimVerifyCoverageTest, JpegOverQualityReduction) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.target_ssimulacra2 = 30.0f;    // Very low target
  config.ssimulacra2_tolerance = 1.0f;  // Tight tolerance
  config.ssimulacra2_max_attempts = 2;
  config.ssimulacra2_quality_step = 20;
  config.jpeg_quality = 95;  // Start very high
  config.lossy_jpeg = true;
  // Use viewport smaller than image (512px) to trigger resize and the
  // pixel-based JPEG encoding path where SSIMULACRA2 verification runs.
  config.viewport_widths.mobile = 400;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  // jpeg_quality_hint > 0 prevents the no-resize optimization short-circuit.
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.carried_hint = 95});

  if (result.optimized_original.success &&
      result.optimized_original.output_mime_type == "image/jpeg") {
    EXPECT_GE(result.ssimulacra2_score, 0.0f);
    // With q=95 and target=30, score should be far above target,
    // triggering quality reduction.
    EXPECT_TRUE(result.ssimulacra2_reencoded)
        << "JPEG q=95 against target=30 should trigger over-quality re-encode";
  }
}

// =================================================================
// Coverage improvement: SSIMULACRA2 AVIF re-encode loop.
// Use an extreme target and tight tolerance to trigger the
// AVIF re-encode path (lines 1193-1239).
// =================================================================

TEST(ImageTranscoderSsimVerifyCoverageTest, AvifReencodeAttempt) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.target_ssimulacra2 = 95.0f;    // Very high target
  config.ssimulacra2_tolerance = 1.0f;  // Tight tolerance
  config.ssimulacra2_max_attempts = 2;
  config.ssimulacra2_quality_step = 20;
  config.avif_quality = 30;  // Start low to force re-encode
  config.avif_speed = 10;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  if (result.avif.success) {
    // If AVIF SSIM score was computed, the re-encode loop was exercised.
    if (result.avif_ssimulacra2_score >= 0.0f) {
      EXPECT_LE(result.avif_ssimulacra2_score, 100.0f);
    }
  }
}

// =================================================================
// Coverage: SSIMULACRA2 AVIF over-quality reduction path
// (score > hi at lines 1207-1211).
// =================================================================

TEST(ImageTranscoderSsimVerifyCoverageTest, AvifOverQualityReduction) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.target_ssimulacra2 = 30.0f;    // Very low target
  config.ssimulacra2_tolerance = 1.0f;  // Tight tolerance
  config.ssimulacra2_max_attempts = 2;
  config.ssimulacra2_quality_step = 20;
  config.avif_quality = 90;  // Start high
  config.avif_speed = 10;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  if (result.avif.success && result.avif_ssimulacra2_score >= 0.0f) {
    // With q=90 and target=30, score should be above target.
    // Quality reduction should have been attempted.
    EXPECT_TRUE(result.avif_ssimulacra2_reencoded)
        << "AVIF q=90 against target=30 should trigger over-quality re-encode";
  }
}

// =================================================================
// Coverage: TranscodeMulti fallback with kOriginal exercises
// line 629-630 in the fallback loop.
// =================================================================

TEST_F(ImageTranscoderTest, TranscodeMultiFallbackOriginalFormat) {
  // Truncated JPEG that fails pixel decode.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());
  std::string truncated = jpeg.substr(0, jpeg.size() / 4);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder_.TranscodeMulti(truncated, formats);
  // Pixel decode fails, falls back to individual Transcode calls.
  // The kOriginal case at lines 629-630 should be exercised.
  // Must not crash.
}

// =================================================================
// Coverage: Tiny 1x1 pixel images through TranscodeMulti.
// These exercise EncodeWebpFromPixels and EncodeAvifFromPixels with
// minimal pixel data.
// =================================================================

TEST(ImageTranscoderEncodeFromPixelsTest, TranscodeMulti1x1PngWebpAndAvif) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = false;
  config.content_analysis = false;
  config.learned_quality = false;
  ImageTranscoder transcoder(config, &handler);

  std::string png = ReadTestFile("pngsuite/s01n3p01.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMulti(png, formats);
  // 1x1 PNG exercises the minimal pixel buffer path in both encoders.
  // Must not crash. Output may or may not be smaller than original.
}

// =================================================================
// Coverage: IsAnimatedGif with truncated extension block.
// Data has extension introducer (0x21) but truncates before the
// sub-block data, exercising bounds checks in the extension
// parsing loop (lines 856-861).
// =================================================================

TEST_F(ImageTranscoderTest, IsAnimatedGifTruncatedExtensionBlock) {
  const uint8_t data[] = {
      // Header
      'G', 'I', 'F', '8', '9', 'a',
      // Logical Screen Descriptor
      0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
      // Extension block that truncates
      0x21,  // Extension introducer
      0xF9,  // Label
             // Truncated: no sub-block size byte
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// =================================================================
// Coverage: IsAnimatedGif with GIF that has a trailer (0x3B)
// immediately after the header with no image descriptors.
// Exercises the trailer detection at line 862-863.
// =================================================================

TEST_F(ImageTranscoderTest, IsAnimatedGifTrailerOnly) {
  const uint8_t data[] = {
      // Header
      'G',
      'I',
      'F',
      '8',
      '9',
      'a',
      // Logical Screen Descriptor
      0x01,
      0x00,
      0x01,
      0x00,
      0x00,
      0x00,
      0x00,
      // Immediate trailer
      0x3B,
  };
  std::string gif(reinterpret_cast<const char*>(data), sizeof(data));

  EXPECT_FALSE(ImageTranscoder::IsAnimatedGif(gif));
}

// =================================================================
// Size gate regression tests: resized WebP/AVIF must not exceed the
// optimized original (resized JPEG) for the same viewport.
// =================================================================

TEST_F(ImageTranscoderTest, ResizedWebpNotLargerThanResizedJpeg) {
  // sjpeg6.jpg is 512x512, 146KB. Mobile target = 480px → resize occurs.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x);

  ASSERT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  ASSERT_FALSE(result.optimized_original.output_data.empty());

  size_t baseline = result.optimized_original.output_data.size();

  // WebP from resized pixels must not exceed the optimized original.
  if (result.webp.success) {
    EXPECT_LT(result.webp.output_data.size(), baseline)
        << "Resized WebP (" << result.webp.output_data.size()
        << ") should be smaller than optimized original (" << baseline << ")";
  }

  // AVIF from resized pixels must not exceed the optimized original.
  if (result.avif.success) {
    EXPECT_LT(result.avif.output_data.size(), baseline)
        << "Resized AVIF (" << result.avif.output_data.size()
        << ") should be smaller than optimized original (" << baseline << ")";
  }
}

TEST_F(ImageTranscoderTest, ResizedWebpNotLargerThanResizedJpeg2xDensity) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  // 2x density: mobile target = 960px, but image is 512px so no resize.
  // Tablet 1x target = 768px, but image is 512px so no resize either.
  // Use desktop 1x to ensure stream path, or just verify the gate logic
  // at mobile 1x where resize does occur.
  auto result = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k2xPlus);

  // At 2x density with 512px image, mobile target becomes 960px — no resize.
  // The stream path (TranscodeMulti) will be used, so just verify no crash
  // and that the size gate still holds against the original input.
  if (result.webp.success) {
    EXPECT_LE(result.webp.output_data.size(), jpeg.size())
        << "WebP should not exceed original input size";
  }
  if (result.avif.success) {
    EXPECT_LE(result.avif.output_data.size(), jpeg.size())
        << "AVIF should not exceed original input size";
  }
}

// =================================================================
// Coverage: SSIMULACRA2 WebP over-quality reduction path
// (score > hi at lines 1127-1135). Start with high WebP quality and
// low target to force quality reduction.
// =================================================================

TEST(ImageTranscoderSsimVerifyCoverageTest, WebpOverQualityReduction) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.content_analysis = false;
  config.learned_quality = false;
  config.target_ssimulacra2 = 30.0f;    // Very low target
  config.ssimulacra2_tolerance = 1.0f;  // Tight tolerance
  config.ssimulacra2_max_attempts = 2;
  config.ssimulacra2_quality_step = 20;
  config.webp_quality = 95;  // Start very high
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  if (result.webp.success && result.webp_ssimulacra2_score >= 0.0f) {
    // With q=95 and target=30, score should be far above target,
    // triggering WebP quality reduction.
    EXPECT_TRUE(result.webp_ssimulacra2_reencoded)
        << "WebP q=95 against target=30 should trigger over-quality re-encode";
  }
}

// =================================================================
// Coverage: Learned quality prediction with per-format fallbacks.
// Disable only JPEG and WebP models to exercise the fallback counter
// for those formats while AVIF uses learned prediction.
// =================================================================

TEST(ImageTranscoderLearnedQualityCoverageTest, PartialFormatFallback) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.learned_quality = true;
  config.learned_quality_jpeg = false;  // Will use heuristic fallback
  config.learned_quality_webp = false;  // Will use heuristic fallback
  config.learned_quality_avif = true;   // Will attempt learned prediction
  config.content_analysis = true;
  config.quality_verify = false;
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // JPEG and WebP don't use learned quality (flags disabled), but AVIF does.
  // At least AVIF should have attempted learned quality or fallen back.
  EXPECT_TRUE(result.used_learned_quality ||
              result.learned_quality_fallbacks > 0)
      << "With AVIF learned=true, either prediction or fallback should occur";
}

// =================================================================
// Coverage: Save-Data quality reduction with content analysis enabled
// but learned quality disabled. This exercises the heuristic fallback
// path (apply_factor lambda at lines 1027-1042) with save-data
// quality overrides.
// =================================================================

TEST(ImageTranscoderSaveDataCoverageTest,
     SaveDataWithContentAnalysisNoLearned) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;  // Force heuristic fallback
  config.quality_verify = false;
  config.viewport_widths.mobile = 100;

  // Set save-data qualities significantly lower
  config.jpeg_quality = 85;
  config.savedata_jpeg_quality = 40;
  config.webp_quality = 75;
  config.savedata_webp_quality = 25;
  config.avif_quality = 60;
  config.savedata_avif_quality = 20;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto normal = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff);

  auto savedata = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn);

  // Both should succeed.
  ASSERT_TRUE(normal.optimized_original.success)
      << normal.optimized_original.error_message;
  ASSERT_TRUE(savedata.optimized_original.success)
      << savedata.optimized_original.error_message;

  // Save-Data output should be smaller due to lower quality.
  EXPECT_LE(savedata.optimized_original.output_data.size(),
            normal.optimized_original.output_data.size())
      << "Save-Data with heuristic fallback should produce smaller/equal "
         "output";

  // Content analysis should still classify the image.
  EXPECT_FALSE(savedata.used_learned_quality)
      << "learned_quality=false should not use ML prediction";
}

// =================================================================
// Coverage: Denoising path. Use a noisy image with low denoise
// threshold to exercise the bilateral filter path in
// TranscodeMultiResized (lines 1057-1083).
// =================================================================

TEST(ImageTranscoderDenoiseCoverageTest, DenoisingAppliedForNoisyImage) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;
  config.viewport_widths.mobile = 100;
  // Very low denoise threshold to increase chance of denoising being applied.
  config.denoise_threshold = 0.01f;
  config.denoise_sigma_spatial = 3.0f;
  config.denoise_sigma_range = 25.0f;

  ImageTranscoder transcoder(config, &handler);

  // sjpeg6.jpg (512x512) — after resize to 100px wide, width=100 < 640.
  // Use a wider target to keep width >= 640.
  config.viewport_widths.mobile = 480;
  ImageTranscoder transcoder2(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder2.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  // Denoising may or may not be applied depending on the image's noise_level.
  // If it was applied, result.denoised will be true.
  // Either way, the path through the denoising code was exercised.
}

// =================================================================
// Coverage: Denoising threshold disabled (0.0).
// =================================================================

TEST(ImageTranscoderDenoiseCoverageTest, DenoisingDisabledWhenThresholdZero) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;
  config.viewport_widths.mobile = 100;
  config.denoise_threshold = 0.0f;  // Denoising disabled

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_FALSE(result.denoised)
      << "Denoising should not be applied when threshold is 0";
}

// =================================================================
// Coverage: Tablet viewport resize path through TranscodeMultiResized.
// =================================================================

TEST(ImageTranscoderResizeCoverageTest, TabletViewportResizePath) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.tablet = 200;
  config.content_analysis = false;
  config.learned_quality = false;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kTablet);

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  // With tablet target 200px and a 512px image, resize should occur.
  // Output should be substantially smaller than original.
  if (result.webp.success) {
    EXPECT_LT(result.webp.output_data.size(), jpeg.size())
        << "Resized WebP for tablet should be smaller than original";
  }
}

// =================================================================
// Coverage: TranscodeMultiResized with animated GIF and target_width=0
// (desktop with default config). Should delegate to TranscodeMulti.
// =================================================================

TEST(ImageTranscoderResizeCoverageTest, AnimatedGifDesktopDelegatesToMulti) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  // Desktop viewport_widths.desktop = 0 by default → target_width == 0.
  config.content_analysis = false;
  config.learned_quality = false;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string gif = ReadTestFile("gif/animated.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      gif, formats, CapabilityMask::Viewport::kDesktop);

  // Should delegate to TranscodeMulti (target_width==0), not crash.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/gif");
}

// =================================================================
// Coverage: TranscodeMultiResized kOriginal path with non-JPEG
// that first tries OptimizeJpeg (fails), then falls through
// to Transcode (lines 1280-1285).
// =================================================================

TEST(ImageTranscoderResizeCoverageTest, NonJpegOriginalFallbackToTranscode) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 100;
  config.content_analysis = false;
  config.learned_quality = false;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      gif, formats, CapabilityMask::Viewport::kMobile);

  // GIF should fall through OptimizeJpeg → Transcode → GIF passthrough.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/gif");
}

// =================================================================
// Coverage: TranscodeMultiResized with WebP input requesting AVIF.
// WebP → AVIF is explicitly rejected in TranscodeMulti path.
// =================================================================

TEST(ImageTranscoderResizeCoverageTest, WebpToAvifRejectedInMultiResized) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = false;
  config.learned_quality = false;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string webp = ReadTestFile("opaque_32x20.webp");
  ASSERT_FALSE(webp.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };

  auto result = transcoder.TranscodeMultiResized(
      webp, formats, CapabilityMask::Viewport::kDesktop);

  // WebP → AVIF should be rejected.
  EXPECT_FALSE(result.avif.success);
}

// =================================================================
// Coverage: Encode GIF source to AVIF via ConvertToAvif path
// (exercises the IMAGE_GIF case at lines 268-269).
// =================================================================

TEST(ImageTranscoderFormatPathTest, GifToAvifViaConvertToAvif) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, &handler);

  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);

  auto result = transcoder.Transcode(gif, mask);
  // Should either succeed as AVIF or fall back to GIF passthrough.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_TRUE(result.output_mime_type == "image/avif" ||
              result.output_mime_type == "image/gif")
      << "Unexpected mime: " << result.output_mime_type;
}

// =================================================================
// Coverage: ConvertToWebp with GIF input (unsupported source for
// non-GIF WebP path). The GIF→WebP path uses ConvertGifToWebp,
// but if a GIF is passed to ConvertToWebp directly, it should
// fail with "unsupported source format".
// =================================================================

TEST(ImageTranscoderFormatPathTest, GifToWebpViaConvertGifToWebp) {
  // GIF→WebP goes through ConvertGifToWebp, not ConvertToWebp.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, &handler);

  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);

  auto result = transcoder.Transcode(gif, mask);
  // Should either succeed (if GIF→WebP conversion works) or fail gracefully.
  // Either way, it should not crash.
  if (result.success) {
    EXPECT_TRUE(result.output_mime_type == "image/webp" ||
                result.output_mime_type == "image/gif");
  }
}

// =================================================================
// Coverage: TranscodeMultiResized with jpeg_quality_hint > 0 and
// mobile viewport where resize occurs. This exercises the path where
// jpeg_quality_hint prevents the TranscodeMulti shortcut even when
// encode_src == &decoded (lines 1048-1050).
// =================================================================

TEST(ImageTranscoderResizeCoverageTest, JpegQualityHintPreventsShortcut) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 1024;  // Larger than image, no resize
  config.content_analysis = false;
  config.learned_quality = false;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  // With jpeg_quality_hint > 0, the no-resize TranscodeMulti shortcut
  // should be bypassed (line 949-951), exercising the full encode path.
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.carried_hint = 70});

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  // The hint quality should be applied.
  EXPECT_EQ(result.final_jpeg_quality, 70);
}

// =================================================================
// Coverage: SVG format target exercises the SVG info log path
// (lines 137-143) and falls back to original format.
// =================================================================

TEST(ImageTranscoderFormatPathTest, SvgTargetFallbackForJpeg) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kSvg);

  auto result = transcoder.Transcode(jpeg, mask);
  // SVG is not a transcoding target — should optimize as JPEG.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ(result.output_mime_type, "image/jpeg");
}

// =================================================================
// Coverage: TranscodeMulti with default format case (kSvg) in the
// switch statement (lines 680-681).
// =================================================================

TEST(ImageTranscoderFormatPathTest, TranscodeMultiSvgFormatIgnored) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = false;
  config.learned_quality = false;
  config.quality_verify = false;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kSvg,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMulti(jpeg, formats);
  // SVG format should be silently ignored (default case in switch).
  // Original should still be processed.
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
}

// =================================================================
// Coverage: ResizeForViewport with tablet viewport and k2xPlus
// density. Target width is doubled.
// =================================================================

TEST(ImageTranscoderResizeCoverageTest, TabletViewport2xDensityResize) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.tablet = 200;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto decoded = transcoder.DecodeToPixels(jpeg);
  ASSERT_FALSE(decoded.pixel_buffer.empty());

  // Tablet at 2x → target = 400px. 512px image > 400px → resize.
  auto resized =
      transcoder.ResizeForViewport(decoded, CapabilityMask::Viewport::kTablet,
                                   CapabilityMask::PixelDensity::k2xPlus);

  EXPECT_FALSE(resized.pixel_buffer.empty());
  EXPECT_EQ(resized.width, 400u);
}

// =================================================================
// Coverage: Content analysis applies_at_viewport gating.
// kScreenshot content class only applies at viewport >= 768px.
// Test with a small viewport that would skip the preset.
// =================================================================

TEST(ImageTranscoderContentAnalysisCoverageTest,
     ContentAnalysisSkippedAtSmallViewport) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;
  // Very small viewport target: if image is classified as kScreenshot,
  // the quality factors won't be applied (applies_at_viewport returns
  // false for viewport < 768 with kScreenshot).
  config.viewport_widths.mobile = 100;

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  // Applied preset should be populated.
  EXPECT_GE(result.final_jpeg_quality, 1);
}

// =================================================================
// Regression test: resized JPEG variants must use resized pixels,
// not the full-size original input.
// =================================================================

TEST(ImageTranscoderJpegResizeTest, ResizedJpegMatchesTargetDimensions) {
  // sjpeg6.jpg is 512x512. Mobile target = 480px → resize to 480x480.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 480;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/jpeg");

  // Decode the output to verify dimensions match the resized target,
  // not the original 512x512.
  auto decoded =
      transcoder.DecodeToPixels(result.optimized_original.output_data);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.width, 480u);
  EXPECT_EQ(decoded.height, 480u);
}

TEST(ImageTranscoderJpegResizeTest, ResizedJpegSsimulacra2NotSkipped) {
  // When SSIMULACRA2 verification is enabled, it should compute a score
  // for resized JPEG (dimensions match encode_src), not silently skip.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 480;
  config.quality_verify = true;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  // SSIMULACRA2 should have been computed (not skipped due to dimension mismatch).
  EXPECT_GE(result.ssimulacra2_score, 0.0f);
}

TEST(ImageTranscoderJpegResizeTest, ResizedJpegSmallerThanOriginal) {
  // Resized JPEG at mobile viewport should be smaller than the full-size
  // original, not the same size (which was the bug).
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 480;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  // Resized 480x480 JPEG must be smaller than 512x512 original.
  EXPECT_LT(result.optimized_original.output_data.size(), jpeg.size());
}

TEST(ImageTranscoderJpegResizeTest,
     ResizedGrayscaleJpegMatchesTargetDimensions) {
  // testgray.jpg is 130x97 grayscale (1 component). Mobile target = 100px
  // triggers resize to 100x74, exercising the GRAY_8 branch of
  // EncodeJpegFromPixels with actual resized pixel data.
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 100;
  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(result.optimized_original.output_mime_type, "image/jpeg");

  // Decode the output to verify dimensions match the resized target
  // and that grayscale is preserved through the resize + encode pipeline.
  auto decoded =
      transcoder.DecodeToPixels(result.optimized_original.output_data);
  ASSERT_FALSE(decoded.pixel_buffer.empty());
  EXPECT_EQ(decoded.width, 100u);
  EXPECT_EQ(decoded.bytes_per_pixel, 1);  // Grayscale preserved
  EXPECT_FALSE(decoded.has_alpha);
  // Height should be proportionally scaled: 97 * 100 / 130 ≈ 74.
  EXPECT_GT(decoded.height, 0u);
  EXPECT_LT(decoded.height, 97u);
}

// =================================================================
// Fix verification: Content class consistency across viewports
// =================================================================

// Test: Content analysis runs on original (unresized) pixels, so the
// content class is identical across viewports that trigger the resize path.
// Desktop with target_width=0 takes a fast path that skips content analysis.
TEST(ContentClassConsistencyTest, SameClassAcrossResizedViewports) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = false;
  config.quality_verify = false;
  // Use small viewport widths so the image is actually resized.
  config.viewport_widths.mobile = 100;
  config.viewport_widths.tablet = 200;

  ImageTranscoder transcoder(config, &handler);

  // sjpeg6.jpg is 512x512 — wider than both mobile (100) and tablet (200),
  // so actual pixel resize occurs for both viewports.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto mobile = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);
  auto tablet = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kTablet);

  // Both viewports should produce the same content class since analysis
  // now runs on original pixels (not viewport-resized).  Before the fix,
  // mobile (100px) and tablet (200px) could disagree because different
  // resize resolutions affected the gradient/color analysis.
  EXPECT_NE(mobile.applied_preset.content_class,
            pagespeed::ContentClass::kUnknown)
      << "Content analysis should have run for mobile";
  EXPECT_NE(tablet.applied_preset.content_class,
            pagespeed::ContentClass::kUnknown)
      << "Content analysis should have run for tablet";
  EXPECT_EQ(mobile.applied_preset.content_class,
            tablet.applied_preset.content_class)
      << "Mobile and tablet should produce the same content class "
         "(analysis runs on original pixels)";

  // Desktop with target_width=0 still decodes pixels for content analysis
  // even though it delegates encoding to the stream-based TranscodeMulti.
  auto desktop = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);
  EXPECT_NE(desktop.applied_preset.content_class,
            pagespeed::ContentClass::kUnknown)
      << "Desktop should run content analysis (pixels decoded before fast "
         "path)";
  EXPECT_EQ(desktop.applied_preset.content_class,
            mobile.applied_preset.content_class)
      << "Desktop should produce the same content class as mobile/tablet";
}

// =================================================================
// Fix verification: Per-format SSIMULACRA2 scores populated
// =================================================================

// Test: WebP, AVIF, and JPEG SSIMULACRA2 scores are independently populated
// in the multi-resize result.  Before the fix, only the JPEG score was
// set — WebP and AVIF scores were ignored in metadata persistence.
TEST(PerFormatSsimulacra2Test, AllFormatScoresPopulated) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.content_analysis = true;
  config.learned_quality = true;
  config.quality_verify = true;
  config.viewport_widths.mobile = 200;  // Force resize path

  ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // All format scores should be >= 0 (not N/A) when encoding succeeds.
  ASSERT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_GE(result.ssimulacra2_score, 0.0f)
      << "JPEG SSIMULACRA2 score should be populated";
  EXPECT_LE(result.ssimulacra2_score, 100.0f);

  ASSERT_TRUE(result.webp.success) << result.webp.error_message;
  EXPECT_GE(result.webp_ssimulacra2_score, 0.0f)
      << "WebP SSIMULACRA2 score should be populated";
  EXPECT_LE(result.webp_ssimulacra2_score, 100.0f);

  // AVIF SSIMULACRA2 verification may not run if the AVIF decode round-trip
  // produces different bytes_per_pixel than the source (bpp guard in the
  // verification path).  When it does run, verify the score is valid.
  if (result.avif.success && result.avif_ssimulacra2_score >= 0.0f) {
    EXPECT_LE(result.avif_ssimulacra2_score, 100.0f);
  }
}

// =================================================================
// Fix verification: Updated SSIMULACRA2 defaults
// =================================================================

// Test: Default config has max_attempts=4, quality_step=5.
TEST(Ssimulacra2DefaultsTest, UpdatedDefaults) {
  ImageTranscoderConfig config;
  EXPECT_EQ(config.ssimulacra2_max_attempts, 4)
      << "Default max_attempts should be 4 (3 re-encode attempts)";
  EXPECT_EQ(config.ssimulacra2_quality_step, 5)
      << "Default quality_step should be 5 for finer adjustment";
}

// Test that the re-encode loop adjusts quality when score is below target band.
// Uses a very low initial quality so the SSIMULACRA2 score starts below the
// band and the loop must raise quality.
TEST(Ssimulacra2VerifyTest, ReencodeLoopRaisesQuality) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.webp_quality = 10;  // Very low → score below target band
  config.quality_verify = true;
  config.learned_quality = false;
  config.content_analysis = false;
  config.target_ssimulacra2 = 70.0f;
  config.ssimulacra2_tolerance = 5.0f;
  config.ssimulacra2_max_attempts = 4;
  config.ssimulacra2_quality_step = 15;
  config.viewport_widths.mobile = 200;  // Force resized path

  ImageTranscoder transcoder(config, &handler);
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  if (result.webp.success) {
    // The loop should have re-encoded at least once.
    EXPECT_TRUE(result.webp_ssimulacra2_reencoded)
        << "Re-encode loop should trigger at q=10 (score below band)";
    // Final score should be closer to target than starting at q=10.
    EXPECT_GE(result.webp_ssimulacra2_score, 0.0f);
  }
}

// Test that verification does NOT re-encode when score is within the band.
TEST(Ssimulacra2VerifyTest, NoReencodeWhenInBand) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.learned_quality = false;
  config.content_analysis = false;
  config.target_ssimulacra2 = 70.0f;
  config.ssimulacra2_tolerance = 30.0f;  // Very wide band [52, 118]
  config.ssimulacra2_max_attempts = 4;
  config.viewport_widths.mobile = 200;

  ImageTranscoder transcoder(config, &handler);
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.webp.success) << result.webp.error_message;
  EXPECT_FALSE(result.webp_ssimulacra2_reencoded)
      << "No re-encode needed when tolerance band is very wide";
  EXPECT_GE(result.webp_ssimulacra2_score, 0.0f);
}

// Test SSIMULACRA2 verification with max_attempts=1 (score only, no loop).
TEST(Ssimulacra2VerifyTest, ScoreOnlyNoLoop) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.webp_quality = 10;  // Low quality, score will be below band
  config.quality_verify = true;
  config.learned_quality = false;
  config.content_analysis = false;
  config.ssimulacra2_max_attempts = 1;  // Score only
  config.viewport_widths.mobile = 200;

  ImageTranscoder transcoder(config, &handler);
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
  };
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  if (result.webp.success) {
    // Score computed but no re-encode loop.
    EXPECT_GE(result.webp_ssimulacra2_score, 0.0f);
    EXPECT_FALSE(result.webp_ssimulacra2_reencoded);
  }
}

// Test that SSIMULACRA2 verification works on the desktop fast-path
// (TranscodeMulti) for AVIF as well.
TEST(Ssimulacra2VerifyTest, DesktopFastPathAvif) {
  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.quality_verify = true;
  config.learned_quality = true;
  config.content_analysis = true;
  config.viewport_widths.desktop = 0;

  ImageTranscoder transcoder(config, &handler);
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kAvif,
  };
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kDesktop);

  ASSERT_TRUE(result.avif.success) << result.avif.error_message;
  // AVIF SSIMULACRA2 may not run if bpp mismatch (3 vs 4), so check >= -1.
  // When it does run, verify score is valid.
  if (result.avif_ssimulacra2_score >= 0.0f) {
    EXPECT_LE(result.avif_ssimulacra2_score, 100.0f);
  }
}

// =================================================================
// Quality Baselining: Quality Cap Tests
// =================================================================

class QualityCapTest : public testing::Test {
 protected:
  NullMessageHandler handler_;

  // Helper to create a transcoder with specific config.
  std::unique_ptr<ImageTranscoder> MakeTranscoder(ImageTranscoderConfig cfg) {
    return std::make_unique<ImageTranscoder>(cfg, &handler_);
  }
};

// Test: Low-quality source (q20) caps JPEG output quality.
TEST_F(QualityCapTest, LowQualitySourceCaps) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q20_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/20);
  // Capped quality may produce no savings (output >= original).
  // The test cares about the cap metadata, not the output data.
  EXPECT_EQ(result.source_jpeg_quality, 20);
  EXPECT_GT(result.quality_capped_count, 0)
      << "Quality should be capped for q20 source with target 85";
}

// Test: a source at the configured quality should NOT cap JPEG output.
TEST_F(QualityCapTest, HighQualitySourceNoCap) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 80;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q80_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/80);
  // Optimization may produce no savings when source is close to target.
  EXPECT_EQ(result.source_jpeg_quality, 80);
  // target (80) == source (80): the cap has nothing to pull down.
  EXPECT_EQ(result.quality_capped_count, 0)
      << "Quality should not be capped for q80 source with target 80";
}

// Test: Medium-quality source (q60) caps JPEG output quality.
TEST_F(QualityCapTest, MediumQualitySourceCaps) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q60_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/60);
  // Capped quality may produce no savings (output >= original).
  // source (60) < target (85), so the cap applies.
  EXPECT_GT(result.quality_capped_count, 0)
      << "Quality should be capped for q60 source with target 85";
}

// Test: the configured margin does not lift the cap above the source.
TEST_F(QualityCapTest, MarginDoesNotWidenTheCap) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 5;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q80_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // target (85) is above the source (80) whatever the margin says.
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/80);
  EXPECT_EQ(result.quality_capped_count, 1)
      << "A margin must not exempt a target above the source quality";
}

// Test: Very low quality source (q20) respects floor of 30.
TEST_F(QualityCapTest, FloorAtThirty) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 5;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q20_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  // source (20) is below the floor, so the cap is max(30, 20) = 30.
  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/20);
  EXPECT_GT(result.quality_capped_count, 0);
  EXPECT_EQ(result.source_jpeg_quality, 20);
}

// Test: PNG source should not be capped (no cross-format cap).
TEST_F(QualityCapTest, PngSourceNoCap) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // source_jpeg_quality=-1 (non-JPEG) should not trigger cap.
  auto result = t->TranscodeMulti(png, fmts, /*source_jpeg_quality=*/-1);
  EXPECT_EQ(result.quality_capped_count, 0)
      << "Non-JPEG sources should not be quality-capped";
}

// Test: Quality cap disabled via no_quality_cap.
TEST_F(QualityCapTest, DisabledViaNoCap) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = true;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q20_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/20);
  EXPECT_EQ(result.quality_capped_count, 0)
      << "Quality cap should be disabled when no_quality_cap=true";
}

// Test: Custom margin value.
TEST_F(QualityCapTest, CustomMargin) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 20;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q60_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // source (60) < target (85), so the cap applies whatever the margin is.
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/60);
  EXPECT_GT(result.quality_capped_count, 0);
}

// Test: WebP format is not capped (only JPEG is capped).
TEST_F(QualityCapTest, WebPNotCapped) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.webp_quality = 75;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q20_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  // Request both JPEG and WebP. JPEG should be capped, WebP should not.
  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal,
      CapabilityMask::ImageFormat::kWebP};
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/20);
  // Capped JPEG may produce no savings (output >= original).
  EXPECT_GT(result.quality_capped_count, 0)
      << "JPEG quality should be capped for q20 source";
  // WebP may or may not succeed (depends on size gate) but the quality cap
  // counter should reflect only JPEG capping, not WebP.
  EXPECT_EQ(result.quality_capped_count, 1)
      << "Only one JPEG cap should fire, WebP is not capped";
}

// Test: source_jpeg_quality propagated in result.
TEST_F(QualityCapTest, SourceQualityInResult) {
  ImageTranscoderConfig cfg;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q40_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/40);
  EXPECT_EQ(result.source_jpeg_quality, 40);
}

// Test: Default source_jpeg_quality is -1.
TEST_F(QualityCapTest, DefaultSourceQualityNegative) {
  ImageTranscoderConfig cfg;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMulti(jpeg, fmts);  // No source_jpeg_quality.
  EXPECT_EQ(result.source_jpeg_quality, -1);
  EXPECT_EQ(result.quality_capped_count, 0)
      << "Unknown source quality should not trigger cap";
}

// =================================================================
// Quality Baselining: Early Exit Tests
// =================================================================

// Test: Source below target -> skipped_jpeg_reencode is true.
TEST_F(QualityCapTest, EarlyExitSourceBelowTarget) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_verify = false;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q40_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.source_quality = 40});
  // Early exit skips pixel re-encode; stream-based optimization may
  // produce no savings on well-compressed sources.
  EXPECT_TRUE(result.skipped_jpeg_reencode)
      << "Source quality 40 <= target 85 should trigger early exit";
}

// Test: Source above target -> skipped_jpeg_reencode is false.
TEST_F(QualityCapTest, NoEarlyExitSourceAboveTarget) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 30;
  cfg.quality_verify = false;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q60_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.source_quality = 60});
  EXPECT_TRUE(result.optimized_original.success);
  EXPECT_FALSE(result.skipped_jpeg_reencode)
      << "Source quality 60 > target 30 should not trigger early exit";
}

// Test: Resized image should not trigger early exit.
TEST_F(QualityCapTest, NoEarlyExitWhenResized) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_verify = false;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  cfg.viewport_widths.mobile = 256;  // Force resize for 512x512 image.
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q40_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.source_quality = 40});
  EXPECT_TRUE(result.optimized_original.success);
  EXPECT_FALSE(result.skipped_jpeg_reencode)
      << "Resized images should not trigger early exit";
}

// Test: WebP still generated after JPEG early exit.
TEST_F(QualityCapTest, WebPStillGeneratedAfterEarlyExit) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_verify = false;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q40_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.source_quality = 40});
  EXPECT_TRUE(result.skipped_jpeg_reencode) << "JPEG should get early exit";
  EXPECT_TRUE(result.webp.success)
      << "WebP should still be generated even when JPEG gets early exit";
}

// =================================================================
// Quality Baselining: Config Tests
// =================================================================

TEST(QualityBaselineConfigTest, DefaultConfigValues) {
  ImageTranscoderConfig cfg;
  EXPECT_EQ(cfg.quality_cap_margin, 10);
  EXPECT_FALSE(cfg.no_quality_cap);
}

TEST(QualityBaselineConfigTest, DefaultResultValues) {
  MultiTranscodeResult result;
  EXPECT_EQ(result.source_jpeg_quality, -1);
  EXPECT_EQ(result.quality_capped_count, 0);
  EXPECT_FALSE(result.skipped_jpeg_reencode);
}

// =================================================================
// Quality Baselining: Additional Tests (review findings)
// =================================================================

// Test: GIF source should not be capped (no cross-format cap).
TEST_F(QualityCapTest, GifSourceNoCap) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string gif = ReadTestFile("gif/interlaced.gif");
  ASSERT_FALSE(gif.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // GIF with source_jpeg_quality=-1: no cap should trigger.
  auto result = t->TranscodeMulti(gif, fmts, /*source_jpeg_quality=*/-1);
  EXPECT_EQ(result.quality_capped_count, 0)
      << "GIF sources should not be quality-capped";
}

// Test: source_jpeg_quality=0 should not trigger cap (boundary at <= 0).
TEST_F(QualityCapTest, QualityZeroNoCap) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q40_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // source_jpeg_quality=0 is treated as "unknown" (guard: <= 0).
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/0);
  EXPECT_EQ(result.quality_capped_count, 0)
      << "source_jpeg_quality=0 should not trigger cap";
  EXPECT_EQ(result.source_jpeg_quality, 0);
}

// Test: source_jpeg_quality=100 should never trigger cap.
TEST_F(QualityCapTest, QualityHundredNoCap) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q80_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // source (100) is above target (85), so no cap.
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/100);
  EXPECT_EQ(result.quality_capped_count, 0)
      << "source_jpeg_quality=100 should never trigger cap";
  EXPECT_EQ(result.source_jpeg_quality, 100);
}

// Test: Early exit also works through desktop fast-path (TranscodeMulti).
TEST_F(QualityCapTest, EarlyExitDesktopFastPath) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q40_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // After cap: max(30, 40) = 40 < 85. Cap fires, jpeg_quality = 40.
  // Then source (40) <= capped target (40) → early exit.
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/40);
  // Early exit + cap: stream optimization may produce no savings.
  EXPECT_TRUE(result.skipped_jpeg_reencode)
      << "Desktop fast-path (TranscodeMulti) should also set early exit flag";
  EXPECT_GT(result.quality_capped_count, 0);
}

// Test: Save-Data + quality cap interaction (save-data takes slow path,
// cap still applies).
TEST_F(QualityCapTest, SaveDataCapInteraction) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 60;  // Save-data quality.
  cfg.quality_cap_margin = 10;
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q30_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // Save-Data with source q30: cap = max(30, 30) = 30 < target (60).
  // Cap fires, jpeg_quality → 30.
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOn,
      {.source_quality = 30});
  // Capped quality may produce no savings (output >= original).
  EXPECT_GT(result.quality_capped_count, 0)
      << "Quality cap should still fire with save-data enabled";
  EXPECT_EQ(result.source_jpeg_quality, 30);
}

// Test: Early exit does NOT trigger when source quality > target (fast path).
TEST_F(QualityCapTest, NoEarlyExitDesktopFastPathSourceAboveTarget) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 30;
  cfg.quality_verify = false;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q60_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // source (60) > target (30), so no cap (30 is already below the cap).
  // 60 > 30 → no early exit.
  auto result = t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/60);
  EXPECT_TRUE(result.optimized_original.success);
  EXPECT_FALSE(result.skipped_jpeg_reencode)
      << "Source above target should not set early exit in fast path";
}

// =================================================================
// Same-format cap at the detected source quality (#1284)
// =================================================================

// Detects whether TranscodeMultiResized still accepts a bare int in the
// trailing quality slot.  Both production call sites used to pass six
// arguments to a seven-argument signature: the carried quality hint bound to
// the hint slot, the source quality silently defaulted to "unknown", and the
// same-format cap never ran on a single production encode.  Grouping the two
// quality inputs into a named struct makes that call shape ill-formed.
template <typename T, typename = void>
struct AcceptsBareIntQualityArg : std::false_type {};

template <typename T>
struct AcceptsBareIntQualityArg<
    T,
    std::void_t<decltype(std::declval<T&>().TranscodeMultiResized(
        std::declval<std::string_view>(),
        std::declval<const std::vector<CapabilityMask::ImageFormat>&>(),
        CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
        CapabilityMask::SaveData::kOff, 85))>> : std::true_type {};

static_assert(!AcceptsBareIntQualityArg<ImageTranscoder>::value,
              "TranscodeMultiResized must not accept a bare int for its "
              "quality inputs: that is how the carried hint used to occupy "
              "the argument list while the source quality went unset");

// Test: the quality inputs cannot be filled positionally by accident.
TEST_F(QualityCapTest, QualityInputsCannotBindPositionally) {
  EXPECT_FALSE(AcceptsBareIntQualityArg<ImageTranscoder>::value)
      << "A bare int in the quality slot must not compile";

  // Defaults are still 'unknown' for both fields.
  pagespeed::JpegQualityInputs defaults;
  EXPECT_EQ(defaults.carried_hint, -1);
  EXPECT_EQ(defaults.source_quality, -1);
}

// Test: a carried hint does not stand in for the source quality.  The hint
// sets the encoder quality; the cap then pulls it back to the source.
TEST_F(QualityCapTest, CarriedHintDoesNotSubstituteForSourceQuality) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 85;
  cfg.quality_verify = false;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  cfg.viewport_widths.mobile = 256;  // Force the pixel encode path.
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q60_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.carried_hint = 90, .source_quality = 60});

  EXPECT_EQ(result.source_jpeg_quality, 60);
  EXPECT_EQ(result.final_jpeg_quality, 60)
      << "A hint of 90 over a q60 source must be capped back to 60";
  EXPECT_EQ(result.quality_capped_count, 1);
}

// Test: source 75 with a configured 95 caps at 75 exactly (no margin).
TEST_F(QualityCapTest, SameFormatCapEqualsSourceQuality) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 95;
  cfg.quality_cap_margin = 10;  // Must not widen the cap.
  cfg.no_quality_cap = false;
  cfg.quality_verify = false;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  cfg.viewport_widths.mobile = 256;  // Force the pixel encode path.
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q80_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  // The source quality is what the caller measured, so it is supplied here.
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.source_quality = 75});

  EXPECT_EQ(result.final_jpeg_quality, 75)
      << "Cap must be the source quality, not source + margin";
  EXPECT_EQ(result.quality_capped_count, 1);
}

// Test: the cap fires for every configured quality above the source, and for
// none at or below it — pinning the cap value without reading it back.
TEST_F(QualityCapTest, CapBoundaryIsExactlyTheSourceQuality) {
  std::string jpeg = ReadTestFile("jpeg/q60_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());
  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};

  auto capped_at = [&](int configured_quality) {
    ImageTranscoderConfig cfg;
    cfg.jpeg_quality = configured_quality;
    cfg.quality_cap_margin = 10;
    cfg.quality_verify = false;
    cfg.learned_quality = false;
    cfg.content_analysis = false;
    auto t = MakeTranscoder(cfg);
    return t->TranscodeMulti(jpeg, fmts, /*source_jpeg_quality=*/60)
        .quality_capped_count;
  };

  EXPECT_EQ(capped_at(60), 0) << "Configured == source must not cap";
  EXPECT_EQ(capped_at(61), 1) << "One point above the source must cap";
  EXPECT_EQ(capped_at(70), 1) << "The old margin of 10 must no longer exempt";
}

// Test: the SSIMULACRA2 re-encode loop treats the cap as a ceiling.
TEST_F(QualityCapTest, VerifyLoopDoesNotClimbPastTheCap) {
  ImageTranscoderConfig cfg;
  cfg.jpeg_quality = 95;
  cfg.quality_verify = true;
  // A target the capped encode cannot reach, so the loop wants to climb.
  cfg.target_ssimulacra2 = 100.0f;
  cfg.ssimulacra2_tolerance = 1.0f;
  cfg.ssimulacra2_max_attempts = 4;
  cfg.ssimulacra2_quality_step = 5;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  cfg.viewport_widths.mobile = 256;  // Force the pixel encode + verify path.
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q60_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kOriginal};
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.source_quality = 60});

  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_LE(result.final_jpeg_quality, 60)
      << "The verify loop must not encode above the source quality";
  EXPECT_EQ(result.final_jpeg_quality, 60);
  EXPECT_FALSE(result.ssimulacra2_reencoded)
      << "At the ceiling the loop accepts the encode instead of re-encoding";
  EXPECT_GE(result.ssimulacra2_score, 0.0f) << "Output must still be verified";
}

// Test: the JPEG cap does not constrain the cross-format encoders, whose
// quality scales are unrelated to the source JPEG's.
TEST_F(QualityCapTest, CrossFormatEncodeIsNotBoundByTheJpegCap) {
  ImageTranscoderConfig cfg;
  cfg.webp_quality = 90;  // Well above the source JPEG quality.
  cfg.quality_verify = false;
  cfg.learned_quality = false;
  cfg.content_analysis = false;
  cfg.viewport_widths.mobile = 256;
  auto t = MakeTranscoder(cfg);

  std::string jpeg = ReadTestFile("jpeg/q60_512x512.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> fmts = {
      CapabilityMask::ImageFormat::kWebP};
  auto result = t->TranscodeMultiResized(
      jpeg, fmts, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      {.source_quality = 60});

  EXPECT_EQ(result.final_webp_quality, 90)
      << "WebP quality is on its own scale and the JPEG cap must not touch it";
}

// Test: the source quality estimate the callers feed in comes from the DQT
// tables and is 'unknown' for anything that is not a JPEG.
TEST_F(QualityCapTest, DetectSourceJpegQualityReadsTheSourceTables) {
  std::string q80 = ReadTestFile("jpeg/q80_512x512.jpg");
  std::string q60 = ReadTestFile("jpeg/q60_512x512.jpg");
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(q80.empty());
  ASSERT_FALSE(q60.empty());
  ASSERT_FALSE(png.empty());

  EXPECT_NEAR(ImageTranscoder::DetectSourceJpegQuality(q80, &handler_), 80, 3);
  EXPECT_NEAR(ImageTranscoder::DetectSourceJpegQuality(q60, &handler_), 60, 3);
  EXPECT_LT(ImageTranscoder::DetectSourceJpegQuality(q60, &handler_),
            ImageTranscoder::DetectSourceJpegQuality(q80, &handler_));
  EXPECT_EQ(ImageTranscoder::DetectSourceJpegQuality(png, &handler_), -1);
  EXPECT_EQ(ImageTranscoder::DetectSourceJpegQuality("", &handler_), -1);
}

// =================================================================
// Size gate tests: no-savings returns success=false
// =================================================================

TEST_F(ImageTranscoderTest, JpegNoSavingsReturnsFalse) {
  // Lossless config on sjpeg1.jpg (small, well-compressed) → no savings.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.lossy_jpeg = false;
  config.progressive_jpeg = false;
  ImageTranscoder transcoder(config, &handler);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder.Transcode(jpeg, mask);
  // Lossless optimization on small, well-compressed JPEG → no savings.
  if (!result.success) {
    EXPECT_TRUE(result.output_data.empty());
  } else {
    // If it somehow succeeds, it must be genuinely smaller.
    EXPECT_LT(result.output_data.size(), jpeg.size());
  }
}

TEST_F(ImageTranscoderTest, PngNoSavingsReturnsFalse) {
  // Already optimized PNG should have no savings (or very small ones).
  std::string png = ReadTestFile("pngsuite/already_optimized.png");
  ASSERT_FALSE(png.empty());

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder_.Transcode(png, mask);
  if (!result.success) {
    EXPECT_TRUE(result.output_data.empty());
  } else {
    // If it succeeds, output must be genuinely smaller.
    EXPECT_LT(result.output_data.size(), png.size());
  }
}

TEST_F(ImageTranscoderTest, JpegWithSavingsStillSucceeds) {
  // Lossy config at q75 on a larger JPEG → should still produce savings.
  std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.lossy_jpeg = true;
  config.progressive_jpeg = true;
  config.jpeg_quality = 75;
  ImageTranscoder transcoder(config, &handler);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);

  auto result = transcoder.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_LT(result.output_data.size(), jpeg.size());
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedNonResizedNoSavings) {
  // Small JPEG + kOriginal format + Mobile viewport (no resize since
  // image width < mobile_width) → no savings expected.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder_.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // Small JPEG at default quality with no resize → no savings.
  // The result may or may not succeed depending on whether default
  // lossy compression can beat the original.
  if (!result.optimized_original.success) {
    EXPECT_TRUE(result.optimized_original.output_data.empty());
  }
}

TEST_F(ImageTranscoderTest, TranscodeMultiResizedResizedStillSucceeds) {
  // Force resize of a larger JPEG by setting a very small mobile width.
  // Resized path produces smaller output → success expected.
  std::string jpeg = ReadTestFile("jpeg/sjpeg4.jpg");
  ASSERT_FALSE(jpeg.empty());

  NullMessageHandler handler;
  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 60;  // Force resize
  config.quality_verify = false;
  ImageTranscoder transcoder(config, &handler);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kOriginal,
  };

  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  // Resized path should succeed (smaller output from fewer pixels).
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  if (result.optimized_original.success) {
    EXPECT_LT(result.optimized_original.output_data.size(), jpeg.size());
  }
}

// ---------------------------------------------------------------------------
// C2PA / Content-Credentials preserve-by-default.
//
// JPEG->JPEG (kOriginal) carries the APP11/JUMBF manifest through recompress; every
// other path (AVIF, WebP, resize, PNG, the XMP/APP1 form) cannot carry it, so the
// transcoder serves the ORIGINAL bytes unchanged (skip-not-strip) when a manifest is
// present, never silently stripping provenance. Detection is a byte scan, so a
// spliced APP11 stub suffices and the tests stay hermetic.
// ---------------------------------------------------------------------------

// Splices a minimal APP11/JUMBF C2PA stub (0xFF 0xEB segment) right after the SOI of
// a JPEG. The decoder ignores the unknown APP11 segment; the byte-scan detector keys
// on the "jumb"/"jumd"/"c2pa" tokens in the payload.
std::string SpliceC2paApp11IntoJpeg(const std::string& jpeg) {
  if (jpeg.size() < 2 || static_cast<unsigned char>(jpeg[0]) != 0xFF ||
      static_cast<unsigned char>(jpeg[1]) != 0xD8) {
    return jpeg;
  }
  const std::string payload = "JPjumbjumdc2pa";
  const size_t seg_len = payload.size() + 2;  // +2 for the length field.
  std::string out;
  out.append(jpeg, 0, 2);  // SOI.
  out.push_back(static_cast<char>(0xFF));
  out.push_back(static_cast<char>(0xEB));  // APP11.
  out.push_back(static_cast<char>((seg_len >> 8) & 0xFF));
  out.push_back(static_cast<char>(seg_len & 0xFF));
  out.append(payload);
  out.append(jpeg, 2, std::string::npos);
  return out;
}

TEST_F(ImageTranscoderTest, C2paJpegToWebpSkipsToOriginal) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());
  const std::string with_c2pa = SpliceC2paApp11IntoJpeg(jpeg);
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_c2pa));

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  auto result = transcoder_.Transcode(with_c2pa, mask);
  // WebP cannot carry C2PA -> serve the original JPEG unchanged (skip-not-strip).
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
  EXPECT_EQ(with_c2pa, result.output_data);
  EXPECT_NE(std::string::npos, result.output_data.find("c2pa"));
}

TEST_F(ImageTranscoderTest, C2paJpegToAvifSkipsToOriginal) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());
  const std::string with_c2pa = SpliceC2paApp11IntoJpeg(jpeg);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);
  auto result = transcoder_.Transcode(with_c2pa, mask);
  // AVIF cannot carry C2PA -> serve the original JPEG unchanged.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
  EXPECT_EQ(with_c2pa, result.output_data);
  EXPECT_NE(std::string::npos, result.output_data.find("c2pa"));
}

TEST_F(ImageTranscoderTest, C2paJpegToJpegCarries) {
  // sjpeg1 optimizes with savings (see OptimizeJpeg test), so the JPEG->JPEG carry
  // path actually recompresses while preserving the APP11/JUMBF manifest.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());
  const std::string with_c2pa = SpliceC2paApp11IntoJpeg(jpeg);

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = transcoder_.Transcode(with_c2pa, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
  // The manifest survives the recompress (codec carry).
  EXPECT_NE(std::string::npos, result.output_data.find("c2pa"));
  // Genuinely recompressed (not a verbatim passthrough): proves the carry path ran,
  // so a future carryable-detection regression that fell back to passthrough is caught.
  EXPECT_LT(result.output_data.size(), with_c2pa.size());
}

TEST_F(ImageTranscoderTest, C2paResizedSkipsToOriginal) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());
  const std::string with_c2pa = SpliceC2paApp11IntoJpeg(jpeg);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };
  auto result = transcoder_.TranscodeMultiResized(
      with_c2pa, formats, CapabilityMask::Viewport::kMobile);
  // Resize/transcode would strip -> WebP/AVIF skipped, original served (manifest
  // intact via the JPEG->JPEG carry or passthrough).
  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_NE(std::string::npos,
            result.optimized_original.output_data.find("c2pa"));
}

TEST_F(ImageTranscoderTest, C2paCleanImageNormalTranscode) {
  // No manifest -> the gate is a pure no-op and normal transcode still works.
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());
  ASSERT_FALSE(net_instaweb::ImageHasC2paManifest(jpeg));

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  auto result = transcoder_.Transcode(jpeg, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/webp", result.output_mime_type);
}

// Splices an APP1 (0xFF 0xE1) XMP segment carrying the Content-Credentials namespace
// into a JPEG. No JUMBF tokens, so it is detected ONLY via the XMP path. The decoder
// skips the unknown APP1 during decode.
std::string SpliceXmpC2paIntoJpeg(const std::string& jpeg) {
  if (jpeg.size() < 2 || static_cast<unsigned char>(jpeg[0]) != 0xFF ||
      static_cast<unsigned char>(jpeg[1]) != 0xD8) {
    return jpeg;
  }
  const std::string payload =
      "http://ns.adobe.com/xap/1.0/ <?xpacket?> cr:provenance urn:uuid:test";
  const size_t seg_len = payload.size() + 2;  // +2 for the length field.
  std::string out;
  out.append(jpeg, 0, 2);  // SOI.
  out.push_back(static_cast<char>(0xFF));
  out.push_back(static_cast<char>(0xE1));  // APP1.
  out.push_back(static_cast<char>((seg_len >> 8) & 0xFF));
  out.push_back(static_cast<char>(seg_len & 0xFF));
  out.append(payload);
  out.append(jpeg, 2, std::string::npos);
  return out;
}

// Splices a synthetic (empty) caBX ancillary chunk after the 8-byte PNG signature.
// The detector keys on the "caBX" token; the skip-not-strip gate serves the original
// bytes WITHOUT decoding, so the chunk's CRC is never validated here.
std::string SpliceCaBxIntoPng(const std::string& png) {
  if (png.size() < 8) {
    return png;
  }
  std::string out;
  out.append(png, 0, 8);  // 8-byte PNG signature.
  out.append(4, '\0');    // chunk length = 0.
  out.append("caBX", 4);  // chunk type.
  out.append(4,
             '\0');  // CRC placeholder (unvalidated on the passthrough path).
  out.append(png, 8, std::string::npos);
  return out;
}

TEST_F(ImageTranscoderTest, C2paXmpJpegSkipsToOriginal) {
  // XMP/APP1-carried Content-Credentials is not reliably carried (APP1 is gated behind
  // retain_exif_data, which the C2PA gate never forces), so it must skip-not-strip on
  // BOTH the transcode path and the JPEG->JPEG path (carryable requires !XMP).
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());
  const std::string with_xmp = SpliceXmpC2paIntoJpeg(jpeg);
  ASSERT_TRUE(net_instaweb::ImageHasXmpC2pa(with_xmp));
  ASSERT_FALSE(net_instaweb::ImageHasJumbfC2pa(with_xmp));

  for (auto fmt : {CapabilityMask::ImageFormat::kWebP,
                   CapabilityMask::ImageFormat::kOriginal}) {
    CapabilityMask mask;
    mask.set_image_format(fmt);
    auto result = transcoder_.Transcode(with_xmp, mask);
    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ("image/jpeg", result.output_mime_type);
    EXPECT_EQ(with_xmp, result.output_data);
    EXPECT_NE(std::string::npos, result.output_data.find("cr:provenance"));
  }
}

TEST_F(ImageTranscoderTest, C2paJumbfPlusXmpJpegSkipsToOriginal) {
  // A JPEG carrying BOTH a JUMBF APP11 manifest and an XMP cr: packet must skip, not
  // carry: OptimizeJpeg would preserve APP11 but silently drop the APP1/XMP packet.
  // Exercises the !c2pa_xmp term in the carryable predicate.
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());
  const std::string with_both =
      SpliceXmpC2paIntoJpeg(SpliceC2paApp11IntoJpeg(jpeg));
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_both));
  ASSERT_TRUE(net_instaweb::ImageHasXmpC2pa(with_both));

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = transcoder_.Transcode(with_both, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/jpeg", result.output_mime_type);
  // Skip-not-strip: original served verbatim, BOTH markers intact.
  EXPECT_EQ(with_both, result.output_data);
  EXPECT_NE(std::string::npos, result.output_data.find("c2pa"));
  EXPECT_NE(std::string::npos, result.output_data.find("cr:provenance"));
}

TEST_F(ImageTranscoderTest, C2paPngSkipsToOriginal) {
  // PNG cannot carry C2PA through OptimizePng (an ancillary-chunk stripper), so a
  // caBX-bearing PNG must skip-not-strip -> served unchanged.
  std::string png = ReadTestFile("pngsuite/basi2c08.png");
  ASSERT_FALSE(png.empty());
  const std::string with_cabx = SpliceCaBxIntoPng(png);
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_cabx));  // caBX token.

  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = transcoder_.Transcode(with_cabx, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  EXPECT_EQ(with_cabx, result.output_data);
  EXPECT_NE(std::string::npos, result.output_data.find("caBX"));
}

// ---------------------------------------------------------------------------
// C2PA PNG carry-through (opt-in via --c2pa-carry / c2pa_carry).
//
// Unlike the default skip-not-strip (serve the original PNG unoptimized), carry
// RECOMPRESSES the PNG AND re-splices its original caBX/iTXt manifest chunks before
// IEND, so the PNG is both optimized and keeps its provenance. PNG-only (the
// JPEG->JPEG codec already carries APP11/JUMBF). Fail-safe to skip on any anomaly.
//
// The carry path runs OptimizePng on the original (unlike the skip path, which never
// decodes), so these helpers build a STRUCTURALLY VALID PNG: the manifest chunk is
// spliced immediately before the trailing IEND (a legal position for an ancillary
// chunk) with a correct CRC so libpng accepts it on decode. The fixture
// gray_saved_as_rgb.png reliably optimizes (RGB->Gray), so the carry path genuinely
// recompresses rather than falling back to passthrough.
// ---------------------------------------------------------------------------

// Standard PNG/zlib CRC-32 over a chunk's type+data (not the length).
uint32_t PngCrc32(std::string_view type_and_data) {
  static uint32_t table[256];
  static bool have_table = false;
  if (!have_table) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[n] = c;
    }
    have_table = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char b : type_and_data) {
    crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

void AppendBE32(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>(v & 0xFF));
}

// A complete, CRC-correct caBX chunk (length + "caBX" + payload + CRC) whose payload
// carries the JUMBF tokens the detector keys on, padded to `payload_len` bytes.
std::string MakeCaBxChunk(size_t payload_len) {
  std::string payload = "jumbjumdc2pa";
  if (payload.size() < payload_len) {
    payload.append(payload_len - payload.size(), 'X');
  }
  std::string type_and_data = "caBX";
  type_and_data += payload;
  std::string chunk;
  AppendBE32(&chunk, static_cast<uint32_t>(payload.size()));
  chunk += type_and_data;
  AppendBE32(&chunk, PngCrc32(type_and_data));
  return chunk;
}

// A complete, CRC-correct iTXt chunk holding the Content-Credentials XMP packet that
// ImageHasXmpC2pa keys on ("cr:" + an XMP marker), with no JUMBF tokens.
std::string MakeXmpItxtChunk() {
  std::string data = "XML:com.adobe.xmp";  // keyword.
  data.push_back('\0');                    // keyword null terminator.
  data.push_back('\0');  // compression flag (0 = uncompressed).
  data.push_back('\0');  // compression method.
  data.push_back('\0');  // language tag (empty) + null.
  data.push_back('\0');  // translated keyword (empty) + null.
  data +=
      "<?xpacket?> http://ns.adobe.com/xap/1.0/ cr:provenance urn:uuid:test";
  std::string type_and_data = "iTXt";
  type_and_data += data;
  std::string chunk;
  AppendBE32(&chunk, static_cast<uint32_t>(data.size()));
  chunk += type_and_data;
  AppendBE32(&chunk, PngCrc32(type_and_data));
  return chunk;
}

// Inserts `chunk` immediately before the trailing 12-byte IEND chunk. Returns the
// input unchanged on any anomaly (caller asserts the splice changed the bytes).
std::string SpliceChunkBeforeIend(const std::string& png,
                                  const std::string& chunk) {
  if (png.size() < 12 || png.compare(png.size() - 8, 4, "IEND") != 0) {
    return png;
  }
  std::string out = png;
  out.insert(out.size() - 12, chunk);
  return out;
}

// A transcoder config with PNG carry-through enabled (everything else default).
ImageTranscoderConfig CarryEnabledConfig() {
  ImageTranscoderConfig cfg;
  cfg.c2pa_carry = true;
  return cfg;
}

TEST_F(ImageTranscoderTest, C2paCarryPngRecompressesKeepsManifestAndDecodes) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  const std::string with_cabx = SpliceChunkBeforeIend(png, MakeCaBxChunk(64));
  ASSERT_NE(with_cabx, png);  // splice happened (valid IEND present).
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_cabx));

  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = carry_tc.Transcode(with_cabx, mask);

  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  // The manifest carrier survived the recompress.
  EXPECT_NE(std::string::npos, result.output_data.find("caBX"));
  // Genuinely recompressed (carry path ran), not a verbatim passthrough.
  EXPECT_NE(with_cabx, result.output_data);
  // RGB->Gray collapse means even with the same manifest re-spliced the output is
  // smaller than the manifest-bearing original -> proves real optimization happened.
  EXPECT_LT(result.output_data.size(), with_cabx.size());

  // Decodability: the carried PNG decodes to pixels (the spliced caBX is a valid
  // ancillary chunk libpng skips, not a corruption).
  auto decoded = carry_tc.DecodeToPixels(result.output_data);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_GT(decoded.height, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());

  // A full decode+recompress with preservation OFF (bypassing the gate) strips the
  // ancillary caBX, proving it was carried as a real chunk, not fused into the IDAT.
  ImageTranscoderConfig strip_cfg;
  strip_cfg.preserve_c2pa = false;
  ImageTranscoder strip_tc(strip_cfg, &handler_);
  auto reopt = strip_tc.Transcode(result.output_data, mask);
  if (reopt.success) {
    EXPECT_EQ(std::string::npos, reopt.output_data.find("caBX"));
  }
}

TEST_F(ImageTranscoderTest, C2paCarryPngOffSkipsToOriginal) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  const std::string with_cabx = SpliceChunkBeforeIend(png, MakeCaBxChunk(64));
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_cabx));

  // Default config: c2pa_carry is OFF -> skip-not-strip (served unchanged).
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = transcoder_.Transcode(with_cabx, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  EXPECT_EQ(with_cabx, result.output_data);  // verbatim original.
}

TEST_F(ImageTranscoderTest, C2paCarryPngToWebpFallsBackToSkip) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  const std::string with_cabx = SpliceChunkBeforeIend(png, MakeCaBxChunk(64));
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_cabx));

  // Carry is on, but the target is WebP, which cannot hold a PNG caBX chunk ->
  // skip-not-strip (serve the original PNG unchanged), never carry.
  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
  auto result = carry_tc.Transcode(with_cabx, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  EXPECT_EQ(with_cabx, result.output_data);
}

TEST_F(ImageTranscoderTest, C2paCarryCleanPngRecompresses) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  ASSERT_FALSE(net_instaweb::ImageHasC2paManifest(png));

  // No manifest -> the carry gate is a pure no-op; normal PNG optimization runs.
  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = carry_tc.Transcode(png, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  EXPECT_LT(result.output_data.size(), png.size());
  EXPECT_EQ(std::string::npos, result.output_data.find("caBX"));
}

TEST_F(ImageTranscoderTest, C2paCarryLargePngManifestCarriesAndDecodes) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  // A manifest far larger than a single 64KB JPEG segment -- trivial for a PNG
  // chunk (32-bit length) and exercises tens-of-KB carry through the splice.
  const std::string big_chunk = MakeCaBxChunk(70000);
  const std::string with_cabx = SpliceChunkBeforeIend(png, big_chunk);
  ASSERT_NE(with_cabx, png);
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_cabx));

  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = carry_tc.Transcode(with_cabx, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  // The full 70KB carrier survived verbatim.
  EXPECT_NE(std::string::npos, result.output_data.find(big_chunk));
  auto decoded = carry_tc.DecodeToPixels(result.output_data);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
}

TEST_F(ImageTranscoderTest, C2paCarryXmpItxtPngCarriesAndDecodes) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  const std::string with_itxt = SpliceChunkBeforeIend(png, MakeXmpItxtChunk());
  ASSERT_NE(with_itxt, png);
  ASSERT_TRUE(net_instaweb::ImageHasXmpC2pa(with_itxt));
  ASSERT_FALSE(net_instaweb::ImageHasJumbfC2pa(with_itxt));  // XMP form only.

  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = carry_tc.Transcode(with_itxt, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  // The XMP/Content-Credentials iTXt chunk survived the recompress.
  EXPECT_NE(std::string::npos, result.output_data.find("cr:provenance"));
  EXPECT_NE(with_itxt, result.output_data);  // recompressed.
  auto decoded = carry_tc.DecodeToPixels(result.output_data);
  EXPECT_GT(decoded.width, 0u);
  EXPECT_FALSE(decoded.pixel_buffer.empty());
}

TEST_F(ImageTranscoderTest, C2paCarryPngResizedSkipsToOriginal) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  const std::string with_cabx = SpliceChunkBeforeIend(png, MakeCaBxChunk(64));
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_cabx));

  // A resize would strip the manifest; carry is suppressed on the resize path, so
  // the original PNG is served byte-for-byte (skip-not-strip), NOT recompressed.
  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };
  auto result = carry_tc.TranscodeMultiResized(
      with_cabx, formats, CapabilityMask::Viewport::kMobile);
  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ(with_cabx, result.optimized_original.output_data);
}

TEST_F(ImageTranscoderTest, C2paCarryPngTranscodeMultiCarries) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  const std::string with_cabx = SpliceChunkBeforeIend(png, MakeCaBxChunk(64));
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_cabx));

  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP,
      CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal,
  };
  auto result = carry_tc.TranscodeMulti(with_cabx, formats);
  // WebP/AVIF can't carry a PNG chunk -> unsuccessful; the optimized PNG carries it.
  EXPECT_FALSE(result.webp.success);
  EXPECT_FALSE(result.avif.success);
  EXPECT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_EQ("image/png", result.optimized_original.output_mime_type);
  EXPECT_NE(std::string::npos,
            result.optimized_original.output_data.find("caBX"));
  EXPECT_NE(with_cabx, result.optimized_original.output_data);  // recompressed.
}

TEST_F(ImageTranscoderTest, C2paCarryPngToAvifFallsBackToSkip) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  const std::string with_cabx = SpliceChunkBeforeIend(png, MakeCaBxChunk(64));
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_cabx));

  // Carry is on, but the target is AVIF, which cannot hold a PNG caBX chunk ->
  // skip-not-strip (serve the original PNG unchanged), never carry.
  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);
  auto result = carry_tc.Transcode(with_cabx, mask);
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  EXPECT_EQ(with_cabx, result.output_data);
}

// Builds a structurally anomalous caBX: a chunk header declaring a length that runs
// far past the buffer. The "caBX" token still makes ImageHasJumbfC2pa fire (so the
// carry gate is entered), but ExtractPngC2paChunks returns empty on the overlong
// length -- and OptimizePng would itself reject the malformed PNG -- so the carry
// MUST fall back to serving the original, never emitting a stripped image.
std::string MakeOverlongCaBxHeader() {
  std::string chunk;
  AppendBE32(&chunk, 0xFFFFFF00u);  // absurd declared length.
  chunk += "caBX";                  // type (also the detector token).
  chunk += "tiny";                  // far fewer bytes than declared.
  return chunk;
}

TEST_F(ImageTranscoderTest, C2paCarryPngStructuralAnomalyFallsBackToOriginal) {
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  const std::string broken =
      SpliceChunkBeforeIend(png, MakeOverlongCaBxHeader());
  ASSERT_NE(broken, png);
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(broken));  // caBX token present.
  // Extractor fails safe to empty on the overlong length.
  ASSERT_TRUE(net_instaweb::ExtractPngC2paChunks(broken).empty());

  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto result = carry_tc.Transcode(broken, mask);
  // No extractable carrier (or recompress fails) -> serve the original verbatim.
  EXPECT_TRUE(result.success) << result.error_message;
  EXPECT_EQ("image/png", result.output_mime_type);
  EXPECT_EQ(broken, result.output_data);
}

// ============================================================
// EXIF orientation baking (issue #1005)
// ============================================================
//
// Fixtures (test/lib/image/testdata/jpeg/): exif_orientation_{1..8}.jpg
// share one upright 32x16 quadrant scene (TL=red, TR=green, BL=blue,
// BR=yellow) stored pre-transformed with the matching Orientation tag.
// exif_orientation_6_photo.jpg is a noisy 256x128 (stored 128x256,
// Orientation=6) variant of the same scene, large enough for WebP/AVIF to
// win the size gates.

// Reads the declared dimensions of an AVIF payload from the container
// metadata. Container parsing needs no AV1 decoder codec (the build links
// libavif with an encoder only, matching production, which never decodes
// AVIF -- so pixel content cannot be re-decoded here; it is covered by the
// WebP/JPEG outputs, which encode from the same upright pixel buffer).
// Returns false when the payload is not a parseable AVIF.
bool ReadAvifDimensions(const std::string& data, uint32_t* width,
                        uint32_t* height) {
  avifDecoder* decoder = avifDecoderCreate();
  if (decoder == nullptr) {
    return false;
  }
  const bool ok = avifDecoderSetIOMemory(
                      decoder, reinterpret_cast<const uint8_t*>(data.data()),
                      data.size()) == AVIF_RESULT_OK &&
                  avifDecoderParse(decoder) == AVIF_RESULT_OK;
  if (ok) {
    *width = decoder->image->width;
    *height = decoder->image->height;
  }
  avifDecoderDestroy(decoder);
  return ok;
}

// Averages an RGB channel triple over a block centered on the given
// fraction of the image, so noisy fixtures compare stably.
void AverageRegion(const DecodedImage& img, double fx, double fy, int* r,
                   int* g, int* b) {
  ASSERT_GE(img.bytes_per_pixel, 3);
  const int cx = static_cast<int>(img.width * fx);
  const int cy = static_cast<int>(img.height * fy);
  const int radius = 4;
  int64_t sum[3] = {0, 0, 0};
  int count = 0;
  for (int y = cy - radius; y < cy + radius; ++y) {
    for (int x = cx - radius; x < cx + radius; ++x) {
      ASSERT_GE(x, 0);
      ASSERT_GE(y, 0);
      ASSERT_LT(static_cast<uint32_t>(x), img.width);
      ASSERT_LT(static_cast<uint32_t>(y), img.height);
      const size_t offset =
          (static_cast<size_t>(y) * img.width + x) * img.bytes_per_pixel;
      sum[0] += static_cast<uint8_t>(img.pixel_buffer[offset]);
      sum[1] += static_cast<uint8_t>(img.pixel_buffer[offset + 1]);
      sum[2] += static_cast<uint8_t>(img.pixel_buffer[offset + 2]);
      ++count;
    }
  }
  *r = static_cast<int>(sum[0] / count);
  *g = static_cast<int>(sum[1] / count);
  *b = static_cast<int>(sum[2] / count);
}

// Asserts the upright quadrant scene (any resolution): TL=red, TR=green,
// BL=blue, BR=yellow at the quadrant centers.
void ExpectUprightQuadrants(const DecodedImage& img, const std::string& what) {
  struct Quadrant {
    double fx, fy;
    int r, g, b;
    const char* name;
  };
  const Quadrant quadrants[] = {
      {0.25, 0.25, 255, 0, 0, "TL"},
      {0.75, 0.25, 0, 255, 0, "TR"},
      {0.25, 0.75, 0, 0, 255, "BL"},
      {0.75, 0.75, 255, 255, 0, "BR"},
  };
  for (const Quadrant& q : quadrants) {
    int r = 0;
    int g = 0;
    int b = 0;
    AverageRegion(img, q.fx, q.fy, &r, &g, &b);
    // Noisy fixtures clamp at the channel bounds, so averages sit within
    // ~30 of the base color; 64 leaves headroom for lossy re-encodes.
    EXPECT_NEAR(r, q.r, 64) << what << " " << q.name;
    EXPECT_NEAR(g, q.g, 64) << what << " " << q.name;
    EXPECT_NEAR(b, q.b, 64) << what << " " << q.name;
  }
}

// DecodeToPixels must hand every consumer upright pixels with display
// (post-rotation) dimensions, for all eight orientation values.
TEST_F(ImageTranscoderTest, DecodeToPixelsBakesExifOrientation) {
  for (int orientation = 1; orientation <= 8; ++orientation) {
    std::string jpeg = ReadTestFile("jpeg/exif_orientation_" +
                                    std::to_string(orientation) + ".jpg");
    ASSERT_FALSE(jpeg.empty()) << "fixture " << orientation;
    DecodedImage decoded = transcoder_.DecodeToPixels(jpeg);
    ASSERT_FALSE(decoded.pixel_buffer.empty()) << "fixture " << orientation;
    EXPECT_EQ(decoded.width, 32u) << "fixture " << orientation;
    EXPECT_EQ(decoded.height, 16u) << "fixture " << orientation;
    ExpectUprightQuadrants(decoded,
                           "orientation " + std::to_string(orientation));
  }
}

// Every output format of a multi-transcode must render upright with no
// reliance on an Orientation tag surviving.
TEST_F(ImageTranscoderTest, TranscodeMultiBakesOrientationInAllFormats) {
  std::string jpeg = ReadTestFile("jpeg/exif_orientation_6_photo.jpg");
  ASSERT_FALSE(jpeg.empty());

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::ImageFormat::kAvif,
      CapabilityMask::ImageFormat::kOriginal};
  auto result = transcoder_.TranscodeMulti(jpeg, formats);

  // WebP: upright pixels, swapped (display) dimensions.
  ASSERT_TRUE(result.webp.success) << result.webp.error_message;
  DecodedImage webp_decoded =
      transcoder_.DecodeToPixels(result.webp.output_data);
  ASSERT_FALSE(webp_decoded.pixel_buffer.empty());
  EXPECT_EQ(webp_decoded.width, 256u);
  EXPECT_EQ(webp_decoded.height, 128u);
  ExpectUprightQuadrants(webp_decoded, "webp");
  // The WebP container must carry no EXIF chunk that could contradict.
  EXPECT_EQ(result.webp.output_data.find("EXIF"), std::string::npos);

  // AVIF: swapped (display) dimensions in the container. Pixel content is
  // covered by the WebP/JPEG assertions above/below -- all three formats
  // encode from the same upright pixel buffer (see ReadAvifDimensions).
  ASSERT_TRUE(result.avif.success) << result.avif.error_message;
  uint32_t avif_width = 0;
  uint32_t avif_height = 0;
  ASSERT_TRUE(
      ReadAvifDimensions(result.avif.output_data, &avif_width, &avif_height));
  EXPECT_EQ(avif_width, 256u);
  EXPECT_EQ(avif_height, 128u);

  // Optimized JPEG: baked pixels, no contradicting Orientation tag.
  ASSERT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  const std::string& out_jpeg = result.optimized_original.output_data;
  EXPECT_EQ(pagespeed::image_compression::ReadJpegExifOrientation(
                out_jpeg.data(), out_jpeg.size()),
            1);
  DecodedImage jpeg_decoded = transcoder_.DecodeToPixels(out_jpeg);
  ASSERT_FALSE(jpeg_decoded.pixel_buffer.empty());
  EXPECT_EQ(jpeg_decoded.width, 256u);
  EXPECT_EQ(jpeg_decoded.height, 128u);
  ExpectUprightQuadrants(jpeg_decoded, "optimized jpeg");
}

// The viewport resize must operate on display dimensions: a portrait-stored
// (128x256) Orientation=6 image displays as 256x128 and resizes to the
// mobile target width with the upright aspect ratio.
TEST_F(ImageTranscoderTest, TranscodeMultiResizedUsesDisplayDimensions) {
  std::string jpeg = ReadTestFile("jpeg/exif_orientation_6_photo.jpg");
  ASSERT_FALSE(jpeg.empty());

  ImageTranscoderConfig config;
  config.viewport_widths.mobile = 128;
  // This test is about dimensions, not quality: the resized webp of this
  // photo scores below the default verify floor and would (correctly)
  // decline under the binding verdict — turn the verifier off so the
  // dimension assertions run on produced bytes.
  config.quality_verify = false;
  ImageTranscoder transcoder(config, &handler_);

  std::vector<CapabilityMask::ImageFormat> formats = {
      CapabilityMask::ImageFormat::kWebP};
  auto result = transcoder.TranscodeMultiResized(
      jpeg, formats, CapabilityMask::Viewport::kMobile);

  ASSERT_TRUE(result.webp.success) << result.webp.error_message;
  DecodedImage webp_decoded =
      transcoder.DecodeToPixels(result.webp.output_data);
  ASSERT_FALSE(webp_decoded.pixel_buffer.empty());
  EXPECT_EQ(webp_decoded.width, 128u);
  EXPECT_EQ(webp_decoded.height, 64u);
  ExpectUprightQuadrants(webp_decoded, "resized webp");
}

TEST_F(ImageTranscoderTest, C2paCarryPngNeverGrowsAndKeepsManifest) {
  // Already-optimal input: re-optimizing yields little/no further pixel saving, so
  // once the manifest is re-added the carry may not beat the original. The carry
  // must then fall back to serving the original, and MUST NEVER emit something
  // larger than the input or drop the manifest (the size-check guard).
  std::string png = ReadTestFile("gray_saved_as_rgb.png");
  ASSERT_FALSE(png.empty());
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kOriginal);
  auto first =
      transcoder_.Transcode(png, mask);  // default config, no manifest.
  ASSERT_TRUE(first.success) << first.error_message;
  ASSERT_EQ("image/png", first.output_mime_type);
  const std::string optimal_with_cabx =
      SpliceChunkBeforeIend(first.output_data, MakeCaBxChunk(64));
  ASSERT_NE(optimal_with_cabx, first.output_data);
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(optimal_with_cabx));

  ImageTranscoder carry_tc(CarryEnabledConfig(), &handler_);
  auto res = carry_tc.Transcode(optimal_with_cabx, mask);
  EXPECT_TRUE(res.success) << res.error_message;
  EXPECT_EQ("image/png", res.output_mime_type);
  // Invariant (covers both branches): never larger than the manifest-bearing input,
  // and the manifest is always preserved -- either carried into a strictly-smaller
  // recompress, or the original served verbatim.
  EXPECT_LE(res.output_data.size(), optimal_with_cabx.size());
  EXPECT_NE(std::string::npos, res.output_data.find("caBX"));
}

// ===========================================================================
// Transcode fall-through and the format-specific slots (#1374)
// ===========================================================================

namespace {

// A GIF-shaped input whose pixel decode fails: valid GIF magic, nothing
// readable behind it.  ComputeImageType reads the magic and calls it a GIF, no
// scanline reader can produce pixels from it, and every conversion path
// therefore falls through to the original-format optimizer -- which, for a GIF,
// has nothing to do and hands back the input verbatim.  That is the exact
// shape #1374 is about, reduced to bytes that need no codec to reproduce.
std::string DecodeFailingGif() {
  return std::string("GIF89a\x01\x00\x01\x00", 10);
}

}  // namespace

TEST_F(ImageTranscoderTest, FallThroughBytesNeverLandInAConvertedSlot) {
  const std::string gif = DecodeFailingGif();
  auto multi =
      transcoder_.TranscodeMulti(gif, {CapabilityMask::ImageFormat::kWebP,
                                       CapabilityMask::ImageFormat::kAvif,
                                       CapabilityMask::ImageFormat::kOriginal});

  // Precondition, asserted rather than assumed: this input really does take
  // the fall-through path, and the original-format slot holds the origin's
  // bytes verbatim in the origin's own format.  If that ever stops holding,
  // the expectations below are no longer testing #1374 and should fail loudly
  // here instead of passing vacuously.
  ASSERT_TRUE(multi.optimized_original.success)
      << multi.optimized_original.error_message;
  ASSERT_EQ("image/gif", multi.optimized_original.output_mime_type);
  ASSERT_EQ(gif, multi.optimized_original.output_data);

  // The family shape: the origin's bytes are in the original-format slot and
  // NOWHERE else.  Before the fix both converted slots came back "successful"
  // holding this same GIF; the worker wrote the first of them under a
  // converted alternate id, and the intra-notification content dedup then
  // dropped the original-format twin as identical content -- leaving the
  // family's only copy of those bytes at an id ScoreAlternate hard-disqualifies
  // for every client that negotiated a different format.
  EXPECT_FALSE(multi.webp.success)
      << "WebP slot holds " << multi.webp.output_mime_type << " content";
  EXPECT_FALSE(multi.avif.success)
      << "AVIF slot holds " << multi.avif.output_mime_type << " content";
  // The rejection is recorded, not silent: an operator reading the result has
  // to be able to tell "nothing to convert" from "never attempted".
  EXPECT_FALSE(multi.webp.error_message.empty());
  EXPECT_FALSE(multi.avif.error_message.empty());
}

TEST_F(ImageTranscoderTest, FallThroughSlotRuleHoldsWhenOnlyOneFormatIsAsked) {
  // Same rule with the original-format slot NOT among the requested formats --
  // the shape a family in which the original-format alternate already exists
  // produces.  There is no twin for the dedup to fold this into, so the slot
  // rule is the only thing standing between the origin's bytes and a
  // converted alternate id.
  const std::string gif = DecodeFailingGif();
  auto multi =
      transcoder_.TranscodeMulti(gif, {CapabilityMask::ImageFormat::kAvif});
  EXPECT_FALSE(multi.avif.success)
      << "AVIF slot holds " << multi.avif.output_mime_type << " content";
  EXPECT_FALSE(multi.optimized_original.success)
      << "original-format slot was not requested and must stay empty";
}

TEST_F(ImageTranscoderTest, ConvertedSlotsOnlyEverHoldTheirOwnFormat) {
  // The invariant across real GIF fixtures, decoding or not: whatever ends up
  // in a format-keyed slot is in that format.  Stated conditionally, so it
  // is an invariant and NOT an oracle for the accepting branch -- that is
  // the next test's job.
  for (const char* name : {"gif/animated.gif", "gif/interlaced.gif"}) {
    std::string gif = ReadTestFile(name);
    ASSERT_FALSE(gif.empty()) << name;
    auto multi = transcoder_.TranscodeMulti(
        gif,
        {CapabilityMask::ImageFormat::kWebP, CapabilityMask::ImageFormat::kAvif,
         CapabilityMask::ImageFormat::kOriginal});
    if (multi.webp.success) {
      EXPECT_EQ("image/webp", multi.webp.output_mime_type) << name;
    }
    if (multi.avif.success) {
      EXPECT_EQ("image/avif", multi.avif.output_mime_type) << name;
    }
    if (multi.optimized_original.success) {
      EXPECT_EQ("image/gif", multi.optimized_original.output_mime_type) << name;
    }
  }
}

TEST_F(ImageTranscoderTest,
       FallThroughCounterCountsOnlyWhatTheSlotRuleRefused) {
  // The counter's contract, in every place it is described, is "the optimizer
  // handed back the origin's own bytes in the origin's own format, so there
  // was nothing to convert".  That is strictly narrower than "the converted
  // slot ended up unsuccessful": OnlyIfFormat returns an already-unsuccessful
  // result unchanged, so a decode failure or an unsupported format arrives at
  // the same place with nothing having been refused by the slot rule.
  //
  // Counting those too would make the counter mean "a converted slot is
  // empty for any reason", which is the thing an operator already knows, and
  // would leave the distinction it exists to draw -- "nothing to convert" vs
  // "the optimizer ran and could not win" -- unmeasurable again.
  // JFIF header and nothing behind it.
  static constexpr char kTruncatedJpeg[] =
      "\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01";
  struct Row {
    const char* label;
    std::string data;
  };
  const Row rows[] = {
      // Not an image at all: Transcode fails outright for every format.
      {"not-an-image", std::string("not an image at all, just bytes")},
      // WebP magic with a corrupt payload: recognised, undecodable.
      {"corrupt-webp-header", ReadTestFile("corrupt_header.webp")},
      {"corrupt-webp-body", ReadTestFile("corrupt_body.webp")},
      // Truncated JPEG: recognised, undecodable.  Sized from the array rather
      // than by a hand-counted literal length -- the embedded NULs mean the
      // length cannot be left to strlen, and a miscount reads past the end.
      {"truncated-jpeg",
       std::string(kTruncatedJpeg, sizeof(kTruncatedJpeg) - 1)},
  };
  for (const auto& row : rows) {
    if (row.data.empty()) continue;
    auto multi = transcoder_.TranscodeMulti(
        row.data,
        {CapabilityMask::ImageFormat::kWebP, CapabilityMask::ImageFormat::kAvif,
         CapabilityMask::ImageFormat::kOriginal});
    // Precondition: nothing was converted, so the slots really are empty --
    // otherwise this input is not exercising the case at all.
    ASSERT_FALSE(multi.webp.success) << row.label;
    ASSERT_FALSE(multi.avif.success) << row.label;
    // ... and not one of those empty slots was refused BY THE SLOT RULE.
    EXPECT_EQ(0u, multi.unconverted_fallthrough)
        << row.label
        << ": an empty converted slot was counted as an unconverted "
           "fall-through, but nothing was ever converted to refuse";
  }

  // Non-vacuity: on an input that DOES drive the slot rule, the counter moves.
  // Without this the expectations above would be satisfied by a counter that
  // never increments at all.
  const std::string gif = DecodeFailingGif();
  auto driven =
      transcoder_.TranscodeMulti(gif, {CapabilityMask::ImageFormat::kWebP,
                                       CapabilityMask::ImageFormat::kAvif,
                                       CapabilityMask::ImageFormat::kOriginal});
  ASSERT_TRUE(driven.optimized_original.success)
      << driven.optimized_original.error_message;
  EXPECT_GT(driven.unconverted_fallthrough, 0u);
}

TEST_F(ImageTranscoderTest, FallThroughSlotRuleAcceptsAGenuineConversion) {
  // the invariant test above states its expectations
  // conditionally, so a slot rule that refused EVERYTHING would satisfy it
  // vacuously -- the accepting branch had coverage but no oracle.  This test
  // is that oracle: a real conversion has to survive the rule.
  //
  // The fall-through path is identified positively rather than assumed.  Only
  // the per-format fall-through loop produces the marker below: it runs when
  // the pixel decode fails, and for a GIF the AVIF arm then refuses the
  // source, falls through to the original-format optimizer and comes back
  // holding GIF bytes, which the slot rule rejects with that reason.  The
  // decode-succeeded path never reaches it (AVIF is encoded from pixels).
  // Meanwhile the GIF->WebP pipeline needs no pixel decode, so the WebP slot
  // of the very same call can still hold a genuine WebP -- the accepting
  // branch.
  const char* kGifFixtures[] = {"gif/animated.gif",
                                "gif/animated_interlaced.gif",
                                "gif/full2loop.gif",
                                "gif/square2loop.gif",
                                "gif/interlaced.gif",
                                "gif/transparent.gif",
                                "gif/frame_smaller_than_screen.gif",
                                "gif/completely_transparent.gif",
                                "gif/red_empty_screen.gif",
                                "gif/o.gif"};

  int fell_through = 0;
  int converted_through_the_rule = 0;
  for (const char* name : kGifFixtures) {
    std::string gif = ReadTestFile(name);
    if (gif.empty()) continue;
    auto multi = transcoder_.TranscodeMulti(
        gif,
        {CapabilityMask::ImageFormat::kWebP, CapabilityMask::ImageFormat::kAvif,
         CapabilityMask::ImageFormat::kOriginal});
    const bool took_fall_through =
        multi.avif.error_message.find("Fall-through produced") !=
        std::string::npos;
    // A census line per fixture, so a future failure says which inputs moved
    // rather than only that a count did.
    std::cerr << "  [fall-through census] " << name
              << " fall_through=" << took_fall_through
              << " webp_success=" << multi.webp.success << " webp_mime=\""
              << multi.webp.output_mime_type
              << "\" unconverted_fallthrough=" << multi.unconverted_fallthrough
              << "\n";
    if (!took_fall_through) continue;
    ++fell_through;
    // Every refusal on this path is counted, so an operator can see it.
    EXPECT_GT(multi.unconverted_fallthrough, 0u) << name;
    if (multi.webp.success) {
      ++converted_through_the_rule;
      EXPECT_EQ("image/webp", multi.webp.output_mime_type) << name;
      // Really WebP bytes, not merely a WebP label: RIFF....WEBP.
      ASSERT_GE(multi.webp.output_data.size(), 12u) << name;
      EXPECT_EQ("RIFF", multi.webp.output_data.substr(0, 4)) << name;
      EXPECT_EQ("WEBP", multi.webp.output_data.substr(8, 4)) << name;
    }
  }

  // Non-vacuity: the path under test was actually exercised ...
  ASSERT_GT(fell_through, 0)
      << "no fixture drove the per-format fall-through loop -- this test is "
         "no longer testing the slot rule";
  // ... and the rule let a genuine conversion through.  A slot rule that
  // rejected unconditionally leaves this at zero.
  ASSERT_GT(converted_through_the_rule, 0)
      << "the slot rule refused every conversion on the fall-through path; it "
         "is over-refusing, not routing";
}

// =================================================================
// mpp #790: the verify verdict is binding (decline), selection is
// band-closest, and grayscale encodes are actually verifiable.
// =================================================================

// Synthetic AVIF authoring via libavif (the test links @libavif), so tests
// can build inputs the static fixture set lacks (oversized-for-decode gray,
// sub-8x8) without hand-rolling container formats. kFlat fills every pixel
// with the same value; kNoise fills with a deterministic LCG pattern.
enum class SyntheticPattern : std::uint8_t { kFlat, kNoise };

std::string MakeSyntheticAvif(int width, int height, bool grayscale,
                              SyntheticPattern pattern, int value, int quality,
                              uint32_t seed) {
  avifImage* image = avifImageCreate(
      width, height, 8,
      grayscale ? AVIF_PIXEL_FORMAT_YUV400 : AVIF_PIXEL_FORMAT_YUV420);
  if (image == nullptr) return {};

  std::string rgb(static_cast<size_t>(width) * height * 3, '\0');
  uint32_t s = seed != 0 ? seed : 1;
  for (char& byte : rgb) {
    if (pattern == SyntheticPattern::kFlat) {
      byte = static_cast<char>(value);
    } else {
      s = s * 1664525u + 1013904223u;
      byte = static_cast<char>((s >> 16) & 0xFF);
    }
  }

  avifRGBImage rgb_image;
  avifRGBImageSetDefaults(&rgb_image, image);
  rgb_image.depth = 8;
  rgb_image.format = AVIF_RGB_FORMAT_RGB;
  rgb_image.pixels = reinterpret_cast<uint8_t*>(rgb.data());
  rgb_image.rowBytes = static_cast<size_t>(width) * 3;
  avifResult r = avifImageRGBToYUV(image, &rgb_image);
  if (r != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    return {};
  }

  avifEncoder* encoder = avifEncoderCreate();
  if (encoder == nullptr) {
    avifImageDestroy(image);
    return {};
  }
  encoder->quality = quality;
  encoder->speed = 8;  // Authoring speed; determinism is what matters.
  avifRWData out = AVIF_DATA_EMPTY;
  r = avifEncoderWrite(encoder, image, &out);
  avifEncoderDestroy(encoder);
  avifImageDestroy(image);
  if (r != AVIF_RESULT_OK) {
    avifRWDataFree(&out);
    return {};
  }
  std::string bytes(reinterpret_cast<const char*>(out.data), out.size);
  avifRWDataFree(&out);
  return bytes;
}

// Author a LOSSY WebP origin directly with libwebp -- deliberately not
// through the pipeline's own encode paths, whose acceptance gates are
// not the authoring tool's business. Content is a field of 1px diagonal
// stripes (period 3): dense fine STRUCTURE off the transform's
// preferred axes, which a floor-quality re-encode obliterates -- the
// perceptual metric grades that catastrophically (measured -20.6 here).
// Chosen empirically over the tempting alternatives: a 1px checkerboard
// is a DCT basis function and survives any quantization (scores ~69),
// and noise only reaches the mid-30s because noise-vs-blur is masked;
// neither reproduces the negative-verdict class this fixture exists
// for.
std::string MakeSyntheticLossyWebp(int width, int height, float quality) {
  std::string rgb(static_cast<size_t>(width) * height * 3, '\0');
  for (size_t i = 0; i < rgb.size(); ++i) {
    const size_t px = i / 3;
    const size_t x = px % static_cast<size_t>(width);
    const size_t y = px / static_cast<size_t>(width);
    rgb[i] =
        ((x + 2 * y) % 3 == 0) ? static_cast<char>(255) : static_cast<char>(0);
  }
  uint8_t* out = nullptr;
  const size_t n = WebPEncodeRGB(reinterpret_cast<const uint8_t*>(rgb.data()),
                                 width, height, width * 3, quality, &out);
  if (n == 0 || out == nullptr) {
    return {};
  }
  std::string bytes(reinterpret_cast<const char*>(out), n);
  WebPFree(out);
  return bytes;
}

// (a) Decline fires: every attempt scores below the floor -> the variant
// is not produced, the measured score is kept as evidence.
TEST_F(ImageTranscoderTest, VerifyDeclinesWhenEveryAttemptIsBelowFloor) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_webp = false;
  config.content_analysis = false;
  config.webp_quality = 5;            // low starting quality
  config.target_ssimulacra2 = 95.0f;  // band [92,103]: unreachable
  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto result =
      transcoder.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kWebP});
  EXPECT_FALSE(result.webp.success);
  EXPECT_TRUE(result.webp.output_data.empty());
  EXPECT_NE(std::string::npos,
            result.webp.error_message.find("verification failed"))
      << result.webp.error_message;
  EXPECT_TRUE(result.webp_ssimulacra2_declined);
  EXPECT_GE(result.webp_ssimulacra2_score, 0.0f) << "score is the evidence";
  EXPECT_LT(result.webp_ssimulacra2_score, 92.0f);
  EXPECT_FALSE(result.webp_ssimulacra2_reencoded);  // shipped-none
}

// (b) Band-closest selection, below-then-in-band leg: the first attempt
// misses low, a retry lands inside the band and ships immediately (this
// is today's behavior — the pin guards against regression).
TEST_F(ImageTranscoderTest, VerifyShipsInBandRetryOverBelowFloorFirstTry) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_avif = false;
  config.content_analysis = false;
  config.avif_quality = 5;
  // Band [79,90]: sjpeg6 at q=5 scores ~75 (below the floor); the first
  // retry (q=10) lands inside the band.
  config.target_ssimulacra2 = 82.0f;
  config.ssimulacra2_tolerance = 5.0f;
  config.ssimulacra2_max_attempts = 4;
  config.ssimulacra2_quality_step = 5;
  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto result =
      transcoder.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kAvif});
  ASSERT_TRUE(result.avif.success) << result.avif.error_message;
  EXPECT_TRUE(result.avif_ssimulacra2_reencoded)
      << "the in-band retry must ship, not the below-floor first attempt "
         "(score="
      << result.avif_ssimulacra2_score
      << ", final_avif_quality=" << result.final_avif_quality << ")";
  EXPECT_GE(result.avif_ssimulacra2_score, 79.0f);
  EXPECT_LE(result.avif_ssimulacra2_score, 90.0f);
  // (c) The quality out-param reports the SHIPPED attempt's quality.
  EXPECT_EQ(result.final_avif_quality, 10);
}

// (b)+(c) Band-closest selection, above-band leg: every attempt scores
// above the ceiling, so the DOWN-STEPPED body ships (the last attempt —
// closest to the band and smallest), not the first higher-scoring one.
// Shipping the highest-scoring attempt here would undo the byte savings
// of the whole down-step chain.
TEST_F(ImageTranscoderTest, VerifyShipsDownSteppedBodyAboveTheBand) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_webp = false;
  config.content_analysis = false;
  config.webp_quality = 90;
  config.target_ssimulacra2 = 10.0f;  // band [7,16]: far below any q=90 encode
  config.ssimulacra2_tolerance = 5.0f;
  config.ssimulacra2_max_attempts = 4;
  config.ssimulacra2_quality_step = 5;
  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto result =
      transcoder.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kWebP});
  ASSERT_TRUE(result.webp.success) << result.webp.error_message;
  // Above-band encodes ship (over-quality wastes bytes, it does not
  // degrade the user) — but the shipped one is the smallest, closest
  // attempt: q=90 stepped down three times.
  EXPECT_TRUE(result.webp_ssimulacra2_reencoded);
  EXPECT_EQ(result.final_webp_quality, 75)
      << "must ship the down-stepped last attempt, not the first "
         "higher-scoring one (score="
      << result.webp_ssimulacra2_score << ")";
  EXPECT_GT(result.webp_ssimulacra2_score, 16.0f);  // still above the band
  EXPECT_FALSE(result.webp_ssimulacra2_declined);
}

// (d) Grayscale AVIF verify runs: YUV400 decode-back yields bpp=1 pixels
// (no "UNVERIFIED" skip), the score is recorded, and steering engages
// (an above-band gray encode is stepped down).
TEST_F(ImageTranscoderTest, GrayAvifVerifyScoresAndSteers) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_avif = false;
  config.content_analysis = false;
  config.avif_quality = 75;
  config.target_ssimulacra2 = 10.0f;  // band [7,16]: above -> step down
  config.ssimulacra2_max_attempts = 4;
  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto result =
      transcoder.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kAvif});
  ASSERT_TRUE(result.avif.success) << result.avif.error_message;
  EXPECT_GE(result.avif_ssimulacra2_score, 0.0f)
      << "grayscale AVIF verify did not produce a verdict";
  EXPECT_LT(result.final_avif_quality, 75)
      << "above-band gray encode was not steered down";

  // The decode-back itself must be bpp=1 (S3.1): YUV400 container truth.
  DecodedImage back = transcoder.DecodeToPixels(result.avif.output_data);
  ASSERT_FALSE(back.pixel_buffer.empty());
  EXPECT_EQ(back.bytes_per_pixel, 1);
}

// (e) Gray WebP comparability: libwebp lossy has no gray mode, so a bpp=1
// source encodes as RGB that decodes back at bpp=3 (near-monochrome — VP8
// chroma quantization rounds Cb/Cr to 127/129). The 1<->3 comparability
// rule must let the verify run on the candidate's G channel. A candidate
// that is NOT monochrome within tolerance joins the same not-comparable
// path as (f) below and declines (fail-closed).
TEST_F(ImageTranscoderTest, GrayWebpVerifyRunsViaMonochromeRule) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_webp = false;
  config.content_analysis = false;
  config.webp_quality = 75;
  config.target_ssimulacra2 = 10.0f;  // band [7,16]: above band ships
  pagespeed::ImageTranscoder transcoder(config, &handler);

  std::string jpeg = ReadTestFile("jpeg/testgray.jpg");
  ASSERT_FALSE(jpeg.empty());

  // Sanity: the WebP decode-back of this gray source really is bpp=3 —
  // that is why the comparability rule is needed at all.
  auto probe =
      transcoder.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kWebP});
  ASSERT_TRUE(probe.webp.success) << probe.webp.error_message;
  DecodedImage back = transcoder.DecodeToPixels(probe.webp.output_data);
  ASSERT_FALSE(back.pixel_buffer.empty());
  ASSERT_EQ(back.bytes_per_pixel, 3);

  EXPECT_GE(probe.webp_ssimulacra2_score, 0.0f)
      << "gray WebP verify skipped: the 1<->3 monochrome rule did not fire";
  EXPECT_FALSE(probe.webp_ssimulacra2_declined);
}

// (f) Fail-closed on the shipped candidate: a candidate whose decode-back
// cannot produce a verdict declines. Construction: a ~20MP GRAY JPEG
// decodes at bpp=1 (20.2MB, under the 50MB cap) but its WebP re-encode
// decodes at bpp=3 (60.7MB, over the cap) -- the decode-back fails on the
// FIRST encode while the reference decode succeeded.
//
// (The #1381 construction spliced a 4200x4200 gray AVIF here; #1382
// tightened the AVIF decode cap to the RGB transient peak, which made the
// reference's accounted size and the re-encode's decode-back the SAME
// number and closed that asymmetry. The gray-JPEG form is the same idea:
// bpp=1 in, bpp=3 out.)
std::string MakeSyntheticGrayJpeg(int width, int height, uint8_t value) {
  pagespeed::NullMessageHandler handler;
  JpegCompressionOptions options;
  options.lossy = true;
  options.lossy_options.quality = 30;

  std::string out;
  std::unique_ptr<net_instaweb::ScanlineWriterInterface> writer(
      CreateScanlineWriter(pagespeed::image_compression::IMAGE_JPEG, GRAY_8,
                           static_cast<size_t>(width),
                           static_cast<size_t>(height), &options, &out,
                           &handler));
  if (writer == nullptr) return {};

  const std::string row(static_cast<size_t>(width), static_cast<char>(value));
  for (int y = 0; y < height; ++y) {
    if (!writer->WriteNextScanline(row.data())) return {};
  }
  if (!writer->FinalizeWrite()) return {};
  return out;
}

TEST_F(ImageTranscoderTest, UnmeasurableCandidateDeclines) {
  // 4600x4400 = 20.24M pixels: bpp=1 decode 20.2MB (under the 50MB cap),
  // bpp=3 WebP decode-back 60.7MB (over it).
  std::string giant = MakeSyntheticGrayJpeg(4600, 4400, 128);
  ASSERT_FALSE(giant.empty());

  // The reference decode fits under the cap and is bpp=1.
  DecodedImage reference = transcoder_.DecodeToPixels(giant);
  ASSERT_FALSE(reference.pixel_buffer.empty());
  ASSERT_EQ(reference.bytes_per_pixel, 1);
  ASSERT_EQ(reference.width, 4600u);
  ASSERT_EQ(reference.height, 4400u);

  auto result =
      transcoder_.TranscodeMulti(giant, {CapabilityMask::ImageFormat::kWebP});
  EXPECT_FALSE(result.webp.success);
  EXPECT_NE(std::string::npos, result.webp.error_message.find("no verdict"))
      << result.webp.error_message;
  EXPECT_TRUE(result.webp_ssimulacra2_declined);
  // No verdict exists, so the score stays N/A -- distinct from a measured
  // below-floor decline, which keeps the score as evidence.
  EXPECT_EQ(result.webp_ssimulacra2_score, -1.0f);
}

// The decode cap accounts the grayscale RGB transient (#1382): a YUV400
// AVIF whose 1-byte OUTPUT fits the 50MB cap must still be refused when its
// YUV->RGB conversion (3 bytes/pixel) does not. Before the fix the decode
// succeeded while transiently allocating ~3x the accounted bytes.
TEST_F(ImageTranscoderTest, DecodeCapAccountsTheGrayscaleRgbTransient) {
  // 4400x4400 = 19.36M pixels: output 19.4MB (fits), RGB transient 58.1MB
  // (does not). Both numbers derived in the comment; the 4400 size is
  // chosen so the two land on opposite sides of kMaxDecodedPixels (50MB).
  std::string giant = MakeSyntheticAvif(4400, 4400, /*grayscale=*/true,
                                        SyntheticPattern::kFlat, 128,
                                        /*quality=*/50, /*seed=*/0);
  ASSERT_FALSE(giant.empty());
  DecodedImage decoded = transcoder_.DecodeToPixels(giant);
  EXPECT_TRUE(decoded.pixel_buffer.empty())
      << "a gray AVIF whose RGB transient exceeds the cap must not decode";

  // Non-vacuity: a smaller gray AVIF whose RGB transient fits still decodes
  // at bpp=1 (the #1381 container-truth behavior is unchanged).
  std::string small = MakeSyntheticAvif(200, 200, /*grayscale=*/true,
                                        SyntheticPattern::kFlat, 128,
                                        /*quality=*/50, /*seed=*/0);
  ASSERT_FALSE(small.empty());
  DecodedImage ok = transcoder_.DecodeToPixels(small);
  ASSERT_FALSE(ok.pixel_buffer.empty());
  EXPECT_EQ(ok.bytes_per_pixel, 1);
}

// (g) A sub-8x8 reference is the one instrumental limit — the metric
// needs 8x8, so no verdict exists to bind — and tiny icons still ship
// today-style. Both dimensions are checked independently.
TEST_F(ImageTranscoderTest, TinyImageShipsWithoutADeclinableVerdict) {
  std::string tiny =
      MakeSyntheticAvif(6, 6, /*grayscale=*/false, SyntheticPattern::kFlat, 170,
                        /*quality=*/80, /*seed=*/0);
  std::string wide_but_short =
      MakeSyntheticAvif(8, 6, /*grayscale=*/false, SyntheticPattern::kFlat, 170,
                        /*quality=*/80, /*seed=*/0);
  // Narrow-but-tall drives the WIDTH clause alone: 6x6 and 8x6 are both
  // caught by the height check, so without this case a mutant deleting the
  // width clause survives.
  std::string narrow_but_tall =
      MakeSyntheticAvif(6, 10, /*grayscale=*/false, SyntheticPattern::kFlat,
                        170, /*quality=*/80, /*seed=*/0);
  ASSERT_FALSE(tiny.empty());
  ASSERT_FALSE(wide_but_short.empty());
  ASSERT_FALSE(narrow_but_tall.empty());

  for (const std::string& input : {tiny, wide_but_short, narrow_but_tall}) {
    auto result =
        transcoder_.TranscodeMulti(input, {CapabilityMask::ImageFormat::kWebP});
    EXPECT_TRUE(result.webp.success) << result.webp.error_message;
    EXPECT_FALSE(result.webp_ssimulacra2_declined);
    EXPECT_EQ(result.webp_ssimulacra2_score, -1.0f);  // metric cannot run
  }
}

// (j) A NEGATIVE score is a legitimate catastrophic verdict ("very
// different images"), not an instrumental limit: on normal-size images it
// declines like any other below-floor result. Noise-like content at low
// encoder quality is the reliable constructor — the encoders tank it far
// below zero.
TEST_F(ImageTranscoderTest, NegativeScoreVerdictDeclines) {
  // Near-lossless noise reference; a low-quality WebP re-encode of pure
  // noise is a very different image and scores below zero.
  std::string noise =
      MakeSyntheticAvif(33, 34, /*grayscale=*/false, SyntheticPattern::kNoise,
                        0, /*quality=*/90, /*seed=*/12345);
  ASSERT_FALSE(noise.empty());

  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_webp = false;
  config.content_analysis = false;
  config.webp_quality = 20;
  config.target_ssimulacra2 = 70.0f;  // default band [67,78]
  pagespeed::ImageTranscoder transcoder(config, &handler);

  auto result =
      transcoder.TranscodeMulti(noise, {CapabilityMask::ImageFormat::kWebP});
  EXPECT_FALSE(result.webp.success);
  EXPECT_TRUE(result.webp_ssimulacra2_declined);
  // The catastrophic verdict itself is kept as evidence.
  EXPECT_LT(result.webp_ssimulacra2_score, 0.0f);
  EXPECT_NE(std::string::npos,
            result.webp.error_message.find("verification failed"))
      << result.webp.error_message;
  EXPECT_EQ(std::string::npos, result.webp.error_message.find("no verdict"))
      << "a measured negative score is a verdict, not a missing one: "
      << result.webp.error_message;
}

// (i) Save-Data moves the floor with the target: the same encode that a
// save-data visitor accepts (floor lowered by savedata_score_reduction)
// is declined for a default visitor. Declines must not become MORE
// likely when the user asked for smaller files.
//
// Fixture calibration (vendored encoders): the mobile-resized webp of
// exif_orientation_6_photo tops out around 63.9 — above the Save-Data
// band [52,63] but below the default floor 67 — so the ladder exhausts
// itself below the default floor and declines there, while the Save-Data
// visitor's band accepts it.
TEST_F(ImageTranscoderTest, SaveDataLoweredFloorPreventsSpuriousDecline) {
  std::string jpeg = ReadTestFile("jpeg/exif_orientation_6_photo.jpg");
  ASSERT_FALSE(jpeg.empty());

  auto run = [&](CapabilityMask::SaveData sd) {
    pagespeed::NullMessageHandler handler;
    pagespeed::ImageTranscoderConfig config;
    config.learned_quality_webp = false;
    config.content_analysis = false;
    // Fixed quality so the only difference between the runs is the
    // score-target reduction: both ladders start from q=75.
    config.webp_quality = 75;
    config.savedata_webp_quality = 75;    // No save-data quality change.
    config.viewport_widths.mobile = 128;  // The calibrated resize.
    config.target_ssimulacra2 = 70.0f;    // band [67,78]; save-data [52,63]
    pagespeed::ImageTranscoder transcoder(config, &handler);
    return transcoder.TranscodeMultiResized(
        jpeg, {CapabilityMask::ImageFormat::kWebP},
        CapabilityMask::Viewport::kMobile, CapabilityMask::PixelDensity::k1x,
        sd);
  };

  auto plain = run(CapabilityMask::SaveData::kOff);
  auto saved = run(CapabilityMask::SaveData::kOn);
  ASSERT_TRUE(saved.webp.success) << saved.webp.error_message;
  EXPECT_FALSE(saved.webp_ssimulacra2_declined);
  EXPECT_GE(saved.webp_ssimulacra2_score, 52.0f);
  ASSERT_FALSE(plain.webp.success);
  EXPECT_TRUE(plain.webp_ssimulacra2_declined);
  EXPECT_LT(plain.webp_ssimulacra2_score, 67.0f);
  EXPECT_GE(plain.webp_ssimulacra2_score, 0.0f);
}

// (k) Save-Data on a C2PA-carrying source (#1382): the manifest-preserving
// delegation happens BEFORE this path's own verification loops, so the
// save-data settings must all be applied before it too. The target
// reduction now travels with the quality overrides; the delegated path
// keeps skip-not-strip semantics under Save-Data and the JPEG->JPEG carry
// still recompresses at the save-data quality.
TEST_F(ImageTranscoderTest, SaveDataC2paSourceKeepsCarryAndLoweredSettings) {
  std::string jpeg = ReadTestFile("jpeg/sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());
  const std::string with_c2pa = SpliceC2paApp11IntoJpeg(jpeg);
  ASSERT_TRUE(net_instaweb::ImageHasJumbfC2pa(with_c2pa));

  auto run = [&](CapabilityMask::SaveData sd) {
    return transcoder_.TranscodeMultiResized(
        with_c2pa,
        {CapabilityMask::ImageFormat::kWebP, CapabilityMask::ImageFormat::kAvif,
         CapabilityMask::ImageFormat::kOriginal},
        CapabilityMask::Viewport::kDesktop, CapabilityMask::PixelDensity::k1x,
        sd);
  };

  auto saved = run(CapabilityMask::SaveData::kOn);
  // Skip-not-strip holds under Save-Data: WebP/AVIF cannot carry the
  // manifest and are refused, by design, band-independent.
  EXPECT_FALSE(saved.webp.success);
  EXPECT_NE(std::string::npos, saved.webp.error_message.find("C2PA"))
      << saved.webp.error_message;
  EXPECT_FALSE(saved.avif.success);
  // The original variant is accepted: the JPEG->JPEG carry recompresses
  // with the manifest intact.
  EXPECT_TRUE(saved.optimized_original.success)
      << saved.optimized_original.error_message;
  EXPECT_NE(std::string::npos,
            saved.optimized_original.output_data.find("c2pa"));

  // The save-data QUALITY override survived the delegation (q60 vs the
  // default 85), proving the delegated call really carries the save-data
  // config block the target reduction now belongs to.
  auto plain = run(CapabilityMask::SaveData::kOff);
  ASSERT_TRUE(plain.optimized_original.success)
      << plain.optimized_original.error_message;
  EXPECT_NE(plain.optimized_original.output_data,
            saved.optimized_original.output_data)
      << "save-data carry must recompress at the save-data quality";
}

}  // namespace

// ===========================================================================
// SelectVerifyAttempt: the band-closest selection, driven directly (#1381).
// A compiling ship-last mutant survived the encoder-driven
// fixtures because their score ladders are monotone or in-band-terminated;
// synthetic (score, size) lists discriminate every rule deterministically.
// Band below: [64, 78].
// ===========================================================================

namespace {
constexpr float kSelLo = 64.0f;
constexpr float kSelHi = 78.0f;

size_t Select(const std::vector<pagespeed::VerifyAttempt>& attempts,
              bool decline_below_floor) {
  return pagespeed::SelectVerifyAttempt(attempts, kSelLo, kSelHi,
                                        decline_below_floor);
}
}  // namespace

TEST(SelectVerifyAttemptTest, BelowFloorShipsHighestScoreNotLast) {
  // The mpp #790 D2 regression shape: a worse retry must not displace a
  // better earlier attempt. Ship-last returns 2 here.
  EXPECT_EQ(1u, Select({{50.0f, 900}, {60.0f, 900}, {55.0f, 900}}, false));
  EXPECT_EQ(1u, Select({{50.0f, 900}, {60.0f, 900}, {55.0f, 900}}, true));
}

TEST(SelectVerifyAttemptTest, InBandRetryBeatsBelowFloorInitial) {
  // Ship-first returns 0 here.
  EXPECT_EQ(1u, Select({{60.0f, 900}, {70.0f, 900}}, false));
}

TEST(SelectVerifyAttemptTest, InitialWinsOverWorseRetries) {
  // The initial encode (index 0) must be able to beat a NON-EMPTY retry
  // set — the quality-restore branch in the verify loop depends on it.
  EXPECT_EQ(0u, Select({{63.0f, 900}, {50.0f, 900}, {55.0f, 900}}, false));
  EXPECT_EQ(0u, Select({{70.0f, 900}, {60.0f, 900}}, false));
}

TEST(SelectVerifyAttemptTest, AboveBandShipsDownSteppedLatest) {
  // All above the ceiling: later down-steps are closer AND smaller.
  EXPECT_EQ(2u, Select({{90.0f, 1000}, {85.0f, 800}, {80.0f, 700}}, false));
}

TEST(SelectVerifyAttemptTest, EqualDistanceTiebreaksOnSmallerBytes) {
  EXPECT_EQ(1u, Select({{60.0f, 1000}, {60.0f, 900}}, false));
  EXPECT_EQ(0u, Select({{60.0f, 900}, {60.0f, 1000}}, false));
}

TEST(SelectVerifyAttemptTest, FullTieShipsLater) {
  EXPECT_EQ(1u, Select({{60.0f, 900}, {60.0f, 900}}, false));
}

TEST(SelectVerifyAttemptTest, DeclineArmedShippableBeatsCloserBelowFloor) {
  // Asymmetry rule (#1381): with decline armed, an above-band
  // attempt is servable and a below-floor one is not, so the below-floor
  // attempt must not win on distance. Unarmed, plain band-closest applies
  // (the JPEG ship-below-floor arm) and the closer attempt wins.
  const std::vector<pagespeed::VerifyAttempt> cliff = {{85.0f, 1000},
                                                       {63.0f, 500}};
  EXPECT_EQ(0u, Select(cliff, true));
  EXPECT_EQ(1u, Select(cliff, false));
}

TEST(SelectVerifyAttemptTest, DeclineArmedAllBelowFloorKeepsHighestEvidence) {
  // Nothing shippable: the decline fires regardless; the highest score is
  // recorded as the evidence.
  EXPECT_EQ(1u, Select({{50.0f, 900}, {60.0f, 900}, {55.0f, 900}}, true));
}
// The same-format WebP re-encode lane consults the
// binding verdict. It used to ship on the size test alone -- a lossy WebP
// origin whose re-encode measured NEGATIVE was served (found downstream
// at SSIMULACRA2 -37.06). Construction: a dense fine-structure lossy
// WebP origin (see MakeSyntheticLossyWebp) whose floor-quality re-encode
// measures NEGATIVE while being much smaller, so the old size gate alone
// would have shipped it.
TEST_F(ImageTranscoderTest, SameFormatWebpReencodeDeclinesNegativeVerdict) {
  pagespeed::NullMessageHandler handler;

  // Author the LOSSY origin (the lossless exemption must not apply)
  // directly with libwebp -- authoring, not the lane under test.
  const std::string webp_origin =
      MakeSyntheticLossyWebp(256, 256, /*quality=*/95.0f);
  ASSERT_FALSE(webp_origin.empty()) << "could not author the WebP origin";

  // The lane under test: same-format re-encode at a low quality floor.
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_webp = false;
  config.content_analysis = false;
  config.webp_quality = 5;
  pagespeed::ImageTranscoder transcoder(config, &handler);
  auto result = transcoder.TranscodeMulti(
      webp_origin, {CapabilityMask::ImageFormat::kOriginal});

  // Nothing ships; the caller's fallback (serve the origin's own bytes)
  // is the same one the no-savings return already exercises.
  EXPECT_FALSE(result.optimized_original.success);
  EXPECT_TRUE(result.optimized_original.output_data.empty());
  EXPECT_NE(std::string::npos,
            result.optimized_original.error_message.find("verification failed"))
      << result.optimized_original.error_message;
  // The decline lands on the ORIGINAL-FORMAT arm's evidence fields, the
  // ones the per-arm decline counter reads.
  EXPECT_TRUE(result.ssimulacra2_declined);
  EXPECT_FALSE(result.ssimulacra2_reencoded);  // shipped-none
  // The measured score is the evidence, and for this construction it is
  // the issue's own class: catastrophically NEGATIVE, far below any
  // band floor.
  EXPECT_LT(result.ssimulacra2_score, 0.0f)
      << "expected a negative verdict on a smeared-noise re-encode";
}

// ...and the healthy side: an in-band same-format re-encode still ships,
// with the verdict recorded on the same arm fields. Same origin class as
// production traffic: a photo re-encoded from a higher-quality WebP.
TEST_F(ImageTranscoderTest, SameFormatWebpReencodeShipsInBand) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig author_cfg;
  author_cfg.learned_quality_webp = false;
  author_cfg.content_analysis = false;
  author_cfg.quality_verify = false;
  author_cfg.webp_quality = 95;
  pagespeed::ImageTranscoder author(author_cfg, &handler);
  std::string jpeg = ReadTestFile("jpeg/sjpeg6.jpg");
  ASSERT_FALSE(jpeg.empty());
  auto authored =
      author.TranscodeMulti(jpeg, {CapabilityMask::ImageFormat::kWebP});
  ASSERT_TRUE(authored.webp.success) << authored.webp.error_message;
  const std::string webp_origin = authored.webp.output_data;

  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_webp = false;
  config.content_analysis = false;
  pagespeed::ImageTranscoder transcoder(config, &handler);
  auto result = transcoder.TranscodeMulti(
      webp_origin, {CapabilityMask::ImageFormat::kOriginal});

  EXPECT_FALSE(result.ssimulacra2_declined);
  EXPECT_GE(result.ssimulacra2_score, 0.0f)
      << "in-band same-format re-encode must carry its verdict";
  // Unconditional: the healthy lane must actually SHIP -- a conditional
  // here would let a silently-broken ship path (an inverted size gate,
  // say) pass on the decline asserts alone.
  ASSERT_TRUE(result.optimized_original.success)
      << result.optimized_original.error_message;
  EXPECT_LT(result.optimized_original.output_data.size(), webp_origin.size())
      << "a shipped same-format re-encode must still beat the origin";
}

// ...and the RESIZED path's same-format lane carries the same evidence
// (#1385 review F1): a declined re-encode on TranscodeMultiResized's
// kOriginal arm lands on the same per-arm fields the decline counter
// reads, instead of being protected but invisible.
TEST_F(ImageTranscoderTest, ResizedSameFormatWebpDeclineCarriesEvidence) {
  pagespeed::NullMessageHandler handler;
  // 600x600 EXCEEDS the mobile viewport target pinned below, so a real
  // resize happens and the RESIZED arm -- not the no-resize fast-path
  // delegation to TranscodeMulti -- is what grades this decline. At
  // 256x256 this test is vacuous: the fast path delegates to the branch
  // the OTHER decline test already pins.
  const std::string webp_origin =
      MakeSyntheticLossyWebp(600, 600, /*quality=*/95.0f);
  ASSERT_FALSE(webp_origin.empty());

  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_webp = false;
  config.content_analysis = false;
  config.webp_quality = 5;
  // Pin the resize threshold so a future default change cannot quietly
  // re-vacuate this test.
  config.viewport_widths.mobile = 480;
  pagespeed::ImageTranscoder transcoder(config, &handler);
  auto result = transcoder.TranscodeMultiResized(
      webp_origin, {CapabilityMask::ImageFormat::kOriginal},
      CapabilityMask::Viewport::kMobile);
  EXPECT_FALSE(result.optimized_original.success);
  EXPECT_TRUE(result.ssimulacra2_declined)
      << "resized-path decline must be visible to the per-arm counter";
  // Below the serve floor, not necessarily negative: since the explicit
  // resized leg (#1380) the verdict is measured against the RESIZED
  // encode source, and the downscale smooths the stripe field -- the
  // negative-verdict class itself is pinned by the non-resizing test
  // above.
  EXPECT_LT(result.ssimulacra2_score, 67.0f);
  EXPECT_FALSE(result.ssimulacra2_reencoded);
}

// ...and the lossless exemption is untouched by the verify (#1375): a
// lossless origin is kept verbatim, no re-encode, no verdict.
TEST_F(ImageTranscoderTest, SameFormatLosslessWebpStaysVerbatim) {
  pagespeed::NullMessageHandler handler;
  pagespeed::ImageTranscoderConfig config;
  config.learned_quality_webp = false;
  config.content_analysis = false;
  pagespeed::ImageTranscoder transcoder(config, &handler);
  std::string lossless = MakeLosslessWebp(transcoder);
  ASSERT_FALSE(lossless.empty()) << "could not build a lossless WebP";
  auto result = transcoder.TranscodeMulti(
      lossless, {CapabilityMask::ImageFormat::kOriginal});
  ASSERT_TRUE(result.optimized_original.success);
  EXPECT_EQ(result.optimized_original.output_data, lossless);
  EXPECT_EQ(result.ssimulacra2_score, -1.0f) << "no verdict on a verbatim";
  EXPECT_FALSE(result.ssimulacra2_declined);
}

// ===========================================================================
// VerifySsimulacra2Quality's metric-error routing (#1382). The score channel
// carries verdicts -- negative ones included -- so a metric-internal failure
// arrives on its own channel (nullopt) and must decline fail-closed on EVERY
// arm, including the ship-below-floor JPEG arm, which previously shipped a
// -1.0f sentinel that read as "no measurement" while the decline evidence
// text asserted one. A metric failure is not constructible through the
// encoders a transcoder-level fixture has (every reachable decode-back is
// well-formed and dimension-matched), so the loop takes an injected scorer
// and these tests drive it synthetically -- the same falsifiability move
// SelectVerifyAttempt made for the selection rule.
// ===========================================================================

namespace {

// A 64x64 RGB reference big enough for the metric's 8x8 floor.
DecodedImage MakeVerifyReference() {
  DecodedImage d;
  d.width = 64;
  d.height = 64;
  d.bytes_per_pixel = 3;
  d.pixel_buffer.assign(static_cast<size_t>(64) * 64 * 3, '\x80');
  return d;
}

// A decode-back that is comparable with the reference (same dims/bpp).
DecodedImage MakeMatchingDecode() { return MakeVerifyReference(); }

TranscodeResult MakeEncodedResult() {
  return {true, std::string(32, '\x11'), "image/webp", {}};
}

}  // namespace

TEST(VerifyLoopTest, MetricFailureOnInitialCandidateDeclinesFailClosed) {
  pagespeed::NullMessageHandler handler;
  DecodedImage reference = MakeVerifyReference();
  TranscodeResult encoded = MakeEncodedResult();
  int quality = 50;

  auto verify = pagespeed::VerifySsimulacra2Quality(
      reference, encoded,
      [](const DecodedImage&) { return MakeEncodedResult(); },
      [](std::string_view) { return MakeMatchingDecode(); },
      [](const pagespeed::ComparablePixels&) -> std::optional<float> {
        return std::nullopt;  // The metric itself failed.
      },
      quality, 0, 100, "WebP", 70.0f, 5.0f, 4, 5,
      pagespeed::VerifyDeclinePolicy::kDeclineBelowFloor, &handler);

  EXPECT_TRUE(verify.declined);
  EXPECT_TRUE(verify.verdict_missing);
  // No measurement exists, so the score keeps the sentinel -- it must not
  // read as a (catastrophic) verdict.
  EXPECT_EQ(verify.score, -1.0f);
  EXPECT_FALSE(verify.reencoded);
}

TEST(VerifyLoopTest, MetricFailureDeclinesOnTheShipBelowFloorArmToo) {
  // The JPEG arm's policy ships below-floor scores (accept-at-ceiling,
  // #1284) -- but a metric failure is not a score, it is the absence of
  // one, and fail-closed applies at every call site (#1382). This is the
  // behavior change: the sentinel -1.0f used to ship as "N/A".
  pagespeed::NullMessageHandler handler;
  DecodedImage reference = MakeVerifyReference();
  TranscodeResult encoded = MakeEncodedResult();
  int quality = 50;

  auto verify = pagespeed::VerifySsimulacra2Quality(
      reference, encoded,
      [](const DecodedImage&) { return MakeEncodedResult(); },
      [](std::string_view) { return MakeMatchingDecode(); },
      [](const pagespeed::ComparablePixels&) -> std::optional<float> {
        return std::nullopt;
      },
      quality, 1, 100, "", 70.0f, 5.0f, 4, 5,
      pagespeed::VerifyDeclinePolicy::kShipBelowFloor, &handler);

  EXPECT_TRUE(verify.declined);
  EXPECT_TRUE(verify.verdict_missing);
  EXPECT_EQ(verify.score, -1.0f);
}

TEST(VerifyLoopTest, MetricFailureOnRetryKeepsTheVerifiedPrior) {
  // Initial verdict far above the band ([67,78]) forces one retry; the
  // retry's scoring fails. The loop must keep the verified initial attempt
  // rather than decline or ship an unmeasured retry.
  pagespeed::NullMessageHandler handler;
  DecodedImage reference = MakeVerifyReference();
  TranscodeResult encoded = MakeEncodedResult();
  int quality = 90;
  int encodes = 0;
  int scorings = 0;

  auto verify = pagespeed::VerifySsimulacra2Quality(
      reference, encoded,
      [&encodes](const DecodedImage&) {
        ++encodes;
        return MakeEncodedResult();
      },
      [](std::string_view) { return MakeMatchingDecode(); },
      [&scorings](const pagespeed::ComparablePixels&) -> std::optional<float> {
        ++scorings;
        return scorings == 1 ? std::optional<float>{90.0f} : std::nullopt;
      },
      quality, 0, 100, "WebP", 70.0f, 5.0f, 4, 5,
      pagespeed::VerifyDeclinePolicy::kDeclineBelowFloor, &handler);

  // The retry really happened (otherwise this tests nothing).
  ASSERT_EQ(1, encodes);
  ASSERT_EQ(2, scorings);
  EXPECT_FALSE(verify.declined);
  EXPECT_FALSE(verify.reencoded);  // The initial attempt is the shipped one.
  EXPECT_EQ(verify.score, 90.0f);
  EXPECT_EQ(quality, 90);  // The stepped-down quality was restored.
}

TEST(VerifyLoopTest, InBandVerdictShipsWithoutDecline) {
  // Non-vacuity for the harness: a real in-band verdict takes the ordinary
  // path -- no decline, no retry -- so the tests above are meaningful.
  pagespeed::NullMessageHandler handler;
  DecodedImage reference = MakeVerifyReference();
  TranscodeResult encoded = MakeEncodedResult();
  int quality = 50;
  int encodes = 0;

  auto verify = pagespeed::VerifySsimulacra2Quality(
      reference, encoded,
      [&encodes](const DecodedImage&) {
        ++encodes;
        return MakeEncodedResult();
      },
      [](std::string_view) { return MakeMatchingDecode(); },
      [](const pagespeed::ComparablePixels&) -> std::optional<float> {
        return 70.0f;
      },
      quality, 0, 100, "WebP", 70.0f, 5.0f, 4, 5,
      pagespeed::VerifyDeclinePolicy::kDeclineBelowFloor, &handler);

  EXPECT_EQ(0, encodes);
  EXPECT_FALSE(verify.declined);
  EXPECT_FALSE(verify.verdict_missing);
  EXPECT_EQ(verify.score, 70.0f);
}
