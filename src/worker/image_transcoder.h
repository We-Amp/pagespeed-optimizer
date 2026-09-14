// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Image Transcoder for Worker
//
// Transcodes images between formats based on client capability mask.
// Bridges the worker (pagespeed namespace) and image library
// (pagespeed::image_compression namespace).

#ifndef PAGESPEED_SRC_WORKER_IMAGE_TRANSCODER_H_
#define PAGESPEED_SRC_WORKER_IMAGE_TRANSCODER_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/classify/capability_mask.h"
#include "lib/image/content_analyzer.h"

namespace pagespeed {

class MessageHandler;

// Target widths for viewport-based image resizing.
// Desktop uses 0 (no resize) by default since it receives full-size images.
struct ViewportWidths {
  uint32_t mobile = 480;
  uint32_t tablet = 768;
  uint32_t desktop = 0;  // 0 = no resize
};

// Configuration for image transcoding.
struct ImageTranscoderConfig {
  // JPEG output quality (1-100). 85 is a good default.
  int jpeg_quality = 85;

  // WebP output quality (0-100). 75 is a good default for lossy.
  int webp_quality = 75;

  // WebP alpha quality (0-100). 100 for lossless alpha.
  int webp_alpha_quality = 100;

  // AVIF quality (0-100, higher = better). 60 is a good default.
  int avif_quality = 60;

  // AVIF encoder speed (0-10, higher = faster). 6 is a good default.
  int avif_speed = 6;

  // Whether to produce progressive JPEGs.
  bool progressive_jpeg = true;

  // Whether to use lossy JPEG compression.
  bool lossy_jpeg = true;

  // Target widths per viewport class.  0 = no resize.
  ViewportWidths viewport_widths;

  // Save-Data quality overrides (lower quality for bandwidth savings).
  int savedata_jpeg_quality = 60;
  int savedata_webp_quality = 50;
  int savedata_avif_quality = 45;

  // Content-aware quality presets: analyze decoded pixels to adjust
  // quality per content class (photo, screenshot, illustration, noisy).
  bool content_analysis = true;

  // Noise-adaptive denoising: apply bilateral filter to images
  // classified as kNoisy before encoding.
  // noise_level (from content analyzer) must exceed this threshold.
  // 0.0 = disabled, 0.3 = conservative default.
  float denoise_threshold = 0.3f;

  // Bilateral filter spatial sigma (kernel size). Clamped to [0.5, 10].
  float denoise_sigma_spatial = 3.0f;

  // Bilateral filter range sigma (edge sensitivity).
  float denoise_sigma_range = 25.0f;

  // SSIMULACRA2 perceptual quality verification for all image encodings.
  // When enabled, checks encoded output quality and re-encodes if outside
  // the asymmetric tolerance band [target - 0.6*tol, target + 1.6*tol].
  // Under-quality is user-visible; over-quality just wastes bytes.
  bool quality_verify = true;
  float target_ssimulacra2 = 70.0f;    // Target score (0-100)
  float ssimulacra2_tolerance = 5.0f;  // Base tolerance (asymmetric: -3/+8)
  // 1 = verify the first encode only, with the verdict binding: a
  // below-floor score declines the variant with no rescue re-encode
  // attempt (#1381). 2+ = re-encode attempts before the verdict.
  int ssimulacra2_max_attempts = 4;
  int ssimulacra2_quality_step = 5;  // Quality adjustment per attempt

  // Learned quality prediction: use per-format ML models to predict
  // the encoder quality parameter for the target SSIMULACRA2 score.
  // Falls back to base*factor when the model returns an invalid prediction.
  bool learned_quality = true;
  bool learned_quality_jpeg = true;
  bool learned_quality_webp = true;
  bool learned_quality_avif = true;
  float savedata_score_reduction =
      15.0f;  // Reduce target by this for Save-Data

  // Quality cap: a same-format (JPEG->JPEG) re-encode never exceeds the
  // detected source quality.  Encoding above the source spends bytes on
  // artifacts already baked into the source and yields no visual gain.
  // A floor of 30 prevents over-aggressive capping when the DQT-based
  // estimate UNDERSTATES the source (Guetzli, Photoshop custom tables).
  //
  // |quality_cap_margin| is retained for configuration compatibility but no
  // longer raises the cap above the source quality (#1284); the floor and
  // the margin are separate concerns and only the floor survives.
  int quality_cap_margin = 10;  // deprecated: no longer widens the cap
  bool no_quality_cap = false;  // --no-quality-cap

  // Preserve C2PA / Content Credentials provenance, on by default.
  // JPEG->JPEG (kOriginal) recompress carries the APP11/JUMBF manifest verbatim.
  // For every path that cannot carry it -- AVIF/WebP transcode, viewport resize, PNG,
  // and the XMP/APP1 form -- the transcoder serves the ORIGINAL bytes unchanged
  // (skip-not-strip) rather than silently dropping provenance; it never strips. The
  // detector only scans for marker signatures, never decoding/validating/re-emitting
  // a manifest (D3). Byte-exact carry THROUGH transcode (e.g. C2PA in AVIF/WebP
  // boxes) is Track B, demand-gated and deferred.
  bool preserve_c2pa = true;  // --no-preserve-c2pa

  // Level A (carry-through), PNG-only, OFF by default (--c2pa-carry).
  // Strict enhancement of preserve_c2pa: when a manifest-bearing PNG would stay in
  // PNG form (kOriginal), recompress it and re-splice its original caBX/iTXt chunks
  // before IEND, so the PNG is BOTH optimized AND keeps its manifest, instead of the
  // default skip-not-strip (serve original, no optimization). PNG-only because the
  // JPEG->JPEG codec already carries APP11/JUMBF; AVIF/WebP/resize still cannot carry
  // and remain skip-not-strip even with this on. Fail-safe to skip-not-strip on any
  // anomaly; never decodes/validates/re-emits the manifest (D3). Requires
  // preserve_c2pa (the carry runs inside the preserve gate). Demand-gated.
  bool c2pa_carry = false;  // --c2pa-carry
};

// JPEG quality inputs for a multi-format transcode.
//
// These two values are both "a JPEG quality number" but mean opposite
// things, and passing one where the other belongs is silent: the cap goes
// inert, or the encoder is pinned to an unrelated quality.  They are grouped
// in a named struct so every call site has to name the field it is filling
// and a short-by-one argument list fails to compile instead of misbinding
// (#1284).
struct JpegQualityInputs {
  // Encoder quality already discovered for THIS image by an earlier call in
  // the same variant loop (-1 = none).  Reuses that result instead of
  // repeating the quality search; it is a target, not a source measurement.
  int carried_hint = -1;

  // Source JPEG quality estimated from the input's DQT tables
  // (-1 = unknown or non-JPEG).  Ceiling for a same-format re-encode.
  int source_quality = -1;
};

// Result of an image transcoding operation.
struct TranscodeResult {
  bool success = false;
  std::string output_data;
  std::string output_mime_type;
  std::string error_message;
};

// Verify outcome of a same-format WebP re-encode (#1385), surfaced by
// OptimizeWebp for callers that record per-arm verification evidence.
// Plain data on purpose: the full verify result type is file-local to
// the implementation.
struct SameFormatWebpVerify {
  float score = -1.0f;  // SSIMULACRA2 verdict (-1 = not measured)
  bool reencoded = false;
  bool declined = false;
};

// Decoded pixel buffer from a source image.
struct DecodedImage {
  std::string pixel_buffer;  // Raw RGB/RGBA/Gray pixels
  uint32_t width = 0;
  uint32_t height = 0;
  int bytes_per_pixel = 3;  // 3=RGB, 4=RGBA, 1=Gray
  bool has_alpha = false;
};

// Result of multi-format transcoding from a single decode pass.
struct MultiTranscodeResult {
  TranscodeResult webp;
  TranscodeResult avif;
  TranscodeResult optimized_original;
  // How many converted-format slots this call refused because the per-format
  // fall-through produced no conversion (#1374).  Counted so the refusal is
  // observable in production: without it an operator cannot tell "there was
  // nothing to convert" from "the conversion was never attempted", which is
  // the exact blind spot that let #1374 sit unnoticed.
  uint32_t unconverted_fallthrough = 0;

  // Content analysis result (populated when content_analysis is enabled).
  QualityPreset applied_preset;

  // Whether bilateral filter denoising was applied.
  bool denoised = false;

  // Learned quality prediction tracking.
  bool used_learned_quality = false;  // At least one format used ML prediction
  uint8_t learned_quality_fallbacks = 0;  // Formats that fell back to heuristic

  // SSIMULACRA2 quality verification results (per format, -1=N/A).
  // The unprefixed pair is the ORIGINAL-FORMAT arm: a JPEG re-encode,
  // or a same-format WebP re-encode (#1385).
  float ssimulacra2_score = -1.0f;     // Final original-format arm score
  bool ssimulacra2_reencoded = false;  // Whether that arm re-encoded
  float webp_ssimulacra2_score = -1.0f;
  bool webp_ssimulacra2_reencoded = false;
  float avif_ssimulacra2_score = -1.0f;
  bool avif_ssimulacra2_reencoded = false;

  // Verification declined the variant (mpp #790): the shipped candidate
  // scored below the band floor (the cross-format arms, and since #1385
  // the same-format WebP re-encode lane -- the capped same-format JPEG
  // arm stays accept-at-the-cap, #1284) or could not be measured at all.
  // The arm's TranscodeResult carries success=false with the decline
  // reason; the score fields above keep the measured evidence.
  bool ssimulacra2_declined = false;
  bool webp_ssimulacra2_declined = false;
  bool avif_ssimulacra2_declined = false;

  // Final qualities after SSIMULACRA2 adjustment (-1 = N/A).
  // Carried to subsequent calls in the same proactive loop to
  // avoid redundant re-encode searches.
  int final_jpeg_quality = -1;
  int final_webp_quality = -1;
  int final_avif_quality = -1;

  // Source JPEG quality from DQT tables (-1 if unavailable/non-JPEG).
  int source_jpeg_quality = -1;

  // Quality baselining tracking.
  int quality_capped_count = 0;  // Number of times JPEG quality was capped
  bool skipped_jpeg_reencode =
      false;  // JPEG re-encode skipped (source <= target)
};

// One candidate encode in the SSIMULACRA2 verify loop's selection:
// attempts[0] is the caller's initial encode, the rest are retries in the
// order they were produced.
struct VerifyAttempt {
  float score = 0.0f;  // SSIMULACRA2 verdict for this attempt
  size_t size = 0;     // encoded byte size
};

// Band-closest attempt selection (mpp #790 D2), extracted as a free
// function so the mechanism is directly testable: a compiling ship-last
// mutant survived the whole suite while this logic lived inline, because
// every encoder-driven fixture's score ladder is monotone or terminates
// in-band -- configurations where band-closest and the pre-fix ship-last
// agree.  Synthetic (score, size) lists have no such blind spot.
//
// Returns the index of the attempt to ship.  Distance is zero inside
// [lo, hi], else the gap to the nearer edge: below the floor the highest
// score wins, above the ceiling the down-stepped later attempt wins.
// Ties resolve to the smaller byte size, full ties to the later attempt.
//
// With |decline_below_floor| armed, the policy is asymmetric: an in-band
// or above-band attempt is shippable, a below-floor one is declined.  A
// below-floor attempt therefore never displaces a shippable one, however
// much closer to the band it measures -- otherwise a score cliff wider
// than the band across one quality step would decline a variant whose
// above-band attempt was perfectly servable.  When nothing is shippable
// (or the policy is ship-below-floor), plain band-closest applies.
size_t SelectVerifyAttempt(const std::vector<VerifyAttempt>& attempts, float lo,
                           float hi, bool decline_below_floor);

// Candidate pixels made comparable with the reference for scoring.
// ok=false means NO VERDICT is possible (dims/format mismatch or a
// chroma-ghost candidate): the caller declines (fail-closed).
struct ComparablePixels {
  bool ok = false;
  const uint8_t* pixels = nullptr;  // into candidate, or gray buffer below
  std::string gray;                 // extracted G channel (1<->3 rule)
};

// S3.2 comparability guard: dims must match and bpp must match, with one
// carve-out -- a bpp=1 reference accepts a bpp=3 MONOCHROME candidate and
// the metric then runs at bpp=1 on the candidate's G channel. Everything
// else (1<->4, 4<->3, 3<->1, chroma ghosts) is not comparable.
ComparablePixels MakeComparable(const DecodedImage& reference,
                                const DecodedImage& candidate);

// Result of SSIMULACRA2 quality verification and optional re-encode loop.
struct Ssimulacra2VerifyResult {
  float score = -1.0f;
  bool reencoded = false;
  // The verify refused the variant (mpp #790): callers must treat the
  // encode as failure-to-produce. Either the shipped candidate scored
  // below the band floor (decline policy armed; negative scores are
  // legitimate catastrophic verdicts) or no verdict existed at all
  // (fail-closed on an unmeasurable candidate or a metric failure).
  bool declined = false;
  // No verdict was ever produced: the decode-back failed, was not
  // comparable, or the metric itself could not run, so |score| is the
  // untouched sentinel, not a measurement.  Distinct from a measured
  // (possibly negative) score, which is evidence.
  bool verdict_missing = false;
  // The floor actually applied (target - 0.6*tol) -- decline evidence for
  // the caller's failure reason.
  float band_lo = -1.0f;
};

// Whether a below-floor final score refuses the variant.
enum class VerifyDeclinePolicy : std::uint8_t {
  // Below-floor scores ship as today. The same-format JPEG arm: the
  // source-quality cap can make the band genuinely unreachable, and the
  // best encode under the cap must still ship (#1284 accept-at-ceiling).
  // NOTE: never infer this from max_quality -- EffectiveJpegQualityCap
  // returns 100 when the source quality is unknown, which would silently
  // arm decline on the JPEG arm.
  kShipBelowFloor,
  // A shipped candidate scoring 0 <= score < lo is declined; the variant
  // is not produced. The four cross-format arms only.
  kDeclineBelowFloor,
};

// Verify encoded output quality against a SSIMULACRA2 target and optionally
// re-encode with adjusted quality until the score falls within the asymmetric
// tolerance band [target - 0.6*tol, target + 1.6*tol].
//
// Attempt selection is band-closest (mpp #790 D2): an attempt landing inside
// the band terminates the search and ships; otherwise the attempt closest to
// the band ships (below the floor: the highest score; above the ceiling: the
// down-stepped body, which is also the smallest), ties broken by smaller byte
// size. The shipped attempt's quality is written back through |quality| and
// |reencoded| reports whether it is a retry rather than the initial encode.
//
// Decline: with |decline_policy| == kDeclineBelowFloor a SHIPPED score
// below the floor refuses the variant (verify.declined) -- INCLUDING
// negative scores, which the metric returns as legitimate catastrophic
// verdicts ("very different images"). The one instrumental limit is a
// sub-8x8 REFERENCE (the metric needs 8x8): tiny icons keep shipping
// without a verdict. A shipped candidate that cannot be measured at all --
// decode-back failure, a non-comparable decode, or a metric-internal
// failure (score_fn returning nullopt) -- is declined at EVERY call site,
// including the ship-below-floor JPEG arm: fail-closed; a RETRY that cannot
// be measured keeps the already-verified prior attempt.
//
// |quality| is searched within [min_quality, max_quality].  max_quality is a
// hard ceiling, not a preference: for a same-format JPEG re-encode it is the
// source-quality cap, and the loop accepts the best encode reachable under it
// rather than climbing past it to reach the score band (#1284).
//
// Template parameters avoid std::function overhead; each is injected so the
// loop is directly testable against synthetic encoders/decoders/scorers
// (#1382 -- the same reason SelectVerifyAttempt left the .cc):
//   EncodeFn: (const DecodedImage&) -> TranscodeResult
//   DecodeFn: (std::string_view)    -> DecodedImage
//   ScoreFn:  (const ComparablePixels&) -> std::optional<float>
//             nullopt = the metric could not produce a verdict.
template <typename EncodeFn, typename DecodeFn, typename ScoreFn>
Ssimulacra2VerifyResult VerifySsimulacra2Quality(
    const DecodedImage& reference, TranscodeResult& encoded, EncodeFn encode_fn,
    DecodeFn decode_fn, ScoreFn score_fn, int& quality, int min_quality,
    int max_quality, const char* format_name, float target, float tolerance,
    int max_attempts, int quality_step, VerifyDeclinePolicy decline_policy,
    MessageHandler* handler) {
  Ssimulacra2VerifyResult verify;

  const float lo = target - tolerance * 0.6f;
  const float hi = target + tolerance * 1.6f;
  verify.band_lo = lo;

  // The metric cannot run on images smaller than 8x8 (quality_verifier.cc
  // makes the same structural check). A sub-8x8 REFERENCE has no verdict
  // to bind -- tiny icons keep shipping -- while any verdict below the
  // floor, negative ones included, declines.
  const bool reference_too_small = reference.width < 8 || reference.height < 8;

  DecodedImage decoded_output = decode_fn(encoded.output_data);
  ComparablePixels comparable = MakeComparable(reference, decoded_output);
  if (!comparable.ok) {
    // Silent score-N/A is what let the AVIF arm ship unverified for
    // months (#1274) -- a no-op verify must be LOUD, and an unverifiable
    // shipped candidate is now declined rather than shipped UNVERIFIED.
    if (handler != nullptr) {
      handler->Warning(
          "SSIMULACRA2 verify declined for %s output: decode of the "
          "candidate failed or dims/format mismatch -- variant not produced",
          format_name[0] == '\0' ? "JPEG" : format_name);
    }
    verify.declined = true;
    verify.verdict_missing = true;
    return verify;
  }

  std::optional<float> initial_verdict = score_fn(comparable);
  if (!initial_verdict.has_value()) {
    // A sub-8x8 REFERENCE is the one structural cause: the metric cannot
    // run on it, no verdict exists to bind, and tiny icons keep shipping
    // without one (the carve-out above). Every other absent verdict is a
    // metric-internal failure on a measurable image: the score channel's
    // negative values are legitimate catastrophic verdicts, so the failure
    // must arrive here -- on its own channel -- and decline fail-closed on
    // EVERY arm, the ship-below-floor JPEG arm included, rather than
    // shipping a sentinel score that reads as a measurement (#1382).
    if (reference_too_small) {
      return verify;  // Ships; |score| keeps the no-verdict sentinel.
    }
    if (handler != nullptr) {
      handler->Warning(
          "SSIMULACRA2 verify declined for %s output: the metric failed to "
          "produce a verdict -- variant not produced",
          format_name[0] == '\0' ? "JPEG" : format_name);
    }
    verify.declined = true;
    verify.verdict_missing = true;
    return verify;
  }
  float score = *initial_verdict;
  verify.score = score;

  // Attempt bookkeeping for band-closest selection. The caller's initial
  // encode is attempt 0 and stays in |encoded| until selection ships a
  // retry instead.
  const float initial_score = score;
  const int initial_quality = quality;
  struct Attempt {
    float score;
    size_t size;
    int quality;
    TranscodeResult result;
  };
  std::vector<Attempt> retries;

  if (score >= 0.0f && max_attempts > 1) {
    for (int attempt = 1; attempt < max_attempts; ++attempt) {
      if (score >= lo && score <= hi) break;

      int next_quality = (score < lo)
                             ? std::min(max_quality, quality + quality_step)
                             : std::max(min_quality, quality - quality_step);
      if (next_quality == quality) {
        // The search has run into the ceiling or the floor.  Re-encoding at
        // the same quality would only reproduce the same output, so accept
        // what we have instead of spending another encode on it.
        if (handler != nullptr) {
          handler->Info(
              "SSIMULACRA2 %.1f outside [%.1f, %.1f] but %s quality q=%d is at "
              "the %s of the allowed range -- accepting this encode",
              score, lo, hi, format_name[0] == '\0' ? "JPEG" : format_name,
              quality, score < lo ? "ceiling" : "floor");
        }
        break;
      }
      quality = next_quality;

      if (handler) {
        handler->Info(
            "SSIMULACRA2 %.1f outside [%.1f, %.1f], "
            "re-encoding %s at q=%d (attempt %d)",
            score, lo, hi, format_name, quality, attempt + 1);
      }

      auto retry = encode_fn(reference);
      if (!retry.success) break;

      auto retry_decoded = decode_fn(retry.output_data);
      ComparablePixels retry_comparable =
          MakeComparable(reference, retry_decoded);
      if (!retry_comparable.ok) break;  // Keep the verified prior attempt.

      std::optional<float> retry_verdict = score_fn(retry_comparable);
      // A retry the metric cannot measure keeps the verified prior
      // attempt; a negative retry verdict is likewise not recorded (it
      // displaces nothing).
      if (!retry_verdict.has_value() || *retry_verdict < 0.0f) break;

      score = *retry_verdict;
      verify.score = score;
      retries.push_back(
          {score, retry.output_data.size(), quality, std::move(retry)});
    }
  }

  // Band-closest selection over {initial encode} + retries, via the
  // directly-tested free function (see SelectVerifyAttempt above for the
  // full contract, including the decline-armed asymmetry rule).
  std::vector<VerifyAttempt> attempts;
  attempts.reserve(retries.size() + 1);
  attempts.push_back({initial_score, encoded.output_data.size()});
  for (const Attempt& r : retries) {
    attempts.push_back({r.score, r.size});
  }
  const size_t best = SelectVerifyAttempt(
      attempts, lo, hi,
      decline_policy == VerifyDeclinePolicy::kDeclineBelowFloor);
  if (best > 0) {
    quality = retries[best - 1].quality;
    encoded = std::move(retries[best - 1].result);
    verify.reencoded = true;
    verify.score = retries[best - 1].score;
  } else {
    quality = initial_quality;
    verify.reencoded = false;
    verify.score = initial_score;
  }
  score = verify.score;

  if (decline_policy == VerifyDeclinePolicy::kDeclineBelowFloor &&
      !reference_too_small && verify.score < lo) {
    verify.declined = true;
    if (handler != nullptr) {
      handler->Info(
          "SSIMULACRA2 decline for %s: score %.1f below floor %.1f "
          "(band [%.1f, %.1f]) -- variant not produced",
          format_name[0] == '\0' ? "JPEG" : format_name, verify.score, lo, lo,
          hi);
    }
  }

  // Print any finite verdict -- a declined negative score must be visible
  // in logs. Only the structurally verdict-less sub-8x8 reference stays
  // silent (nothing was measured).
  if (handler && (score >= 0.0f || !reference_too_small)) {
    // JPEG uses no format prefix in the log message for backward compat.
    if (format_name[0] == '\0') {
      handler->Info("SSIMULACRA2 score: %.1f (q=%d%s)", score, quality,
                    verify.reencoded ? ", re-encoded" : "");
    } else {
      handler->Info("SSIMULACRA2 score: %.1f (%s q=%d%s)", score, format_name,
                    quality, verify.reencoded ? ", re-encoded" : "");
    }
  }

  return verify;
}

// Transcodes images between formats based on client capabilities.
//
// Usage:
//   ImageTranscoderConfig config;
//   ImageTranscoder transcoder(config, &handler);  // fixed config
//   // Or with a config accessor for hot-reload:
//   ImageTranscoder transcoder([&]{ return get_config(); }, &handler);
//
//   CapabilityMask mask;
//   mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
//
//   auto result = transcoder.Transcode(jpeg_data, mask);
//   if (result.success) {
//     // result.output_data is WebP-encoded
//   }
class ImageTranscoder {
 public:
  // Construct with a config accessor that is called at transcode time.
  // This enables hot-reload: the accessor can return a fresh config
  // snapshot on every call (e.g., from an atomic shared_ptr).
  using ConfigAccessor = std::function<ImageTranscoderConfig()>;
  ImageTranscoder(ConfigAccessor config_accessor, MessageHandler* handler);

  // Convenience constructor for a fixed (non-reloadable) config.
  ImageTranscoder(const ImageTranscoderConfig& config, MessageHandler* handler);

  // Transcode image data based on the target mask's image format.
  //
  // If the target format matches the source format, the image is
  // optimized in-place (e.g., lossless JPEG optimization).
  //
  // If the target format is different, the image is converted
  // (e.g., JPEG -> WebP).
  //
  // If the target format is not supported (e.g., AVIF), falls back
  // to optimizing the original format.
  TranscodeResult Transcode(std::string_view input_data,
                            const CapabilityMask& target_mask);

  // Decode image to raw pixels.  Returns empty DecodedImage on failure.
  // Enforces max_decoded_pixels to prevent OOM.
  DecodedImage DecodeToPixels(std::string_view input_data);

  // Generate multiple format variants from a single decode pass.
  // Only produces formats listed in |formats|.  GIF input is handled
  // specially (animated GIF -> WebP uses its own pipeline).
  // |source_jpeg_quality| is the source JPEG quality from DQT tables
  // (-1 if unavailable/non-JPEG), used for quality capping.
  MultiTranscodeResult TranscodeMulti(
      std::string_view input_data,
      const std::vector<CapabilityMask::ImageFormat>& formats,
      int source_jpeg_quality = -1);

  // Config-accepting overload: uses |config_override| instead of config().
  // Enables callers (e.g., TranscodeMultiResized fast-path) to pass
  // pre-adjusted quality settings (learned quality, content-class factors).
  // When |skip_quality_cap| is true, ApplyJpegQualityCap is not called
  // (caller already applied it).  |caller_decoded| is the caller's own
  // full-size decode of |input_data|, when it has one: this call reuses
  // that frame instead of decoding the source a second time (#1407).
  MultiTranscodeResult TranscodeMulti(
      std::string_view input_data,
      const std::vector<CapabilityMask::ImageFormat>& formats,
      int source_jpeg_quality, const ImageTranscoderConfig& config_override,
      bool skip_quality_cap = false,
      const DecodedImage* caller_decoded = nullptr);

  // Resize a decoded pixel buffer to the target width for a viewport class.
  // Returns an empty DecodedImage if no resize is needed (Desktop with
  // viewport_widths.desktop == 0, or image already smaller).
  // Preserves aspect ratio.  When density is k2xPlus, the target width
  // is doubled (e.g., mobile 480px * 2 = 960px for retina).
  DecodedImage ResizeForViewport(
      const DecodedImage& decoded, CapabilityMask::Viewport viewport,
      CapabilityMask::PixelDensity density = CapabilityMask::PixelDensity::k1x);

  // TranscodeMulti with viewport-based resizing.  Decodes once,
  // resizes if the viewport target width is set, then encodes to
  // all requested formats.  When save_data is kOn, uses lower
  // quality settings from config.  When density is k2xPlus,
  // doubles the viewport target width.
  //
  // |quality| carries the two JPEG quality inputs by name; see
  // JpegQualityInputs.  Callers that have the source bytes can fill
  // source_quality with DetectSourceJpegQuality().
  MultiTranscodeResult TranscodeMultiResized(
      std::string_view input_data,
      const std::vector<CapabilityMask::ImageFormat>& formats,
      CapabilityMask::Viewport viewport,
      CapabilityMask::PixelDensity density = CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData save_data = CapabilityMask::SaveData::kOff,
      JpegQualityInputs quality = {});

  // Estimate the source JPEG quality from the input's DQT tables.
  // Returns -1 for non-JPEG input or when the quality cannot be determined.
  // Callers hoist this out of per-variant loops: the estimate depends only
  // on the input bytes.
  static int DetectSourceJpegQuality(std::string_view input_data,
                                     MessageHandler* handler);

  // Replace the message handler (e.g., after wrapping with
  // TeeMessageHandler).
  void set_handler(MessageHandler* handler) { handler_ = handler; }

  // Check if GIF data contains multiple frames (animated).
  static bool IsAnimatedGif(std::string_view data);

  // Pixel decodes this transcoder has performed: one count per
  // DecodeToPixels entry on non-empty input, source decodes and verify
  // decode-backs alike.  The multi-format contract is "decode once,
  // encode many", so the count is the per-instance cost observable that
  // pins it: a call that decodes its source twice is measurably wrong
  // here (#1380).
  uint64_t decode_count() const {
    return decode_count_.load(std::memory_order_relaxed);
  }

  // Maximum decoded pixel buffer size (default 50MB).
  static constexpr size_t kMaxDecodedPixels = 50 * 1024 * 1024;

 private:
  // Decode AVIF to raw pixels via libavif. The scanline-reader framework
  // has no AVIF reader (AVIF is an output-only format in the encode
  // arms), so the SSIMULACRA2 verify floor decodes through this instead
  // (#1274 -- without it the floor's decode_fn returns empty and the
  // verify silently no-ops).
  DecodedImage DecodeAvifToPixels(std::string_view input_data);

  // Optimize JPEG in-place (lossless or lossy).
  // Uses quality settings from |cfg|.
  TranscodeResult OptimizeJpeg(std::string_view input_data,
                               const ImageTranscoderConfig& cfg);

  // Optimize PNG in-place.
  TranscodeResult OptimizePng(std::string_view input_data);

  // Optimize WebP in-place: decode, re-encode at |cfg|'s WebP settings, and
  // keep the result only if it is STRICTLY smaller than the input -- the same
  // re-encode-and-accept-if-smaller rule OptimizeJpeg and OptimizePng apply
  // (#1375). |decoded| may be a decode the caller already performed for this
  // same input, which a multi-format call has; pass nullptr to decode here.
  //
  // The re-encode consults the binding quality verdict (#1385): with
  // cfg.quality_verify on, a candidate the verifier condemns (below the
  // band floor -- negative scores are legitimate catastrophic verdicts)
  // or cannot measure is DECLINED, success=false with the decline
  // reason, and the caller falls back exactly as it does for a
  // no-savings re-encode: the origin's own bytes keep serving. This
  // lane used to ship on the size test alone. The capped same-format
  // JPEG arm's accept-at-the-cap rule is deliberately NOT inherited:
  // that rule exists because a source-quality cap can make the band
  // unreachable by design (#1284), and no such cap binds this search.
  // |verify_out|, when non-null, receives the verdict evidence so multi
  // callers can record it on the original-format arm's fields.
  TranscodeResult OptimizeWebp(std::string_view input_data,
                               const ImageTranscoderConfig& cfg,
                               const DecodedImage* decoded = nullptr,
                               SameFormatWebpVerify* verify_out = nullptr);

  // Level A PNG carry-through (opt-in via cfg.c2pa_carry). Recompresses
  // the PNG via OptimizePng, then splices the ORIGINAL caBX/iTXt manifest chunks
  // back in immediately before the trailing IEND. Fail-safe to serving the
  // original bytes verbatim (skip-not-strip) on ANY anomaly -- recompress failed
  // or saved nothing, output is not a well-formed PNG, no carrier chunk found, or
  // no terminating IEND -- so a manifest is never silently dropped. Never parses
  // or re-authors the manifest (D3). Callers must guarantee `input_data` is a PNG.
  TranscodeResult OptimizePngWithC2paCarry(std::string_view input_data);

  // Convert image to WebP.
  TranscodeResult ConvertToWebp(std::string_view input_data);

  // Convert image to AVIF.
  TranscodeResult ConvertToAvif(std::string_view input_data);

  // Convert GIF (static or animated) to WebP.
  TranscodeResult ConvertGifToWebp(std::string_view input_data);

  // Encode from decoded pixel buffer using quality settings from |cfg|.
  TranscodeResult EncodeJpegFromPixels(const DecodedImage& decoded,
                                       const ImageTranscoderConfig& cfg);
  TranscodeResult EncodeWebpFromPixels(const DecodedImage& decoded,
                                       const ImageTranscoderConfig& cfg);
  TranscodeResult EncodeAvifFromPixels(const DecodedImage& decoded,
                                       const ImageTranscoderConfig& cfg);

  // Returns a snapshot of the current config.  Called at the start
  // of each transcode operation so hot-reloaded values take effect.
  ImageTranscoderConfig config() const { return config_accessor_(); }

  ConfigAccessor config_accessor_;
  MessageHandler* handler_;
  std::atomic<uint64_t> decode_count_{0};
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_IMAGE_TRANSCODER_H_
