// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTML Transform Filter Tests

#include "src/worker/html_transform_filter.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/string_writer.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_parse.h"
#include "lib/html/html_writer_filter.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {
namespace {

// Count occurrences of a substring in a string.
size_t CountOccurrences(std::string_view haystack, std::string_view needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

// Helper to run the transform filter on HTML and return the output.
std::string TransformHtml(
    std::string_view html, std::string_view url,
    const HtmlTransformConfig& config, std::string_view critical_css = "",
    PageSpeedCache* cache = nullptr, std::string_view hostname = "",
    const LcpCandidate& lcp_candidate = {},
    const std::vector<PreconnectOrigin>& preconnect_origins = {},
    const std::vector<std::string>& speculation_urls = {},
    const std::vector<std::string>& defer_scripts = {},
    const std::vector<std::string>& font_urls = {}) {
  net_instaweb::HtmlKeywords::Init();
  net_instaweb::NullMessageHandler message_handler;
  net_instaweb::HtmlParse parser(&message_handler);

  HtmlTransformFilter transform(&parser, config, critical_css, cache, hostname,
                                "https", lcp_candidate, preconnect_origins,
                                speculation_urls, defer_scripts, font_urls);
  parser.AddFilter(&transform);

  std::string output;
  net_instaweb::StringWriter writer(&output);
  net_instaweb::HtmlWriterFilter writer_filter(&parser);
  writer_filter.set_writer(&writer);
  parser.AddFilter(&writer_filter);

  if (!parser.StartParse(url)) return "";
  parser.ParseText(html);
  parser.FinishParse();
  return output;
}

// ========== Critical CSS Injection Tests ==========

TEST(HtmlTransformFilterTest, InjectsCriticalCssBeforeHead) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head><title>Test</title></head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_NE(result.find("body { color: red; }"), std::string::npos);
  // The style tag should be before </head>.
  auto style_pos = result.find("<style");
  auto head_end = result.find("</head>");
  ASSERT_NE(style_pos, std::string::npos);
  ASSERT_NE(head_end, std::string::npos);
  EXPECT_LT(style_pos, head_end);
}

TEST(HtmlTransformFilterTest, SkipsEmptyCriticalCss) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "");

  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos);
}

TEST(HtmlTransformFilterTest, RejectsStyleClosingSequence) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head></head><body></body></html>", "http://example.com/", config,
      "body { } </style><script>alert(1)</script>");

  // Should not inject CSS containing </style.
  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos);
}

// ========== Loading Lazy Tests ==========

TEST(HtmlTransformFilterTest, AddsLazyLoadToImages) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"hero.jpg\">"
      "<img src=\"below.jpg\">"
      "<img src=\"footer.jpg\">"
      "</body></html>",
      "http://example.com/", config);

  // First <img> in body gets fetchpriority="high", not lazy.
  auto first_img = result.find("hero.jpg");
  ASSERT_NE(first_img, std::string::npos);
  auto first_img_tag_end = result.find('>', first_img);
  std::string first_tag =
      result.substr(result.rfind('<', first_img),
                    first_img_tag_end - result.rfind('<', first_img) + 1);
  EXPECT_NE(first_tag.find("fetchpriority=\"high\""), std::string::npos);
  EXPECT_EQ(first_tag.find("loading=\"lazy\""), std::string::npos);

  // Second and third images get loading="lazy".
  auto second_img = result.find("below.jpg");
  ASSERT_NE(second_img, std::string::npos);
  auto second_img_start = result.rfind('<', second_img);
  auto second_img_end = result.find('>', second_img);
  std::string second_tag =
      result.substr(second_img_start, second_img_end - second_img_start + 1);
  EXPECT_NE(second_tag.find("loading=\"lazy\""), std::string::npos);
}

TEST(HtmlTransformFilterTest, SkipsImgWithExistingLoading) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"first.jpg\">"
      "<img src=\"manual.jpg\" loading=\"eager\">"
      "</body></html>",
      "http://example.com/", config);

  // The image with loading="eager" should keep it.
  auto manual_pos = result.find("manual.jpg");
  ASSERT_NE(manual_pos, std::string::npos);
  auto manual_start = result.rfind('<', manual_pos);
  auto manual_end = result.find('>', manual_pos);
  std::string manual_tag =
      result.substr(manual_start, manual_end - manual_start + 1);
  EXPECT_NE(manual_tag.find("loading=\"eager\""), std::string::npos);
  // Should NOT have loading="lazy" added.
  size_t lazy_count = 0;
  size_t pos = 0;
  while ((pos = manual_tag.find("loading=", pos)) != std::string::npos) {
    ++lazy_count;
    pos += 8;
  }
  EXPECT_EQ(lazy_count, 1u);  // Only the original loading="eager".
}

TEST(HtmlTransformFilterTest, FirstBodyIframeNotLazied) {
  // A top-of-page embed (video player etc.) is usually the first iframe;
  // lazy-loading it regresses LCP.  The first body iframe is exempt, the
  // rest get loading="lazy" — mirroring the above-fold img window.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<iframe src=\"hero-video.html\"></iframe>"
      "<iframe src=\"comments.html\"></iframe>"
      "<iframe src=\"ad.html\"></iframe>"
      "</body></html>",
      "http://example.com/", config);

  auto tag_around = [&result](std::string_view needle) {
    auto pos = result.find(needle);
    EXPECT_NE(pos, std::string::npos);
    auto start = result.rfind('<', pos);
    auto end = result.find('>', pos);
    return result.substr(start, end - start + 1);
  };

  EXPECT_EQ(tag_around("hero-video.html").find("loading=\"lazy\""),
            std::string::npos)
      << "The first body iframe (likely above-fold hero embed) must not be "
         "lazy-loaded";
  EXPECT_NE(tag_around("comments.html").find("loading=\"lazy\""),
            std::string::npos)
      << "Later iframes should be lazy-loaded";
  EXPECT_NE(tag_around("ad.html").find("loading=\"lazy\""), std::string::npos);
}

TEST(HtmlTransformFilterTest, HiddenGtmIframeDoesNotConsumeExemption) {
  // The canonical Google Tag Manager <noscript> iframe is the FIRST body
  // iframe on a large fraction of real pages.  It is invisible (0x0,
  // display:none), so it must get NO transform — lazy on a layout-less
  // iframe makes browsers skip the load, breaking no-JS tracking — and it
  // must NOT consume the above-fold exemption: the first VISIBLE iframe
  // (the real hero embed) keeps it.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<noscript><iframe src=\"https://www.googletagmanager.com/ns.html"
      "?id=GTM-XXXX\" height=\"0\" width=\"0\" "
      "style=\"display:none;visibility:hidden\"></iframe></noscript>"
      "<iframe src=\"hero-video.html\"></iframe>"
      "<iframe src=\"comments.html\"></iframe>"
      "</body></html>",
      "http://example.com/", config);

  auto tag_around = [&result](std::string_view needle) {
    auto pos = result.find(needle);
    EXPECT_NE(pos, std::string::npos);
    auto start = result.rfind('<', pos);
    auto end = result.find('>', pos);
    return result.substr(start, end - start + 1);
  };

  EXPECT_EQ(tag_around("googletagmanager").find("loading"), std::string::npos)
      << "The hidden GTM iframe must keep exactly its original attributes";
  EXPECT_EQ(tag_around("hero-video.html").find("loading=\"lazy\""),
            std::string::npos)
      << "The first VISIBLE body iframe keeps the above-fold exemption";
  EXPECT_NE(tag_around("comments.html").find("loading=\"lazy\""),
            std::string::npos)
      << "Iframes after the exempt visible one are still lazy-loaded";
}

TEST(HtmlTransformFilterTest, HiddenIframeAtLaterPositionNotLazied) {
  // An invisible iframe past the exemption window gets no transform either:
  // lazy-loading a layout-less tracking frame means it never loads for
  // no-JS users.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<iframe src=\"hero-video.html\"></iframe>"
      "<iframe src=\"comments.html\"></iframe>"
      "<iframe src=\"tracker.html\" width=\"0\" height=\"0\" "
      "style=\"display:none\"></iframe>"
      "</body></html>",
      "http://example.com/", config);

  auto tag_around = [&result](std::string_view needle) {
    auto pos = result.find(needle);
    EXPECT_NE(pos, std::string::npos);
    auto start = result.rfind('<', pos);
    auto end = result.find('>', pos);
    return result.substr(start, end - start + 1);
  };

  EXPECT_EQ(tag_around("hero-video.html").find("loading=\"lazy\""),
            std::string::npos)
      << "The first visible body iframe stays exempt";
  EXPECT_NE(tag_around("comments.html").find("loading=\"lazy\""),
            std::string::npos);
  EXPECT_EQ(tag_around("tracker.html").find("loading"), std::string::npos)
      << "A position-3 hidden iframe must not be lazy-loaded";
}

TEST(HtmlTransformFilterTest, InvisibleIframeMarkersEachRecognized) {
  // Each invisibility marker individually exempts the iframe from lazy
  // loading AND from consuming the above-fold exemption slot.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  for (const char* marker :
       {"width=\"0\"", "height=\"0\"", "hidden", "style=\"display:none\"",
        "style=\"visibility:hidden\""}) {
    std::string html = "<html><body><iframe src=\"tracker.html\" ";
    html += marker;
    html +=
        "></iframe>"
        "<iframe src=\"hero-video.html\"></iframe>"
        "</body></html>";
    std::string result = TransformHtml(html, "http://example.com/", config);

    auto tag_around = [&result](std::string_view needle) {
      auto pos = result.find(needle);
      EXPECT_NE(pos, std::string::npos);
      auto start = result.rfind('<', pos);
      auto end = result.find('>', pos);
      return result.substr(start, end - start + 1);
    };

    EXPECT_EQ(tag_around("tracker.html").find("loading"), std::string::npos)
        << "Marker not recognized as invisible: " << marker;
    EXPECT_EQ(tag_around("hero-video.html").find("loading=\"lazy\""),
              std::string::npos)
        << "Invisible iframe (" << marker
        << ") must not consume the exemption slot";
  }
}

TEST(HtmlTransformFilterTest, IframeWithoutSizeInfoTreatedAsVisible) {
  // No false positives: an iframe with no size info and no hiding markers
  // is visible by default — it takes the exemption slot, and a normally
  // sized iframe is visible too.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<iframe src=\"embed.html\" title=\"player\"></iframe>"
      "<iframe src=\"sized.html\" width=\"560\" height=\"315\"></iframe>"
      "</body></html>",
      "http://example.com/", config);

  auto tag_around = [&result](std::string_view needle) {
    auto pos = result.find(needle);
    EXPECT_NE(pos, std::string::npos);
    auto start = result.rfind('<', pos);
    auto end = result.find('>', pos);
    return result.substr(start, end - start + 1);
  };

  EXPECT_EQ(tag_around("embed.html").find("loading=\"lazy\""),
            std::string::npos)
      << "A marker-less iframe is visible and takes the exemption slot";
  EXPECT_NE(tag_around("sized.html").find("loading=\"lazy\""),
            std::string::npos)
      << "A normally sized iframe is visible: the slot is already consumed, "
         "so it is lazy-loaded";
}

TEST(HtmlTransformFilterTest, TrackingPixelNotPromotedToFetchpriority) {
  // Without an LCP candidate the first PLAUSIBLE body img gets
  // fetchpriority="high".  A 1x1 beacon first in <body> must not soak up
  // the high-priority slot — the next real image within the above-fold
  // window takes it instead.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"pixel.gif\" width=\"1\" height=\"1\">"
      "<img src=\"hero.jpg\">"
      "</body></html>",
      "http://example.com/", config);

  auto tag_around = [&result](std::string_view needle) {
    auto pos = result.find(needle);
    EXPECT_NE(pos, std::string::npos);
    auto start = result.rfind('<', pos);
    auto end = result.find('>', pos);
    return result.substr(start, end - start + 1);
  };

  EXPECT_EQ(tag_around("pixel.gif").find("fetchpriority"), std::string::npos)
      << "A 1x1 beacon must not be promoted to fetchpriority=high";
  EXPECT_NE(tag_around("hero.jpg").find("fetchpriority=\"high\""),
            std::string::npos)
      << "The first plausible image should take the promotion instead";
  EXPECT_EQ(tag_around("hero.jpg").find("loading=\"lazy\""), std::string::npos)
      << "The promoted image must not be lazy-loaded";
}

TEST(HtmlTransformFilterTest, HiddenImagesGetNoTransform) {
  // Invisible images get NO transform at all: no promotion (they cannot be
  // the LCP element) and no loading="lazy" — browsers skip lazy images with
  // no layout box, so lazying a display:none beacon breaks the analytics it
  // exists for.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"spacer.gif\" hidden>"
      "<img src=\"deferred.png\" style=\"display: none\">"
      "<img src=\"real.jpg\">"
      "</body></html>",
      "http://example.com/", config);

  auto tag_around = [&result](std::string_view needle) {
    auto pos = result.find(needle);
    EXPECT_NE(pos, std::string::npos);
    auto start = result.rfind('<', pos);
    auto end = result.find('>', pos);
    return result.substr(start, end - start + 1);
  };

  EXPECT_EQ(tag_around("spacer.gif").find("fetchpriority"), std::string::npos);
  EXPECT_EQ(tag_around("spacer.gif").find("loading"), std::string::npos)
      << "A hidden first body img must keep exactly its original attributes";
  EXPECT_EQ(tag_around("deferred.png").find("fetchpriority"), std::string::npos)
      << "display:none images are not rendered and must not be promoted";
  EXPECT_EQ(tag_around("deferred.png").find("loading"), std::string::npos)
      << "display:none images must not be lazy-loaded either";
  EXPECT_NE(tag_around("real.jpg").find("fetchpriority=\"high\""),
            std::string::npos);
}

TEST(HtmlTransformFilterTest, BeaconUntouchedAtAnyPosition) {
  // An implausible image is exempt from BOTH transforms at every position —
  // not just inside the promotion window.  Plausible images around it keep
  // the normal treatment (first promoted, later ones lazied).
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"a.jpg\">"
      "<img src=\"b.jpg\">"
      "<img src=\"pixel.gif\" width=\"1\" height=\"1\">"
      "<img src=\"c.jpg\">"
      "</body></html>",
      "http://example.com/", config);

  auto tag_around = [&result](std::string_view needle) {
    auto pos = result.find(needle);
    EXPECT_NE(pos, std::string::npos);
    auto start = result.rfind('<', pos);
    auto end = result.find('>', pos);
    return result.substr(start, end - start + 1);
  };

  EXPECT_NE(tag_around("a.jpg").find("fetchpriority=\"high\""),
            std::string::npos)
      << "The first plausible image still takes the promotion";
  EXPECT_NE(tag_around("b.jpg").find("loading=\"lazy\""), std::string::npos)
      << "Plausible images after the promoted one are still lazied";
  EXPECT_EQ(tag_around("pixel.gif").find("loading"), std::string::npos)
      << "A position-3 beacon must not be lazy-loaded";
  EXPECT_EQ(tag_around("pixel.gif").find("fetchpriority"), std::string::npos);
  EXPECT_NE(tag_around("c.jpg").find("loading=\"lazy\""), std::string::npos)
      << "Plausible images after the beacon are unaffected";
}

TEST(HtmlTransformFilterTest, IframeOutsideBodyStillLazied) {
  // Without an explicit <body> tag the in-body tracking never engages, so
  // the first-iframe exemption cannot fire and the iframe is lazy-loaded —
  // pinning the pre-existing behavior for body-less documents.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result =
      TransformHtml("<html><iframe src=\"embed.html\"></iframe></html>",
                    "http://example.com/", config);

  EXPECT_NE(result.find("loading=\"lazy\""), std::string::npos)
      << "An iframe outside an explicit <body> keeps the old lazy behavior";
}

TEST(HtmlTransformFilterTest, NoPromotionWhenOnlyBeaconsAboveFold) {
  // If no plausible image exists inside the above-fold window, promote
  // nothing (fail-safe) rather than promoting a below-fold image.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"p1.gif\" width=\"1\" height=\"1\">"
      "<img src=\"p2.gif\" width=\"0\" height=\"0\">"
      "<img src=\"p3.gif\" width=\"1\" height=\"1\">"
      "<img src=\"below-fold.jpg\">"
      "</body></html>",
      "http://example.com/", config);

  EXPECT_EQ(result.find("fetchpriority"), std::string::npos)
      << "No plausible above-fold image -> no promotion at all";
}

// ========== Image Dimensions Tests (without cache) ==========

TEST(HtmlTransformFilterTest, SkipsDimensionsWithoutCache) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;

  std::string input = "<html><body><img src=\"test.jpg\"></body></html>";
  std::string result = TransformHtml(input, "http://example.com/", config);

  // Without cache, no dimensions should be added.
  EXPECT_EQ(result.find("width="), std::string::npos);
  EXPECT_EQ(result.find("height="), std::string::npos);
}

// ========== Combined Tests ==========

TEST(HtmlTransformFilterTest, CombinesAllTransformations) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"hero.jpg\">"
      "<img src=\"lazy.jpg\">"
      "</body></html>",
      "http://example.com/", config, "h1 { font-size: 2em; }");

  // Critical CSS injected.
  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos);
  // First img gets fetchpriority.
  EXPECT_NE(result.find("fetchpriority=\"high\""), std::string::npos);
  // Second img gets lazy.
  auto lazy_pos = result.find("lazy.jpg");
  ASSERT_NE(lazy_pos, std::string::npos);
  auto lazy_start = result.rfind('<', lazy_pos);
  auto lazy_end = result.find('>', lazy_pos);
  std::string lazy_tag = result.substr(lazy_start, lazy_end - lazy_start + 1);
  EXPECT_NE(lazy_tag.find("loading=\"lazy\""), std::string::npos);
}

TEST(HtmlTransformFilterTest, DisabledConfigProducesNoChanges) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string input =
      "<html><head></head><body><img src=\"test.jpg\"></body></html>";
  std::string result = TransformHtml(input, "http://example.com/", config);

  // No modifications — output should match input structurally.
  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos);
  EXPECT_EQ(result.find("loading="), std::string::npos);
  EXPECT_EQ(result.find("fetchpriority="), std::string::npos);
}

// ========== Pixel Dimension Validation ==========

TEST(HtmlTransformFilterTest, IsValidPixelDimension) {
  // Valid dimensions.
  EXPECT_TRUE(HtmlTransformFilter::IsValidPixelDimension("100"));
  EXPECT_TRUE(HtmlTransformFilter::IsValidPixelDimension("1"));

  // Invalid dimensions.
  EXPECT_FALSE(HtmlTransformFilter::IsValidPixelDimension(nullptr));
  EXPECT_FALSE(HtmlTransformFilter::IsValidPixelDimension(""));
  EXPECT_FALSE(HtmlTransformFilter::IsValidPixelDimension("0"));
  EXPECT_FALSE(HtmlTransformFilter::IsValidPixelDimension("100%"));
  EXPECT_FALSE(HtmlTransformFilter::IsValidPixelDimension("auto"));
  EXPECT_FALSE(HtmlTransformFilter::IsValidPixelDimension("10px"));

  // Largest accepted dimension: 5 digits, i.e. kMaxPixelDimension (99999).
  EXPECT_TRUE(HtmlTransformFilter::IsValidPixelDimension("99999"));
  // The two below are both rejected on LENGTH (6 digits), not on a numeric
  // comparison -- that is the only mechanism enforcing the range here, and it
  // is why a zero-padded but numerically in-range value is rejected too.
  EXPECT_FALSE(HtmlTransformFilter::IsValidPixelDimension("100000"));
  EXPECT_FALSE(HtmlTransformFilter::IsValidPixelDimension("099999"));
}

// ========== LCP Preload Tests ==========

TEST(HtmlTransformFilterTest, InjectsLcpPreloadLink) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::string result = TransformHtml(
      "<html><head><title>Test</title></head>"
      "<body><img src=\"/images/hero.jpg\"></body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_NE(result.find("as=\"image\""), std::string::npos);
  EXPECT_NE(result.find("href=\"/images/hero.jpg\""), std::string::npos);
  EXPECT_NE(result.find("fetchpriority=\"high\""), std::string::npos);

  // Preload link should be before </head>.
  auto preload_pos = result.find("rel=\"preload\"");
  auto head_end = result.find("</head>");
  ASSERT_NE(preload_pos, std::string::npos);
  ASSERT_NE(head_end, std::string::npos);
  EXPECT_LT(preload_pos, head_end);
}

TEST(HtmlTransformFilterTest, LcpPreloadSuppressedForImgInsidePicture) {
  // Inside <picture> a <source> sibling may win source selection, so
  // preloading the img's src risks downloading an image the browser never
  // uses.  The preload must be suppressed; fetchpriority on the <img>
  // element itself stays correct whichever source wins.
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";
  lcp.in_picture = true;

  std::string result = TransformHtml(
      "<html><head><title>Test</title></head><body>"
      "<picture>"
      "<source srcset=\"/images/hero.avif\" type=\"image/avif\">"
      "<img src=\"/images/hero.jpg\">"
      "</picture>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos)
      << "No preload for an LCP img inside <picture> (a <source> may win)";
  EXPECT_NE(result.find("fetchpriority=\"high\""), std::string::npos)
      << "The <img> itself still gets fetchpriority=high";
}

TEST(HtmlTransformFilterTest, LcpImageGetsFetchPriorityHigh) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/first.jpg\">"
      "<img src=\"/images/hero.jpg\">"
      "<img src=\"/images/third.jpg\">"
      "<img src=\"/images/fourth.jpg\">"
      "<img src=\"/images/fifth.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  // hero.jpg should get fetchpriority="high" (it's the LCP candidate).
  auto hero_pos = result.find("hero.jpg");
  ASSERT_NE(hero_pos, std::string::npos);
  auto hero_start = result.rfind('<', hero_pos);
  auto hero_end = result.find('>', hero_pos);
  std::string hero_tag = result.substr(hero_start, hero_end - hero_start + 1);
  EXPECT_NE(hero_tag.find("fetchpriority=\"high\""), std::string::npos);
  EXPECT_EQ(hero_tag.find("loading=\"lazy\""), std::string::npos);

  // first.jpg is an early image (within first 3) with LCP set — no lazy.
  auto first_pos = result.find("first.jpg");
  ASSERT_NE(first_pos, std::string::npos);
  auto first_start = result.rfind('<', first_pos);
  auto first_end = result.find('>', first_pos);
  std::string first_tag =
      result.substr(first_start, first_end - first_start + 1);
  EXPECT_EQ(first_tag.find("loading=\"lazy\""), std::string::npos)
      << "Early image should not be lazy-loaded when LCP candidate is set";

  // fifth.jpg (img #5) should get loading="lazy" (past the guard).
  auto fifth_pos = result.find("fifth.jpg");
  ASSERT_NE(fifth_pos, std::string::npos);
  auto fifth_start = result.rfind('<', fifth_pos);
  auto fifth_end = result.find('>', fifth_pos);
  std::string fifth_tag =
      result.substr(fifth_start, fifth_end - fifth_start + 1);
  EXPECT_NE(fifth_tag.find("loading=\"lazy\""), std::string::npos)
      << "Late image should be lazy-loaded";
}

TEST(HtmlTransformFilterTest, FallbackToFirstImgWhenNoCandidate) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;

  // No LCP candidate — should fall back to first-img heuristic.
  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"first.jpg\">"
      "<img src=\"second.jpg\">"
      "</body></html>",
      "http://example.com/", config);

  auto first_pos = result.find("first.jpg");
  ASSERT_NE(first_pos, std::string::npos);
  auto first_start = result.rfind('<', first_pos);
  auto first_end = result.find('>', first_pos);
  std::string first_tag =
      result.substr(first_start, first_end - first_start + 1);
  EXPECT_NE(first_tag.find("fetchpriority=\"high\""), std::string::npos)
      << "Without LCP candidate, first body img should get fetchpriority";
}

TEST(HtmlTransformFilterTest, PreloadBeforeCriticalCss) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "body { color: red; }", nullptr, "", lcp);

  auto preload_pos = result.find("rel=\"preload\"");
  auto style_pos = result.find("data-pagespeed-critical");
  ASSERT_NE(preload_pos, std::string::npos);
  ASSERT_NE(style_pos, std::string::npos);
  EXPECT_LT(preload_pos, style_pos)
      << "Preload link should appear before critical CSS";
}

TEST(HtmlTransformFilterTest, NoPreloadWithEmptyCandidate) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  // Empty LCP candidate.
  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config);

  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos)
      << "No preload should be injected without an LCP candidate";
}

TEST(HtmlTransformFilterTest, PreloadWithSrcset) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";
  lcp.srcset = "/images/hero-2x.jpg 2x";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_NE(result.find("imagesrcset=\"/images/hero-2x.jpg 2x\""),
            std::string::npos)
      << "Preload should include imagesrcset when srcset is available";
}

// ========== H1: URL Scheme Validation Tests ==========

TEST(HtmlTransformFilterTest, RejectsJavascriptUrlInPreload) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "javascript:alert(1)";

  std::string result = TransformHtml(
      "<html><head><title>Test</title></head>"
      "<body><img src=\"javascript:alert(1)\"></body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos)
      << "javascript: URL should not produce a preload link";
}

TEST(HtmlTransformFilterTest, RejectsDataUrlInPreload) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "data:text/html,<script>alert(1)</script>";

  std::string result =
      TransformHtml("<html><head></head><body></body></html>",
                    "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos)
      << "data: URL should not produce a preload link";
}

TEST(HtmlTransformFilterTest, AllowsRelativeUrlInPreload) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos)
      << "Relative URL should produce a preload link";
  EXPECT_NE(result.find("href=\"/images/hero.jpg\""), std::string::npos);
}

TEST(HtmlTransformFilterTest, AllowsAbsoluteUrlInPreload) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "https://cdn.example.com/hero.jpg";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"https://cdn.example.com/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos)
      << "Absolute https URL should produce a preload link";
  EXPECT_NE(result.find("href=\"https://cdn.example.com/hero.jpg\""),
            std::string::npos);
}

TEST(HtmlTransformFilterTest, JavascriptUrlDoesNotGetFetchPriority) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;

  LcpCandidate lcp;
  lcp.src = "javascript:alert(1)";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/first.jpg\">"
      "<img src=\"/images/second.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  // With an invalid LCP candidate, fallback to first-img heuristic.
  auto first_pos = result.find("first.jpg");
  ASSERT_NE(first_pos, std::string::npos);
  auto first_start = result.rfind('<', first_pos);
  auto first_end = result.find('>', first_pos);
  std::string first_tag =
      result.substr(first_start, first_end - first_start + 1);
  EXPECT_NE(first_tag.find("fetchpriority=\"high\""), std::string::npos)
      << "First img should get fetchpriority via fallback when LCP URL is "
         "invalid";
}

// ========== M3: Early Image Lazy-Load Guard Tests ==========

TEST(HtmlTransformFilterTest, EarlyImagesNotLazyLoadedWithLcpCandidate) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "<img src=\"/images/second.jpg\">"
      "<img src=\"/images/third.jpg\">"
      "<img src=\"/images/fourth.jpg\">"
      "<img src=\"/images/fifth.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  // hero.jpg (img #1) should get fetchpriority="high".
  auto hero_pos = result.find("hero.jpg");
  ASSERT_NE(hero_pos, std::string::npos);
  auto hero_start = result.rfind('<', hero_pos);
  auto hero_end = result.find('>', hero_pos);
  std::string hero_tag = result.substr(hero_start, hero_end - hero_start + 1);
  EXPECT_NE(hero_tag.find("fetchpriority=\"high\""), std::string::npos);
  EXPECT_EQ(hero_tag.find("loading=\"lazy\""), std::string::npos);

  // second.jpg (img #2) should NOT be lazy-loaded (early image guard).
  auto second_pos = result.find("second.jpg");
  ASSERT_NE(second_pos, std::string::npos);
  auto second_start = result.rfind('<', second_pos);
  auto second_end = result.find('>', second_pos);
  std::string second_tag =
      result.substr(second_start, second_end - second_start + 1);
  EXPECT_EQ(second_tag.find("loading=\"lazy\""), std::string::npos)
      << "Early image #2 should not be lazy-loaded";

  // third.jpg (img #3) should NOT be lazy-loaded (early image guard).
  auto third_pos = result.find("third.jpg");
  ASSERT_NE(third_pos, std::string::npos);
  auto third_start = result.rfind('<', third_pos);
  auto third_end = result.find('>', third_pos);
  std::string third_tag =
      result.substr(third_start, third_end - third_start + 1);
  EXPECT_EQ(third_tag.find("loading=\"lazy\""), std::string::npos)
      << "Early image #3 should not be lazy-loaded";

  // fourth.jpg (img #4) SHOULD be lazy-loaded.
  auto fourth_pos = result.find("fourth.jpg");
  ASSERT_NE(fourth_pos, std::string::npos);
  auto fourth_start = result.rfind('<', fourth_pos);
  auto fourth_end = result.find('>', fourth_pos);
  std::string fourth_tag =
      result.substr(fourth_start, fourth_end - fourth_start + 1);
  EXPECT_NE(fourth_tag.find("loading=\"lazy\""), std::string::npos)
      << "Image #4 should be lazy-loaded";

  // fifth.jpg (img #5) SHOULD be lazy-loaded.
  auto fifth_pos = result.find("fifth.jpg");
  ASSERT_NE(fifth_pos, std::string::npos);
  auto fifth_start = result.rfind('<', fifth_pos);
  auto fifth_end = result.find('>', fifth_pos);
  std::string fifth_tag =
      result.substr(fifth_start, fifth_end - fifth_start + 1);
  EXPECT_NE(fifth_tag.find("loading=\"lazy\""), std::string::npos)
      << "Image #5 should be lazy-loaded";
}

// ========== M5: Null-Byte Bypass Tests ==========

TEST(HtmlTransformFilterTest, NullByteBypassClosingStyleCheck) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  // Construct CSS with null byte embedded in "</style" to try to bypass
  // the XSS check. After null-byte stripping, this becomes
  // "</style><script>alert(1)</script>".
  std::string malicious_css = "body { } </sty";
  malicious_css.push_back('\0');
  malicious_css += "le><script>alert(1)</script>";

  std::string result =
      TransformHtml("<html><head></head><body></body></html>",
                    "http://example.com/", config, malicious_css);

  // Should NOT inject CSS — the null-byte bypass should be caught.
  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos)
      << "CSS with null-byte-obfuscated </style> should be rejected";
}

// ========== M6: Combined Production Config Test ==========

TEST(HtmlTransformFilterTest, CombinedProductionConfig) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::string result = TransformHtml(
      "<html><head><title>Production</title></head><body>"
      "<img src=\"/images/nav-logo.jpg\">"
      "<img src=\"/images/hero.jpg\">"
      "<img src=\"/images/sidebar.jpg\">"
      "<img src=\"/images/article1.jpg\">"
      "<img src=\"/images/article2.jpg\">"
      "</body></html>",
      "http://example.com/", config, "h1 { font-size: 2em; }", nullptr, "",
      lcp);

  // 1. Preload link in <head>.
  auto preload_pos = result.find("rel=\"preload\"");
  ASSERT_NE(preload_pos, std::string::npos)
      << "Should have preload link in head";
  auto head_end = result.find("</head>");
  ASSERT_NE(head_end, std::string::npos);
  EXPECT_LT(preload_pos, head_end);

  // 2. Critical CSS <style> in <head> after preload.
  auto style_pos = result.find("data-pagespeed-critical");
  ASSERT_NE(style_pos, std::string::npos) << "Should have critical CSS in head";
  EXPECT_LT(preload_pos, style_pos)
      << "Preload should appear before critical CSS";
  EXPECT_LT(style_pos, head_end);

  // 3. LCP image (hero.jpg) gets fetchpriority="high".
  // Find the one in <body>, not the preload href.
  auto body_start = result.find("<body>");
  auto hero_body_pos = result.find("hero.jpg", body_start);
  ASSERT_NE(hero_body_pos, std::string::npos);
  auto hero_start = result.rfind('<', hero_body_pos);
  auto hero_end = result.find('>', hero_body_pos);
  std::string hero_tag = result.substr(hero_start, hero_end - hero_start + 1);
  EXPECT_NE(hero_tag.find("fetchpriority=\"high\""), std::string::npos)
      << "LCP image should get fetchpriority high";

  // 4. Early non-LCP images (nav-logo, sidebar) have NO loading="lazy".
  auto nav_pos = result.find("nav-logo.jpg");
  ASSERT_NE(nav_pos, std::string::npos);
  auto nav_start = result.rfind('<', nav_pos);
  auto nav_end = result.find('>', nav_pos);
  std::string nav_tag = result.substr(nav_start, nav_end - nav_start + 1);
  EXPECT_EQ(nav_tag.find("loading=\"lazy\""), std::string::npos)
      << "Early non-LCP image should not be lazy-loaded";

  auto sidebar_pos = result.find("sidebar.jpg");
  ASSERT_NE(sidebar_pos, std::string::npos);
  auto sidebar_start = result.rfind('<', sidebar_pos);
  auto sidebar_end = result.find('>', sidebar_pos);
  std::string sidebar_tag =
      result.substr(sidebar_start, sidebar_end - sidebar_start + 1);
  EXPECT_EQ(sidebar_tag.find("loading=\"lazy\""), std::string::npos)
      << "Early non-LCP image should not be lazy-loaded";

  // 5. Late images (article1, article2) get loading="lazy".
  auto art1_pos = result.find("article1.jpg");
  ASSERT_NE(art1_pos, std::string::npos);
  auto art1_start = result.rfind('<', art1_pos);
  auto art1_end = result.find('>', art1_pos);
  std::string art1_tag = result.substr(art1_start, art1_end - art1_start + 1);
  EXPECT_NE(art1_tag.find("loading=\"lazy\""), std::string::npos)
      << "Late image should be lazy-loaded";

  auto art2_pos = result.find("article2.jpg");
  ASSERT_NE(art2_pos, std::string::npos);
  auto art2_start = result.rfind('<', art2_pos);
  auto art2_end = result.find('>', art2_pos);
  std::string art2_tag = result.substr(art2_start, art2_end - art2_start + 1);
  EXPECT_NE(art2_tag.find("loading=\"lazy\""), std::string::npos)
      << "Late image should be lazy-loaded";
}

// ========== Imagesizes Support Test ==========

TEST(HtmlTransformFilterTest, PreloadWithSizes) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";
  lcp.srcset = "/images/hero-2x.jpg 2x";
  lcp.sizes = "(max-width: 600px) 100vw, 50vw";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_NE(result.find("imagesrcset=\"/images/hero-2x.jpg 2x\""),
            std::string::npos)
      << "Should include imagesrcset";
  EXPECT_NE(result.find("imagesizes=\"(max-width: 600px) 100vw, 50vw\""),
            std::string::npos)
      << "Should include imagesizes when sizes is set";
}

// ========== IsAllowedPreloadUrl Unit Tests ==========

TEST(HtmlTransformFilterTest, IsAllowedPreloadUrl) {
  // Allowed schemes.
  EXPECT_TRUE(
      HtmlTransformFilter::IsAllowedPreloadUrl("https://example.com/img.jpg"));
  EXPECT_TRUE(
      HtmlTransformFilter::IsAllowedPreloadUrl("http://example.com/img.jpg"));
  EXPECT_TRUE(HtmlTransformFilter::IsAllowedPreloadUrl("/images/hero.jpg"));

  // Protocol-relative URLs rejected (CSRF risk in speculation rules).
  EXPECT_FALSE(
      HtmlTransformFilter::IsAllowedPreloadUrl("//cdn.example.com/img.jpg"));
  EXPECT_FALSE(HtmlTransformFilter::IsAllowedPreloadUrl("//evil.com/steal"));

  // Disallowed schemes.
  EXPECT_FALSE(HtmlTransformFilter::IsAllowedPreloadUrl("javascript:alert(1)"));
  EXPECT_FALSE(
      HtmlTransformFilter::IsAllowedPreloadUrl("data:text/html,<h1>hi</h1>"));
  EXPECT_FALSE(HtmlTransformFilter::IsAllowedPreloadUrl("vbscript:foo"));
  EXPECT_FALSE(HtmlTransformFilter::IsAllowedPreloadUrl(""));
  EXPECT_FALSE(HtmlTransformFilter::IsAllowedPreloadUrl("relative/path.jpg"));
}

// ========== Preconnect Injection Tests ==========

TEST(HtmlTransformFilterTest, InjectsPreconnectLinks) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = true;

  std::vector<PreconnectOrigin> origins = {
      {"https://cdn.example.com", /*crossorigin=*/false},
      {"https://fonts.gstatic.com", /*crossorigin=*/true}};

  std::string result = TransformHtml(
      "<html><head><title>Test</title></head>"
      "<body><p>Content</p></body></html>",
      "http://example.com/", config, "", nullptr, "", {}, origins);

  EXPECT_NE(result.find("rel=\"preconnect\""), std::string::npos);
  // The no-cors origin warms the plain pool: NO crossorigin attribute
  // (attribute order is rel, href, [crossorigin], data-pagespeed-hint).
  EXPECT_NE(result.find("href=\"https://cdn.example.com\" "
                        "data-pagespeed-hint"),
            std::string::npos)
      << "no-cors origin must get a bare preconnect. Got: " << result;
  // The CORS-mode origin (fonts) warms the crossorigin pool.
  EXPECT_NE(result.find("href=\"https://fonts.gstatic.com\" crossorigin"),
            std::string::npos)
      << "CORS-mode origin must get a crossorigin preconnect. Got: " << result;
  EXPECT_EQ(CountOccurrences(result, "crossorigin"), 1u)
      << "Exactly one preconnect should carry crossorigin";

  // Preconnect links should be before </head>.
  auto preconnect_pos = result.find("rel=\"preconnect\"");
  auto head_end = result.find("</head>");
  ASSERT_NE(preconnect_pos, std::string::npos);
  ASSERT_NE(head_end, std::string::npos);
  EXPECT_LT(preconnect_pos, head_end);
}

TEST(HtmlTransformFilterTest, PreconnectBeforeLcpPreload) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;
  config.enable_preconnect_injection = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::vector<PreconnectOrigin> origins = {{"https://cdn.example.com"}};

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp, origins);

  auto preconnect_pos = result.find("rel=\"preconnect\"");
  auto preload_pos = result.find("rel=\"preload\"");
  ASSERT_NE(preconnect_pos, std::string::npos);
  ASSERT_NE(preload_pos, std::string::npos);
  EXPECT_LT(preconnect_pos, preload_pos)
      << "Preconnect should appear before LCP preload";
}

TEST(HtmlTransformFilterTest, NoPreconnectWhenDisabled) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  std::vector<PreconnectOrigin> origins = {{"https://cdn.example.com"}};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, origins);

  EXPECT_EQ(result.find("rel=\"preconnect\""), std::string::npos)
      << "Preconnect should not be injected when disabled";
}

TEST(HtmlTransformFilterTest, NoPreconnectWithEmptyOrigins) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = true;

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config);

  EXPECT_EQ(result.find("rel=\"preconnect\""), std::string::npos)
      << "No preconnect with empty origins list";
}

TEST(HtmlTransformFilterTest, PreconnectRejectsInvalidScheme) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = true;

  std::vector<PreconnectOrigin> origins = {{"javascript:alert(1)"},
                                           {"https://cdn.example.com"},
                                           {"data:text/html,hi"}};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, origins);

  // Only valid origin should be injected.
  EXPECT_NE(result.find("href=\"https://cdn.example.com\""), std::string::npos);
  EXPECT_EQ(result.find("javascript"), std::string::npos);
  EXPECT_EQ(result.find("data:"), std::string::npos);
}

TEST(HtmlTransformFilterTest, PreconnectOrderingInHead) {
  // Full production config: preconnect first, then LCP preload, then critical
  // CSS.
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;
  config.enable_preconnect_injection = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::vector<PreconnectOrigin> origins = {{"https://cdn.example.com"}};

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "body { color: red; }", nullptr, "", lcp,
      origins);

  auto preconnect_pos = result.find("rel=\"preconnect\"");
  auto preload_pos = result.find("rel=\"preload\"");
  auto critical_pos = result.find("data-pagespeed-critical");
  auto head_end = result.find("</head>");

  ASSERT_NE(preconnect_pos, std::string::npos);
  ASSERT_NE(preload_pos, std::string::npos);
  ASSERT_NE(critical_pos, std::string::npos);
  ASSERT_NE(head_end, std::string::npos);

  EXPECT_LT(preconnect_pos, preload_pos) << "Preconnect before LCP preload";
  EXPECT_LT(preload_pos, critical_pos) << "LCP preload before critical CSS";
  EXPECT_LT(critical_pos, head_end) << "All before </head>";
}

// ========== Speculation Rules Tests ==========

TEST(HtmlTransformFilterTest, InjectsSpeculationRules) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_speculation_rules = true;

  std::vector<std::string> spec_urls = {"https://example.com/page1",
                                        "https://example.com/page2"};

  std::string result = TransformHtml(
      "<html><head></head><body><p>Content</p></body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, spec_urls);

  // Should contain speculationrules script.
  EXPECT_NE(result.find("type=\"speculationrules\""), std::string::npos);
  // Should contain valid JSON with prefetch.
  EXPECT_NE(result.find("\"prefetch\""), std::string::npos);
  EXPECT_NE(result.find(R"("source":"list")"), std::string::npos);
  EXPECT_NE(result.find("https://example.com/page1"), std::string::npos);
  EXPECT_NE(result.find("https://example.com/page2"), std::string::npos);

  // Should be before </body>.
  auto script_pos = result.find("speculationrules");
  auto body_end = result.find("</body>");
  ASSERT_NE(script_pos, std::string::npos);
  ASSERT_NE(body_end, std::string::npos);
  EXPECT_LT(script_pos, body_end);
}

TEST(HtmlTransformFilterTest, SpeculationRulesDisabledByDefault) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  // enable_speculation_rules defaults to false

  std::vector<std::string> spec_urls = {"https://example.com/page1"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, spec_urls);

  EXPECT_EQ(result.find("speculationrules"), std::string::npos)
      << "Speculation rules should not be injected when disabled";
}

TEST(HtmlTransformFilterTest, SpeculationRulesEscapesSpecialChars) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  // URL with characters that need JSON escaping.
  std::vector<std::string> spec_urls = {"https://example.com/path?a=1&b=2"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, spec_urls);

  EXPECT_NE(result.find("speculationrules"), std::string::npos);
  // URL should be present (ampersand doesn't need JSON escaping).
  EXPECT_NE(result.find("https://example.com/path?a=1&b=2"), std::string::npos);
}

TEST(HtmlTransformFilterTest, SpeculationRulesEscapesAngleBrackets) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  // URL containing < that must be escaped to prevent </script> injection.
  std::vector<std::string> spec_urls = {"https://example.com/path?q=a<b"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, spec_urls);

  EXPECT_NE(result.find("speculationrules"), std::string::npos);
  // The < in the URL should be escaped as \u003c in JSON.
  EXPECT_NE(result.find("\\u003c"), std::string::npos)
      << "< should be escaped to \\u003c in JSON";
  // The raw < should not appear within the JSON content.
  // (Find the JSON portion between speculationrules and </script>.)
  auto json_start = result.find("speculationrules\">");
  auto json_end = result.find("</script>", json_start);
  if (json_start != std::string::npos && json_end != std::string::npos) {
    std::string json_content =
        result.substr(json_start + 18, json_end - json_start - 18);
    EXPECT_EQ(json_content.find('<'), std::string::npos)
        << "Raw < should not appear in JSON content";
  }
}

TEST(HtmlTransformFilterTest, NoSpeculationRulesWithEmptyUrls) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config);

  EXPECT_EQ(result.find("speculationrules"), std::string::npos)
      << "Should not inject speculation rules with empty URL list";
}

TEST(HtmlTransformFilterTest, SpeculationRulesRejectsProtocolRelativeUrls) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  // Protocol-relative URLs could point to attacker-controlled hosts, causing
  // the browser to prefetch from evil.com with page cookies (CSRF).
  std::vector<std::string> spec_urls = {"//evil.com/steal",
                                        "//cdn.example.com/page"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, spec_urls);

  EXPECT_EQ(result.find("speculationrules"), std::string::npos)
      << "Protocol-relative URLs should be filtered from speculation rules";
  EXPECT_EQ(result.find("evil.com"), std::string::npos);
}

TEST(HtmlTransformFilterTest, SpeculationRulesFiltersInvalidUrls) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  std::vector<std::string> spec_urls = {"javascript:alert(1)",
                                        "data:text/html,hi"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, spec_urls);

  // All URLs invalid, so no injection should happen.
  EXPECT_EQ(result.find("speculationrules"), std::string::npos)
      << "Should not inject when all URLs are invalid";
}

// ========== Phase 1.5: <picture> Element Handling ==========

TEST(HtmlTransformFilterTest, PictureElementImgGetsLazyLoad) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  // hero.jpg = img #1, img2 = #2, img3 = #3 (guard covers 1-3),
  // card.jpg = img #4 (lazy), footer.jpg = img #5 (lazy).
  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "<img src=\"/images/img2.jpg\">"
      "<img src=\"/images/img3.jpg\">"
      "<picture>"
      "<source srcset=\"/images/card.avif\" type=\"image/avif\">"
      "<source srcset=\"/images/card.webp\" type=\"image/webp\">"
      "<img src=\"/images/card.jpg\" alt=\"Card\">"
      "</picture>"
      "<picture>"
      "<source srcset=\"/images/footer.avif\" type=\"image/avif\">"
      "<img src=\"/images/footer.jpg\" alt=\"Footer\">"
      "</picture>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  // hero.jpg gets fetchpriority="high" (LCP candidate).
  auto hero_pos = result.find("hero.jpg");
  ASSERT_NE(hero_pos, std::string::npos);
  auto hero_start = result.rfind('<', hero_pos);
  auto hero_end = result.find('>', hero_pos);
  std::string hero_tag = result.substr(hero_start, hero_end - hero_start + 1);
  EXPECT_NE(hero_tag.find("fetchpriority=\"high\""), std::string::npos);

  // card.jpg (img #4) inside <picture> should get loading="lazy".
  auto card_img_pos = result.find("card.jpg");
  ASSERT_NE(card_img_pos, std::string::npos);
  auto card_start = result.rfind('<', card_img_pos);
  auto card_end = result.find('>', card_img_pos);
  std::string card_tag = result.substr(card_start, card_end - card_start + 1);
  EXPECT_NE(card_tag.find("loading=\"lazy\""), std::string::npos)
      << "img inside <picture> past guard should get loading=\"lazy\"";

  // footer.jpg (img #5) inside <picture> should get loading="lazy".
  auto footer_img_pos = result.find("footer.jpg");
  ASSERT_NE(footer_img_pos, std::string::npos);
  auto footer_start = result.rfind('<', footer_img_pos);
  auto footer_end = result.find('>', footer_img_pos);
  std::string footer_tag =
      result.substr(footer_start, footer_end - footer_start + 1);
  EXPECT_NE(footer_tag.find("loading=\"lazy\""), std::string::npos)
      << "img inside late <picture> should get loading=\"lazy\"";
}

TEST(HtmlTransformFilterTest, PictureElementNotDoubleLazy) {
  // Verify that only the <img> inside <picture> gets loading="lazy",
  // not the <source> elements (which are void elements anyway).
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/first.jpg\">"
      "<picture>"
      "<source srcset=\"/images/pic.avif\" type=\"image/avif\">"
      "<img src=\"/images/pic.jpg\" alt=\"Pic\">"
      "</picture>"
      "</body></html>",
      "http://example.com/", config);

  // Count occurrences of loading="lazy" — should be exactly 1
  // (on the <img> inside <picture>, not duplicated).
  size_t lazy_count = 0;
  size_t search_pos = 0;
  while ((search_pos = result.find("loading=\"lazy\"", search_pos)) !=
         std::string::npos) {
    ++lazy_count;
    search_pos += 14;
  }
  EXPECT_EQ(lazy_count, 1u)
      << "Only one loading=\"lazy\" expected (on the <img> inside <picture>)";
}

// ========== Phase 4.5: fetchpriority Preservation ==========

TEST(HtmlTransformFilterTest, PreservesExistingFetchPriorityHigh) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\" fetchpriority=\"high\">"
      "<img src=\"/images/second.jpg\">"
      "</body></html>",
      "http://example.com/", config);

  // hero.jpg already has fetchpriority="high" — should not be duplicated.
  auto hero_pos = result.find("hero.jpg");
  ASSERT_NE(hero_pos, std::string::npos);
  auto hero_start = result.rfind('<', hero_pos);
  auto hero_end = result.find('>', hero_pos);
  std::string hero_tag = result.substr(hero_start, hero_end - hero_start + 1);

  size_t fp_count = 0;
  size_t pos = 0;
  while ((pos = hero_tag.find("fetchpriority", pos)) != std::string::npos) {
    ++fp_count;
    pos += 13;
  }
  EXPECT_EQ(fp_count, 1u)
      << "fetchpriority should not be duplicated on hero image";
  EXPECT_NE(hero_tag.find("fetchpriority=\"high\""), std::string::npos);
}

TEST(HtmlTransformFilterTest, PreservesExistingFetchPriorityLow) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/decorative.jpg\" fetchpriority=\"low\">"
      "<img src=\"/images/main.jpg\">"
      "</body></html>",
      "http://example.com/", config);

  // decorative.jpg already has fetchpriority="low" — filter should not
  // override it with "high".
  auto dec_pos = result.find("decorative.jpg");
  ASSERT_NE(dec_pos, std::string::npos);
  auto dec_start = result.rfind('<', dec_pos);
  auto dec_end = result.find('>', dec_pos);
  std::string dec_tag = result.substr(dec_start, dec_end - dec_start + 1);
  EXPECT_NE(dec_tag.find("fetchpriority=\"low\""), std::string::npos)
      << "Existing fetchpriority=\"low\" should be preserved";
  EXPECT_EQ(dec_tag.find("fetchpriority=\"high\""), std::string::npos)
      << R"(Should not override fetchpriority="low" with "high")";
}

TEST(HtmlTransformFilterTest, ImgWithExistingLazyLoading) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/first.jpg\">"
      "<img src=\"/images/already-lazy.jpg\" loading=\"lazy\">"
      "</body></html>",
      "http://example.com/", config);

  // already-lazy.jpg should keep its loading="lazy" without duplication.
  auto lazy_pos = result.find("already-lazy.jpg");
  ASSERT_NE(lazy_pos, std::string::npos);
  auto lazy_start = result.rfind('<', lazy_pos);
  auto lazy_end = result.find('>', lazy_pos);
  std::string lazy_tag = result.substr(lazy_start, lazy_end - lazy_start + 1);
  size_t loading_count = 0;
  size_t pos = 0;
  while ((pos = lazy_tag.find("loading=", pos)) != std::string::npos) {
    ++loading_count;
    pos += 8;
  }
  EXPECT_EQ(loading_count, 1u) << "loading attribute should not be duplicated";
}

// ========== Revalidation Deduplication Tests ==========

TEST(HtmlTransformFilterTest, RevalidationDoesNotDuplicatePreconnect) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = true;

  std::vector<PreconnectOrigin> origins = {{"https://cdn.example.com"},
                                           {"https://fonts.googleapis.com"}};

  // Input has preconnect links from a previous pass (with marker attribute).
  std::string input =
      "<html><head>"
      "<link rel=\"preconnect\" href=\"https://cdn.example.com\" "
      "crossorigin data-pagespeed-hint>"
      "<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\" "
      "crossorigin data-pagespeed-hint>"
      "</head><body></body></html>";

  std::string result = TransformHtml(input, "http://example.com/", config, "",
                                     nullptr, "", {}, origins);

  EXPECT_EQ(CountOccurrences(result, "rel=\"preconnect\""), 2u)
      << "Should have exactly 2 preconnect links, not 4";
}

TEST(HtmlTransformFilterTest, RevalidationDoesNotDuplicateLcpPreload) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  // Input has a preload link from a previous pass.
  std::string input =
      "<html><head>"
      "<link rel=\"preload\" as=\"image\" href=\"/images/hero.jpg\" "
      "fetchpriority=\"high\" data-pagespeed-hint>"
      "</head><body><img src=\"/images/hero.jpg\"></body></html>";

  std::string result =
      TransformHtml(input, "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_EQ(CountOccurrences(result, "rel=\"preload\""), 1u)
      << "Should have exactly 1 preload link, not 2";
}

TEST(HtmlTransformFilterTest, RevalidationDoesNotDuplicateSpeculationRules) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  std::vector<std::string> spec_urls = {"https://example.com/page1"};

  // Input has speculation rules from a previous pass.
  std::string input =
      "<html><head></head><body>"
      "<script type=\"speculationrules\" "
      "data-pagespeed-hint>{\"prefetch\":[{\"source\":\"list\",\"urls\":["
      "\"https://example.com/page1\"]}]}</script>"
      "</body></html>";

  std::string result = TransformHtml(input, "http://example.com/", config, "",
                                     nullptr, "", {}, {}, spec_urls);

  EXPECT_EQ(CountOccurrences(result, "speculationrules"), 1u)
      << "Should have exactly 1 speculation rules script";
}

TEST(HtmlTransformFilterTest, PreservesUserAuthoredPreconnectLinks) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = true;

  std::vector<PreconnectOrigin> origins = {{"https://cdn.example.com"}};

  // Input has a user-authored preconnect link (NO data-pagespeed-hint marker).
  std::string input =
      "<html><head>"
      "<link rel=\"preconnect\" href=\"https://user-origin.example.com\" "
      "crossorigin>"
      "</head><body></body></html>";

  std::string result = TransformHtml(input, "http://example.com/", config, "",
                                     nullptr, "", {}, origins);

  EXPECT_NE(result.find("href=\"https://user-origin.example.com\""),
            std::string::npos)
      << "User-authored preconnect should be preserved";
  EXPECT_NE(result.find("href=\"https://cdn.example.com\""), std::string::npos)
      << "PageSpeed preconnect should also be injected";
}

TEST(HtmlTransformFilterTest, PreservesUserAuthoredPreloadLinks) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  // Input has a user-authored preload link (NO marker).
  std::string input =
      "<html><head>"
      "<link rel=\"preload\" as=\"style\" href=\"/css/main.css\">"
      "</head><body><img src=\"/images/hero.jpg\"></body></html>";

  std::string result =
      TransformHtml(input, "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_NE(result.find("href=\"/css/main.css\""), std::string::npos)
      << "User-authored preload should be preserved";
  EXPECT_NE(result.find("href=\"/images/hero.jpg\""), std::string::npos)
      << "LCP preload should also be injected";
}

TEST(HtmlTransformFilterTest, MarkerAttributeOnInjectedPreconnect) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = true;

  std::vector<PreconnectOrigin> origins = {{"https://cdn.example.com"}};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, origins);

  EXPECT_NE(result.find("data-pagespeed-hint"), std::string::npos)
      << "Injected preconnect should have data-pagespeed-hint marker";
}

TEST(HtmlTransformFilterTest, MarkerAttributeOnInjectedPreload) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::string result = TransformHtml(
      "<html><head></head><body><img src=\"/images/hero.jpg\"></body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_NE(result.find("data-pagespeed-hint"), std::string::npos)
      << "Injected preload should have data-pagespeed-hint marker";
}

TEST(HtmlTransformFilterTest, CombinedRevalidationAllInjections) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;
  config.enable_preconnect_injection = true;
  config.enable_speculation_rules = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::vector<PreconnectOrigin> origins = {{"https://cdn.example.com"}};
  std::vector<std::string> spec_urls = {"https://example.com/page1"};

  // Input has all previously-injected elements (with markers).
  std::string input =
      "<html><head>"
      "<link rel=\"preconnect\" href=\"https://cdn.example.com\" "
      "crossorigin data-pagespeed-hint>"
      "<link rel=\"preload\" as=\"image\" href=\"/images/hero.jpg\" "
      "fetchpriority=\"high\" data-pagespeed-hint>"
      "<style data-pagespeed-critical>body { color: red; }</style>"
      "</head><body>"
      "<img src=\"/images/hero.jpg\">"
      "<script type=\"speculationrules\" "
      "data-pagespeed-hint>{\"prefetch\":[{\"source\":\"list\",\"urls\":["
      "\"https://example.com/page1\"]}]}</script>"
      "</body></html>";

  std::string result = TransformHtml(input, "http://example.com/", config,
                                     "body { color: blue; }", nullptr, "", lcp,
                                     origins, spec_urls);

  EXPECT_EQ(CountOccurrences(result, "rel=\"preconnect\""), 1u);
  EXPECT_EQ(CountOccurrences(result, "rel=\"preload\""), 1u);
  EXPECT_EQ(CountOccurrences(result, "data-pagespeed-critical"), 1u);
  EXPECT_EQ(CountOccurrences(result, "speculationrules"), 1u);
}

TEST(HtmlTransformFilterTest, TriplePassRevalidationStable) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;
  config.enable_preconnect_injection = true;
  config.enable_speculation_rules = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";

  std::vector<PreconnectOrigin> origins = {{"https://cdn.example.com"}};
  std::vector<std::string> spec_urls = {"https://example.com/page1"};

  std::string html =
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>";

  // Run 3 passes — output of each pass feeds into the next.
  for (int pass = 0; pass < 3; ++pass) {
    html = TransformHtml(html, "http://example.com/", config,
                         "body { color: red; }", nullptr, "", lcp, origins,
                         spec_urls);
    EXPECT_EQ(CountOccurrences(html, "rel=\"preconnect\""), 1u)
        << "Pass " << pass + 1 << ": preconnect count should be 1";
    EXPECT_EQ(CountOccurrences(html, "rel=\"preload\""), 1u)
        << "Pass " << pass + 1 << ": preload count should be 1";
    EXPECT_EQ(CountOccurrences(html, "data-pagespeed-critical"), 1u)
        << "Pass " << pass + 1 << ": critical CSS count should be 1";
    EXPECT_EQ(CountOccurrences(html, "speculationrules"), 1u)
        << "Pass " << pass + 1 << ": speculation rules count should be 1";
  }
}

TEST(HtmlTransformFilterTest, DisabledConfigStillStripsOldMarkers) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_speculation_rules = false;

  // Input has old marked links from a previous enabled config.
  std::string input =
      "<html><head>"
      "<link rel=\"preconnect\" href=\"https://cdn.example.com\" "
      "crossorigin data-pagespeed-hint>"
      "<link rel=\"preload\" as=\"image\" href=\"/images/hero.jpg\" "
      "fetchpriority=\"high\" data-pagespeed-hint>"
      "</head><body>"
      "<script type=\"speculationrules\" "
      "data-pagespeed-hint>{}</script>"
      "</body></html>";

  std::string result = TransformHtml(input, "http://example.com/", config);

  EXPECT_EQ(result.find("data-pagespeed-hint"), std::string::npos)
      << "Old marked elements should be stripped even when features are "
         "disabled";
}

// ========== Coverage: revalidation deletes critical CSS, srcset validation,
//                     image dimensions with existing width/height ==========

TEST(HtmlTransformFilterTest, RevalidationDeletesCriticalCssStyle) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  // HTML that already has a <style data-pagespeed-critical> from a previous
  // pass. On revalidation, the filter should remove it and inject a fresh one.
  std::string result = TransformHtml(
      "<html><head>"
      "<style data-pagespeed-critical>old { color: red; }</style>"
      "<title>Test</title>"
      "</head><body></body></html>",
      "http://example.com/", config, "new { color: blue; }");

  // The old critical CSS should be gone.
  EXPECT_EQ(result.find("old { color: red; }"), std::string::npos)
      << "Old critical CSS should be removed on revalidation";
  // The new critical CSS should be injected.
  EXPECT_NE(result.find("new { color: blue; }"), std::string::npos)
      << "New critical CSS should be injected";
  // There should be exactly one data-pagespeed-critical attribute in the output
  // (the new one).
  size_t first = result.find("data-pagespeed-critical");
  ASSERT_NE(first, std::string::npos)
      << "The new style should have data-pagespeed-critical";
  size_t second = result.find("data-pagespeed-critical", first + 1);
  EXPECT_EQ(second, std::string::npos)
      << "There should be only one data-pagespeed-critical attribute (old one "
         "deleted)";
}

TEST(HtmlTransformFilterTest, SrcsetDisallowedUrl) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";
  lcp.srcset = "data:image/gif;base64,R0lGODlhAQ 1x, /images/hero-2x.jpg 2x";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  // The preload link should be injected (src is valid).
  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos);
  // But the srcset should NOT be included because it contains a data: URL.
  EXPECT_EQ(result.find("imagesrcset"), std::string::npos)
      << "Srcset with data: URL should not be included in preload";
}

TEST(HtmlTransformFilterTest, ImageDimensionsSkipsExistingWidth) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;

  // An <img> with width already set — the filter should NOT add another width
  // (even without a cache, the early check short-circuits).
  std::string input =
      R"(<html><body><img src="test.jpg" width="200"></body></html>)";
  std::string result = TransformHtml(input, "http://example.com/", config);

  // Count occurrences of "width=" in the img tag.
  auto img_pos = result.find("test.jpg");
  ASSERT_NE(img_pos, std::string::npos);
  auto img_start = result.rfind('<', img_pos);
  auto img_end = result.find('>', img_pos);
  std::string img_tag = result.substr(img_start, img_end - img_start + 1);

  size_t width_count = 0;
  size_t pos = 0;
  while ((pos = img_tag.find("width=", pos)) != std::string::npos) {
    ++width_count;
    pos += 6;
  }
  EXPECT_EQ(width_count, 1u) << "Should not add duplicate width attribute";
}

TEST(HtmlTransformFilterTest, ImageDimensionsSkipsExistingHeight) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;

  // An <img> with height already set.
  std::string input =
      R"(<html><body><img src="test.jpg" height="150"></body></html>)";
  std::string result = TransformHtml(input, "http://example.com/", config);

  // Count occurrences of "height=" in the img tag.
  auto img_pos = result.find("test.jpg");
  ASSERT_NE(img_pos, std::string::npos);
  auto img_start = result.rfind('<', img_pos);
  auto img_end = result.find('>', img_pos);
  std::string img_tag = result.substr(img_start, img_end - img_start + 1);

  size_t height_count = 0;
  size_t pos = 0;
  while ((pos = img_tag.find("height=", pos)) != std::string::npos) {
    ++height_count;
    pos += 7;
  }
  EXPECT_EQ(height_count, 1u) << "Should not add duplicate height attribute";
}

// ========== Coverage: Critical CSS fallback injection before </body> ==========

TEST(HtmlTransformFilterTest, CriticalCssFallbackToBody) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  // HTML without a <head> element — CSS should be injected before </body>.
  std::string result =
      TransformHtml("<html><body><p>Content</p></body></html>",
                    "http://example.com/", config, "p { margin: 0; }");

  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos)
      << "Critical CSS should be injected as fallback before </body>";
  EXPECT_NE(result.find("p { margin: 0; }"), std::string::npos);

  // The style tag should be before </body>.
  auto style_pos = result.find("<style");
  auto body_end = result.find("</body>");
  ASSERT_NE(style_pos, std::string::npos);
  ASSERT_NE(body_end, std::string::npos);
  EXPECT_LT(style_pos, body_end);
}

// ========== Coverage: Speculation rules with backslash and quote escaping ==========

TEST(HtmlTransformFilterTest, SpeculationRulesEscapesBackslashAndQuotes) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  // URL with quotes and backslashes that need escaping.
  std::vector<std::string> spec_urls = {"https://example.com/path?q=a\"b\\c"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, spec_urls);

  EXPECT_NE(result.find("speculationrules"), std::string::npos);
  // Quotes should be escaped.
  EXPECT_NE(result.find("\\\""), std::string::npos)
      << "Quotes in URL should be escaped in speculation rules JSON";
  // Backslashes should be escaped.
  EXPECT_NE(result.find("\\\\"), std::string::npos)
      << "Backslashes in URL should be escaped in speculation rules JSON";
}

// ========== Coverage: Speculation rules with control characters (lines 248-253) ==========

TEST(HtmlTransformFilterTest, SpeculationRulesEscapesControlChars) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  // URL with a control character (\x01 SOH) that won't be stripped by
  // the HTML parser.
  std::string url_with_ctrl = "https://example.com/path";
  url_with_ctrl.push_back('\x01');
  url_with_ctrl += "end";
  std::vector<std::string> spec_urls = {url_with_ctrl};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, spec_urls);

  // The control character should be escaped. Check both that the rules
  // were injected and that no raw control character appears in the JSON.
  EXPECT_NE(result.find("speculationrules"), std::string::npos);

  // Find the JSON portion between speculationrules"> and </script>.
  auto json_start = result.find("speculationrules\">");
  auto json_end = result.find("</script>", json_start);
  if (json_start != std::string::npos && json_end != std::string::npos) {
    std::string json_content =
        result.substr(json_start + 18, json_end - json_start - 18);
    // The raw \x01 should not appear (it should be escaped to \u0001).
    EXPECT_EQ(json_content.find('\x01'), std::string::npos)
        << "Raw control character should be escaped in JSON";
  }
}

// ========== Coverage: Speculation rules with mix of valid/invalid URLs ==========

TEST(HtmlTransformFilterTest, SpeculationRulesFiltersMixedUrls) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_speculation_rules = true;

  std::vector<std::string> spec_urls = {"javascript:alert(1)",  // filtered
                                        "https://example.com/valid1",  // kept
                                        "data:text/html,hi",  // filtered
                                        "https://example.com/valid2"};  // kept

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, spec_urls);

  EXPECT_NE(result.find("speculationrules"), std::string::npos)
      << "Should inject rules when some URLs are valid";
  EXPECT_NE(result.find("https://example.com/valid1"), std::string::npos);
  EXPECT_NE(result.find("https://example.com/valid2"), std::string::npos);
  EXPECT_EQ(result.find("javascript"), std::string::npos)
      << "javascript: URL should be filtered out";
  EXPECT_EQ(result.find("data:"), std::string::npos)
      << "data: URL should be filtered out";
}

// ========== Coverage: Lazy load on img not in body (in_body_ guard) ==========

TEST(HtmlTransformFilterTest, ImgInHeadNotLazyLoaded) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = true;
  config.enable_image_dimensions = false;

  // An <img> in <head> is unusual but should not crash and should get lazy.
  std::string result = TransformHtml(
      "<html><head><img src=\"icon.png\"></head>"
      "<body><img src=\"hero.jpg\"><img src=\"below.jpg\"></body></html>",
      "http://example.com/", config);

  // The <img> in <head> should get loading="lazy" (it's not in_body_,
  // so the fetchpriority/LCP guard doesn't apply).
  auto icon_pos = result.find("icon.png");
  ASSERT_NE(icon_pos, std::string::npos);
  auto icon_start = result.rfind('<', icon_pos);
  auto icon_end = result.find('>', icon_pos);
  std::string icon_tag = result.substr(icon_start, icon_end - icon_start + 1);
  EXPECT_NE(icon_tag.find("loading=\"lazy\""), std::string::npos)
      << "Img in <head> should get loading=lazy (no body img guard)";

  // hero.jpg (first body img) should get fetchpriority="high".
  auto hero_pos = result.find("hero.jpg");
  ASSERT_NE(hero_pos, std::string::npos);
  auto hero_start = result.rfind('<', hero_pos);
  auto hero_end = result.find('>', hero_pos);
  std::string hero_tag = result.substr(hero_start, hero_end - hero_start + 1);
  EXPECT_NE(hero_tag.find("fetchpriority=\"high\""), std::string::npos);
}

// ========== Coverage: Preload srcset with multiple valid entries ==========

TEST(HtmlTransformFilterTest, PreloadSrcsetMultipleValidEntries) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "/images/hero.jpg";
  lcp.srcset =
      "/images/hero-1x.jpg 1x, /images/hero-2x.jpg 2x, /images/hero-3x.jpg 3x";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"/images/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos);
  // All entries are valid (start with /), so the full srcset should be included.
  EXPECT_NE(result.find(
                "imagesrcset=\"/images/hero-1x.jpg 1x, /images/hero-2x.jpg 2x, "
                "/images/hero-3x.jpg 3x\""),
            std::string::npos)
      << "All valid srcset entries should be preserved in imagesrcset";
}

// ========== Coverage: Critical CSS with case-insensitive </STYLE check ==========

TEST(HtmlTransformFilterTest, RejectsUppercaseStyleClosingSequence) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head></head><body></body></html>", "http://example.com/", config,
      "body { } </STYLE><script>alert(1)</script>");

  // Case-insensitive check should catch </STYLE.
  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos)
      << "Uppercase </STYLE should be rejected (case-insensitive check)";
}

TEST(HtmlTransformFilterTest, RejectsMixedCaseStyleClosing) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head></head><body></body></html>", "http://example.com/", config,
      "body { } </sTyLe><script>alert(1)</script>");

  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos)
      << "Mixed-case </sTyLe should be rejected";
}

// ========== Coverage: Image dimensions with data: URL (line 363) ==========

TEST(HtmlTransformFilterTest, ImageDimensionsSkipsDataUrl) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;

  std::string result = TransformHtml(
      "<html><body><img src=\"data:image/gif;base64,R0lGODlh\"></body></html>",
      "http://example.com/", config);

  // data: URLs should be skipped entirely.
  EXPECT_EQ(result.find("width="), std::string::npos);
  EXPECT_EQ(result.find("height="), std::string::npos);
}

// ========== Coverage: Image dimensions with empty src (line 359) ==========

TEST(HtmlTransformFilterTest, ImageDimensionsSkipsEmptySrc) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;

  std::string result = TransformHtml("<html><body><img src=\"\"></body></html>",
                                     "http://example.com/", config);

  EXPECT_EQ(result.find("width="), std::string::npos);
  EXPECT_EQ(result.find("height="), std::string::npos);
}

// ========== Coverage: Image dimensions with percentage width (line 398) ==========

TEST(HtmlTransformFilterTest, ImageDimensionsSkipsPercentageWidth) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;

  // width="50%" is not a valid pixel dimension, so dimensions should still
  // be looked up (but without a cache, nothing is added).
  std::string result = TransformHtml(
      "<html><body><img src=\"test.jpg\" width=\"50%\"></body></html>",
      "http://example.com/", config);

  // The percentage width should be preserved (not replaced).
  EXPECT_NE(result.find("width=\"50%\""), std::string::npos);
}

// ========== Coverage: Preconnect with only invalid origins (no links) ==========

TEST(HtmlTransformFilterTest, PreconnectAllInvalidOriginsNoLinks) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = true;

  std::vector<PreconnectOrigin> origins = {{"javascript:alert(1)"},
                                           {"data:text/html,hi"}};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, origins);

  // No valid origins, so no preconnect links should be injected.
  EXPECT_EQ(result.find("rel=\"preconnect\""), std::string::npos)
      << "All-invalid origins should produce no preconnect links";
}

// ========== Coverage: LCP preload rejects protocol-relative URL ==========

TEST(HtmlTransformFilterTest, LcpPreloadRejectsProtocolRelativeUrl) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = true;

  LcpCandidate lcp;
  lcp.src = "//cdn.example.com/hero.jpg";

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<img src=\"//cdn.example.com/hero.jpg\">"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", lcp);

  // Protocol-relative URLs are rejected because they resolve to arbitrary
  // hostnames. Legitimate CDN URLs should use https:// instead.
  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos)
      << "Protocol-relative URL should NOT produce a preload link";
}

// ========== Coverage: XSS check character mismatch (lines 132-133, 137) ==========

TEST(HtmlTransformFilterTest, CriticalCssAllowsNonStyleClosingTag) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  // CSS containing "</span>" — starts with "</" but is NOT "</style",
  // so the XSS check loop should hit the mismatch branch and allow
  // injection.
  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config,
                                     "body { } </span> .foo { color: red; }");

  // The CSS should be injected (it's safe — no </style).
  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos)
      << "CSS with </span> should be allowed (not </style)";
  EXPECT_NE(result.find("</span>"), std::string::npos);
}

TEST(HtmlTransformFilterTest, CriticalCssAllowsMultipleNonStyleClosingTags) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  // CSS containing multiple "</xyz" sequences that are NOT "</style".
  std::string result = TransformHtml(
      "<html><head></head><body></body></html>", "http://example.com/", config,
      ".a { } </div> .b { } </section> .c { color: blue; }");

  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos)
      << "CSS with </div> and </section> should be allowed";
}

TEST(HtmlTransformFilterTest, CriticalCssRejectsStyleAfterNonStyleTag) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  // CSS containing both a safe "</div>" and a dangerous "</style>".
  // The XSS check should still reject because of the </style>.
  std::string result = TransformHtml(
      "<html><head></head><body></body></html>", "http://example.com/", config,
      ".a { } </div> .b { } </style><script>alert(1)</script>");

  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos)
      << "CSS with </style> should be rejected even if </div> is also present";
}

// ========== Coverage: ApplyImageDimensions cache hit (lines 372-389) ==========

// Minimal valid 1x1 red PNG (67 bytes).
// Generated from: 8-byte signature + IHDR(1x1,RGBA) + IDAT(filtered) + IEND.
// clang-format off
static constexpr uint8_t kMinimalPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  // width=1, height=1
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,  // 8bit RGB, CRC
    0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,  // IDAT chunk
    0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,  // compressed data
    0x00, 0x00, 0x04, 0x00, 0x01, 0x3B, 0x7E, 0x03,  // checksum
    0x8E, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,  // IEND chunk
    0x44, 0xAE, 0x42, 0x60, 0x82,                     // IEND CRC
};
// clang-format on

// As kMinimalPng, but the IHDR declares width=1000000 (0x000F4240), which is
// above the filter's kMaxPixelDimension (99999) yet still within libpng's
// default 1,000,000 user width limit -- so the header parses cleanly and the
// oversized dimension really does reach the filter.  The IHDR CRC covers the
// width bytes and has been recomputed accordingly (0x1DBF001F).  Keeping the
// original CRC would NOT have exercised the range check: libpng would reject
// the chunk, the filter would take the !dims.valid path, and the test would
// pass with or without the range guard.  (Note: that rejection path returns
// cleanly only with the NullMessageHandler fix in image_dimensions.cc --
// without it, the codec error dereferences a null handler and crashes.)
// clang-format off
static constexpr uint8_t kOversizedPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk
    0x00, 0x0F, 0x42, 0x40, 0x00, 0x00, 0x00, 0x01,  // width=1000000, height=1
    0x08, 0x02, 0x00, 0x00, 0x00, 0x1D, 0xBF, 0x00,  // 8bit RGB, recomputed CRC
    0x1F, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,  // IDAT chunk
    0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,  // compressed data
    0x00, 0x00, 0x04, 0x00, 0x01, 0x3B, 0x7E, 0x03,  // checksum
    0x8E, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,  // IEND chunk
    0x44, 0xAE, 0x42, 0x60, 0x82,                     // IEND CRC
};
// clang-format on

// As kOversizedPng but with the HEIGHT out of range (1 x 1000000): pins the
// dims.height clause of the range check independently of the width clause
// (either clause alone could otherwise be deleted with the suite green).
// libpng's default user height limit is also 1,000,000 inclusive, so the
// header parses cleanly.  IHDR CRC recomputed (0x5EC7FCEB).
// clang-format off
static constexpr uint8_t kOversizedHeightPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk
    0x00, 0x00, 0x00, 0x01, 0x00, 0x0F, 0x42, 0x40,  // width=1, height=1000000
    0x08, 0x02, 0x00, 0x00, 0x00, 0x5E, 0xC7, 0xFC,  // 8bit RGB, recomputed CRC
    0xEB, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,  // IDAT chunk
    0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,  // compressed data
    0x00, 0x00, 0x04, 0x00, 0x01, 0x3B, 0x7E, 0x03,  // checksum
    0x8E, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,  // IEND chunk
    0x44, 0xAE, 0x42, 0x60, 0x82,                     // IEND CRC
};
// clang-format on

// Accept-boundary fixture for the HEIGHT clause: width=1, height=99999
// (0x0001869F) == kMaxPixelDimension.  Symmetric to kBoundaryWidthPng so
// neither clause's '>' can silently become '>='.  IHDR CRC recomputed
// (0x9077FBBB).
// clang-format off
static constexpr uint8_t kBoundaryHeightPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk
    0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x86, 0x9F,  // width=1, height=99999
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0xFB,  // 8bit RGB, recomputed CRC
    0xBB, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,  // IDAT chunk
    0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,  // compressed data
    0x00, 0x00, 0x04, 0x00, 0x01, 0x3B, 0x7E, 0x03,  // checksum
    0x8E, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,  // IEND chunk
    0x44, 0xAE, 0x42, 0x60, 0x82,                     // IEND CRC
};
// clang-format on

// Accept-boundary fixture: width=99999 (0x0001869F) == kMaxPixelDimension,
// height=1.  Pins that EXACTLY the maximum is still accepted on the
// header-inference path -- the author-path boundary tests enforce the range
// via string length in IsValidPixelDimension, which shares no code with the
// numeric comparison in ApplyImageDimensions.  IHDR CRC recomputed
// (0x4BE45837).
// clang-format off
static constexpr uint8_t kBoundaryWidthPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  // IHDR chunk
    0x00, 0x01, 0x86, 0x9F, 0x00, 0x00, 0x00, 0x01,  // width=99999, height=1
    0x08, 0x02, 0x00, 0x00, 0x00, 0x4B, 0xE4, 0x58,  // 8bit RGB, recomputed CRC
    0x37, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,  // IDAT chunk
    0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,  // compressed data
    0x00, 0x00, 0x04, 0x00, 0x01, 0x3B, 0x7E, 0x03,  // checksum
    0x8E, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,  // IEND chunk
    0x44, 0xAE, 0x42, 0x60, 0x82,                     // IEND CRC
};
// clang-format on

// Helper: create a temporary cache, store image data, and return the cache.
class CacheHelper {
 public:
  CacheHelper() {
    cache_dir_ =
        (std::filesystem::temp_directory_path() / "html_transform_cache_test")
            .string();
    std::error_code ec;
    std::filesystem::remove_all(cache_dir_, ec);
    std::filesystem::create_directories(cache_dir_);
    cache_path_ = (std::filesystem::path(cache_dir_) / "test.cache").string();

    PageSpeedCacheConfig config;
    config.volume_path = cache_path_;
    config.volume_size = static_cast<uint64_t>(4 * 1024 * 1024);  // 4MB
    config.ram_cache_size = 0;  // Disable RAM cache
    auto result = PageSpeedCache::Create(config);
    if (result.has_value()) {
      cache_ = std::move(*result);
    }
  }

  ~CacheHelper() {
    // Destroy the cache first to unmap and close the volume file.
    // On Windows, remove_all() cannot delete a memory-mapped file.
    cache_.reset();
    std::error_code ec;
    std::filesystem::remove_all(cache_dir_, ec);
  }

  // Write image data at the default mask (what nginx does on cache miss).
  void WriteImage(std::string_view url, std::string_view hostname,
                  std::span<const uint8_t> image_data) {
    CapabilityMask mask;  // Default = Desktop/Identity = 0x08
    AlternateId id =
        MaskToAlternateId(static_cast<uint8_t>(mask.Encode() & 0xFF));
    AlternateMetadata meta;
    meta.full_mask = mask.Encode();
    meta.content_type = ContentType::kImage;

    auto wh = cache_->WriteAlternate(url, hostname, "https", id,
                                     image_data.size(), meta);
    ASSERT_TRUE(wh.has_value()) << "WriteAlternate failed";
    auto bytes = std::as_bytes(std::span(image_data));
    ASSERT_TRUE(wh->write_sync(bytes).has_value());
    ASSERT_TRUE(wh->close_sync().has_value());
  }

  PageSpeedCache* cache() { return cache_.get(); }
  [[nodiscard]] bool valid() const { return cache_ != nullptr; }

 private:
  std::string cache_dir_;
  std::string cache_path_;
  std::unique_ptr<PageSpeedCache> cache_;
};

TEST(HtmlTransformFilterTest, ImageDimensionsFromCache) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  // Store a valid 1x1 PNG in cache for the image URL.
  std::string image_url = "/images/test.png";
  std::string hostname = "example.com";
  cache_helper.WriteImage(image_url, hostname,
                          std::span<const uint8_t>(kMinimalPng));

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  // HTML with <img> missing width/height.
  std::string result = TransformHtml(
      "<html><body><img src=\"/images/test.png\"></body></html>",
      "http://example.com/", config, "", cache_helper.cache(), hostname);

  // Dimensions should be injected from the cached image (1x1 PNG).
  EXPECT_NE(result.find("width=\"1\""), std::string::npos)
      << "Should inject width from cached image";
  EXPECT_NE(result.find("height=\"1\""), std::string::npos)
      << "Should inject height from cached image";
}

// An image whose header declares an out-of-range dimension must yield NO
// dimensions at all: the same range the filter demands of author-supplied
// width/height governs header-inferred values, and both are skipped together
// because they come from the same distrusted header (skip, never clamp).
TEST(HtmlTransformFilterTest, ImageDimensionsSkipsOutOfRangeHeader) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  std::string image_url = "/images/huge.png";
  std::string hostname = "example.com";
  cache_helper.WriteImage(image_url, hostname,
                          std::span<const uint8_t>(kOversizedPng));

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  std::string result = TransformHtml(
      "<html><body><img src=\"/images/huge.png\"></body></html>",
      "http://example.com/", config, "", cache_helper.cache(), hostname);

  EXPECT_EQ(result.find("width="), std::string::npos)
      << "Must not inject an out-of-range inferred width";
  EXPECT_EQ(result.find("height="), std::string::npos)
      << "Must not inject the paired height either";
}

// Same as above but with the HEIGHT out of range: pins the height clause of
// the range check independently (the width-only fixture would stay green if
// the height comparison were dropped).
TEST(HtmlTransformFilterTest, ImageDimensionsSkipsOutOfRangeHeaderHeight) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  std::string image_url = "/images/tall.png";
  std::string hostname = "example.com";
  cache_helper.WriteImage(image_url, hostname,
                          std::span<const uint8_t>(kOversizedHeightPng));

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  std::string result = TransformHtml(
      "<html><body><img src=\"/images/tall.png\"></body></html>",
      "http://example.com/", config, "", cache_helper.cache(), hostname);

  EXPECT_EQ(result.find("height="), std::string::npos)
      << "Must not inject an out-of-range inferred height";
  EXPECT_EQ(result.find("width="), std::string::npos)
      << "Must not inject the paired width either";
}

// Accept boundary: a header declaring exactly kMaxPixelDimension (99999) is
// in range and must still be emitted -- the range check is exclusive of
// values ABOVE the maximum, not of the maximum itself.
TEST(HtmlTransformFilterTest, ImageDimensionsAcceptsMaxBoundaryHeader) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  std::string image_url = "/images/wide.png";
  std::string hostname = "example.com";
  cache_helper.WriteImage(image_url, hostname,
                          std::span<const uint8_t>(kBoundaryWidthPng));

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  std::string result = TransformHtml(
      "<html><body><img src=\"/images/wide.png\"></body></html>",
      "http://example.com/", config, "", cache_helper.cache(), hostname);

  EXPECT_NE(result.find("width=\"99999\""), std::string::npos)
      << "Exactly kMaxPixelDimension must be accepted";
  EXPECT_NE(result.find("height=\"1\""), std::string::npos)
      << "The paired in-range height must be emitted too";
}

// Height-clause twin of the boundary test above.
TEST(HtmlTransformFilterTest, ImageDimensionsAcceptsMaxBoundaryHeaderHeight) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  std::string image_url = "/images/tallmax.png";
  std::string hostname = "example.com";
  cache_helper.WriteImage(image_url, hostname,
                          std::span<const uint8_t>(kBoundaryHeightPng));

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  std::string result = TransformHtml(
      "<html><body><img src=\"/images/tallmax.png\"></body></html>",
      "http://example.com/", config, "", cache_helper.cache(), hostname);

  EXPECT_NE(result.find("height=\"99999\""), std::string::npos)
      << "Exactly kMaxPixelDimension must be accepted for height";
  EXPECT_NE(result.find("width=\"1\""), std::string::npos)
      << "The paired in-range width must be emitted too";
}

TEST(HtmlTransformFilterTest, ImageDimensionsSkipsExistingBothDimensions) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  // Store a valid PNG in cache.
  std::string image_url = "/images/sized.png";
  std::string hostname = "example.com";
  cache_helper.WriteImage(image_url, hostname,
                          std::span<const uint8_t>(kMinimalPng));

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  // HTML with <img> that already has both width and height.
  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"/images/sized.png\" width=\"200\" height=\"150\">"
      "</body></html>",
      "http://example.com/", config, "", cache_helper.cache(), hostname);

  // Should NOT modify the existing dimensions.
  EXPECT_NE(result.find("width=\"200\""), std::string::npos);
  EXPECT_NE(result.find("height=\"150\""), std::string::npos);
  // Should NOT add duplicate width/height from cache.
  auto img_pos = result.find("sized.png");
  ASSERT_NE(img_pos, std::string::npos);
  auto img_start = result.rfind('<', img_pos);
  auto img_end = result.find('>', img_pos);
  std::string img_tag = result.substr(img_start, img_end - img_start + 1);
  size_t width_count = 0;
  size_t pos = 0;
  while ((pos = img_tag.find("width=", pos)) != std::string::npos) {
    ++width_count;
    pos += 6;
  }
  EXPECT_EQ(width_count, 1u) << "Should not duplicate width attribute";
}

TEST(HtmlTransformFilterTest, ImageDimensionsOnlyMissingWidth) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  std::string image_url = "/images/partial.png";
  std::string hostname = "example.com";
  cache_helper.WriteImage(image_url, hostname,
                          std::span<const uint8_t>(kMinimalPng));

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  // Has height="50" but no width — should add width from cache.
  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"/images/partial.png\" height=\"50\">"
      "</body></html>",
      "http://example.com/", config, "", cache_helper.cache(), hostname);

  // The filter should add width from the cached 1x1 PNG.
  EXPECT_NE(result.find("width=\"1\""), std::string::npos)
      << "Should inject missing width from cache";
  // Original height should be preserved.
  EXPECT_NE(result.find("height=\"50\""), std::string::npos)
      << "Original height should be preserved";
}

TEST(HtmlTransformFilterTest, ImageDimensionsOnlyMissingHeight) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  std::string image_url = "/images/noheight.png";
  std::string hostname = "example.com";
  cache_helper.WriteImage(image_url, hostname,
                          std::span<const uint8_t>(kMinimalPng));

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  // Has width="100" but no height — should add height from cache.
  std::string result = TransformHtml(
      "<html><body>"
      "<img src=\"/images/noheight.png\" width=\"100\">"
      "</body></html>",
      "http://example.com/", config, "", cache_helper.cache(), hostname);

  // The filter should add height from the cached 1x1 PNG.
  EXPECT_NE(result.find("height=\"1\""), std::string::npos)
      << "Should inject missing height from cache";
  // Original width should be preserved.
  EXPECT_NE(result.find("width=\"100\""), std::string::npos)
      << "Original width should be preserved";
}

TEST(HtmlTransformFilterTest, ImageDimensionsCacheMissNoChange) {
  CacheHelper cache_helper;
  ASSERT_TRUE(cache_helper.valid()) << "Cache creation failed";

  // Don't write anything to cache — the image URL will be a cache miss.

  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = true;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;

  std::string result = TransformHtml(
      "<html><body><img src=\"/images/missing.png\"></body></html>",
      "http://example.com/", config, "", cache_helper.cache(), "example.com");

  // Cache miss — no dimensions should be added.
  EXPECT_EQ(result.find("width="), std::string::npos);
  EXPECT_EQ(result.find("height="), std::string::npos);
}

// --- Async CSS tests ---

TEST(HtmlTransformFilterTest, AsyncCssConvertsLinkToNonBlocking) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string result = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Non-render-blocking via the preload pattern: the inlined critical CSS
  // paints first, the full sheet downloads as a normal-priority
  // rel="preload" as="style", and a CSP-safe external loader flips it to a
  // stylesheet once loaded.
  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_NE(result.find("as=\"style\""), std::string::npos);
  EXPECT_NE(result.find("data-pagespeed-async=\"\""), std::string::npos);
  // CSP-safe: no inline onload handler anywhere.
  EXPECT_EQ(result.find("onload="), std::string::npos)
      << "must not use an inline onload handler (blocked by strict CSP)";
  // The external, same-origin loader script is injected. The path is
  // content-addressed (hash embedded), so assert the stable prefix/suffix shape.
  EXPECT_NE(result.find("data-pagespeed-async-loader"), std::string::npos);
  EXPECT_NE(result.find("/pagespeed_static/async_css."), std::string::npos);
  EXPECT_NE(result.find(".js\""), std::string::npos);
  // The old deprioritize-only approach (link stayed render-blocking) is gone.
  EXPECT_EQ(result.find("fetchpriority=\"low\""), std::string::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssAddsNoscriptFallback) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string result = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // A <noscript> fallback preserves the full stylesheet for clients without
  // JavaScript (the loader that flips the preload never runs there).
  ASSERT_EQ(CountOccurrences(result, "<noscript"), 1u);
  size_t ns = result.find("<noscript");
  size_t ns_end = result.find("</noscript>", ns);
  ASSERT_NE(ns_end, std::string::npos);
  std::string_view inside(result.data() + ns, ns_end - ns);
  EXPECT_NE(inside.find("rel=\"stylesheet\""), std::string_view::npos);
  EXPECT_NE(inside.find("href=\"/style.css\""), std::string_view::npos);
  // The fallback link is a plain render-blocking stylesheet, never the
  // deferral primitive.
  EXPECT_EQ(inside.find("rel=\"preload\""), std::string_view::npos);
  EXPECT_EQ(inside.find("as=\"style\""), std::string_view::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssOnlyWhenCriticalCssPresent) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  // No critical CSS → links should NOT be made async (nothing paints the
  // above-the-fold while the sheet loads, so deferring it would flash).
  std::string result = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "");

  EXPECT_EQ(result.find("data-pagespeed-async"), std::string::npos);
  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_EQ(result.find("<noscript"), std::string::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssRecordsOriginalMediaForSwap) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string result = TransformHtml(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\" media=\"screen\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // The link is switched to rel="preload" as="style" (non-blocking); the
  // original media is recorded in data-pagespeed-media so the loader can
  // restore it at the instant it flips the rel back.
  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_NE(result.find("as=\"style\""), std::string::npos);
  EXPECT_NE(result.find("data-pagespeed-media=\"screen\""), std::string::npos);
  EXPECT_EQ(result.find("onload="), std::string::npos);  // CSP-safe loader
  // The <noscript> fallback keeps the original media on the plain stylesheet.
  size_t ns = result.find("<noscript");
  ASSERT_NE(ns, std::string::npos);
  size_t ns_end = result.find("</noscript>", ns);
  ASSERT_NE(ns_end, std::string::npos);
  std::string_view inside(result.data() + ns, ns_end - ns);
  EXPECT_NE(inside.find("media=\"screen\""), std::string_view::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssSkipsNonStylesheetLinks) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string result = TransformHtml(
      "<html><head><link rel=\"preconnect\" href=\"https://cdn.example.com\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Preconnect link should not get async treatment.
  EXPECT_EQ(result.find("data-pagespeed-async"), std::string::npos);
  EXPECT_EQ(result.find("as=\"style\""), std::string::npos);
  EXPECT_EQ(result.find("<noscript"), std::string::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssIdempotency) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  // First pass.
  std::string first = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Second pass on already-transformed output.
  std::string second = TransformHtml(first, "http://example.com/", config,
                                     "body { color: red; }");

  // Revalidation must clean up and re-apply, leaving exactly one of each:
  // one async marker, one preloaded link, one <noscript> fallback.
  EXPECT_NE(second.find("data-pagespeed-async=\"\""), std::string::npos);
  EXPECT_EQ(CountOccurrences(second, "data-pagespeed-async=\"\""), 1u)
      << "exactly one async marker after revalidation";
  EXPECT_EQ(CountOccurrences(second, "as=\"style\""), 1u)
      << "exactly one preloaded stylesheet after revalidation";
  EXPECT_EQ(CountOccurrences(second, "<noscript"), 1u)
      << "exactly one noscript fallback after revalidation";
  EXPECT_EQ(CountOccurrences(second, "data-pagespeed-async-loader"), 1u)
      << "exactly one async-CSS loader script after revalidation";
  // CSP-safe: no inline onload handler, and no legacy deprioritize marker.
  EXPECT_EQ(second.find("onload="), std::string::npos);
  EXPECT_EQ(second.find("fetchpriority=\"low\""), std::string::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssDisabledByConfig) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = false;  // Disabled.

  std::string result = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  EXPECT_EQ(result.find("data-pagespeed-async"), std::string::npos);
  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_EQ(result.find("<noscript"), std::string::npos);
}

// The FOUC sufficiency gate (worker.cc) sets enable_async_css=false while
// keeping enable_critical_css=true when the critical CSS is too thin to bridge
// first paint. This is that transform-level contract: the stylesheet stays
// render-blocking (no preload swap, no loader) so there is no flash, but
// the critical CSS is STILL inlined as a progressive-enhancement hint.
TEST(HtmlTransformFilterTest,
     AsyncSuppressedKeepsLinkBlockingButInlinesCritical) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = false;  // suppressed by the low-coverage gate

  std::string result = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Stylesheet stays render-blocking: original link, no async machinery.
  EXPECT_NE(result.find("rel=\"stylesheet\""), std::string::npos);
  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_EQ(result.find("as=\"style\""), std::string::npos);
  EXPECT_EQ(result.find("data-pagespeed-async"), std::string::npos);
  EXPECT_EQ(result.find("/pagespeed_static/async_css."), std::string::npos);
  // But critical CSS is STILL inlined.
  EXPECT_NE(result.find("color: red"), std::string::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssMultipleStylesheets) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string result = TransformHtml(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/b.css\">"
      "<link rel=\"stylesheet\" href=\"/c.css\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // All three stylesheet links should be made async (preload + noscript).
  EXPECT_EQ(CountOccurrences(result, "rel=\"preload\""), 3u);
  EXPECT_EQ(CountOccurrences(result, "as=\"style\""), 3u);
  EXPECT_EQ(CountOccurrences(result, "data-pagespeed-async=\"\""), 3u);
  EXPECT_EQ(CountOccurrences(result, "<noscript"), 3u);
  // ...but the loader script is injected exactly ONCE per document.
  EXPECT_EQ(CountOccurrences(result, "data-pagespeed-async-loader"), 1u);
  EXPECT_EQ(result.find("onload="), std::string::npos);
}

// The async-CSS loader must be CSP-safe: an external, same-origin, deferred
// <script> — never an inline onload= handler (which strict CSP blocks).
TEST(HtmlTransformFilterTest, AsyncCssLoaderIsCspSafeExternalScript) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string result = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Exactly one loader <script>, external (src=), deferred, marked.
  ASSERT_EQ(CountOccurrences(result, "data-pagespeed-async-loader"), 1u);
  size_t s = result.find("<script");
  ASSERT_NE(s, std::string::npos);
  size_t s_end = result.find('>', s);
  ASSERT_NE(s_end, std::string::npos);
  std::string_view tag(result.data() + s, s_end - s);
  // Content-addressed src: assert the reserved prefix and .js suffix, not the
  // frozen literal (the embedded hash tracks the loader body).
  EXPECT_NE(tag.find("src=\"/pagespeed_static/async_css."),
            std::string_view::npos);
  EXPECT_NE(tag.find(".js\""), std::string_view::npos);
  EXPECT_NE(tag.find("defer"), std::string_view::npos);
  // No executable inline script body and no inline event handler anywhere.
  EXPECT_EQ(result.find("onload="), std::string_view::npos);
  EXPECT_EQ(result.find("onerror="), std::string_view::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssRevalidationPreservesMediaAttribute) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  // First pass: link with media="screen".
  std::string first = TransformHtml(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\" media=\"screen\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // The original media is recorded for the loader's swap and the noscript copy.
  EXPECT_NE(first.find("data-pagespeed-media=\"screen\""), std::string::npos)
      << "First pass should record the original media attribute";
  EXPECT_EQ(CountOccurrences(first, "as=\"style\""), 1u);

  // Second pass (revalidation): the recorded media must survive with no
  // duplication of the link, marker, or noscript fallback.
  std::string second =
      TransformHtml(first, "http://example.com/", config, "body{color:red}");

  EXPECT_NE(second.find("data-pagespeed-media=\"screen\""), std::string::npos)
      << "Revalidation must preserve the recorded original media";
  EXPECT_EQ(CountOccurrences(second, "as=\"style\""), 1u)
      << "exactly one preloaded stylesheet after revalidation";
  EXPECT_EQ(CountOccurrences(second, "data-pagespeed-async=\"\""), 1u);
  EXPECT_EQ(CountOccurrences(second, "<noscript"), 1u);
}

// --- Async CSS hard-case regressions (found via the real-CSS corpus) ---

TEST(HtmlTransformFilterTest, AsyncCssSkipsDisabledStylesheet) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  // A disabled stylesheet is inactive; deferring it would (via the noscript
  // copy, which can't carry the live `disabled` state) wrongly activate it.
  std::string r = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/d.css\" disabled>"
      "</head><body></body></html>",
      "http://example.com/", config, "body{color:red}");
  EXPECT_EQ(r.find("data-pagespeed-async"), std::string::npos);
  EXPECT_EQ(r.find("rel=\"preload\""), std::string::npos);
  EXPECT_EQ(r.find("<noscript"), std::string::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssSkipsPrintOnlyStylesheet) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  // media="print" is already non-render-blocking for screen — left untouched.
  std::string r = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/p.css\" media=\"print\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body{color:red}");
  EXPECT_EQ(r.find("data-pagespeed-async"), std::string::npos);
  EXPECT_EQ(r.find("<noscript"), std::string::npos);
}

TEST(HtmlTransformFilterTest, AsyncCssAllAndEmptyMediaAreIdempotent) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  for (const char* m : {"", "all", "ALL"}) {
    std::string in = std::string(
                         "<html><head><link rel=\"stylesheet\" href=\"/s.css\" "
                         "media=\"") +
                     m + "\"></head><body></body></html>";
    std::string first =
        TransformHtml(in, "http://example.com/", config, "body{color:red}");
    std::string second =
        TransformHtml(first, "http://example.com/", config, "body{color:red}");
    EXPECT_EQ(first, second) << "media=\"" << m << "\" must be a fixed point";
    EXPECT_EQ(CountOccurrences(second, "data-pagespeed-async=\"\""), 1u);
  }
}

TEST(HtmlTransformFilterTest, AsyncCssPreservesSriOnNoscriptFallback) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string r = TransformHtml(
      "<html><head><link rel=\"stylesheet\" "
      "href=\"https://cdn.example.com/x.css\""
      " integrity=\"sha384-abc\" crossorigin=\"anonymous\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "body{color:red}");

  // The live link keeps its SRI...
  EXPECT_NE(r.find("integrity=\"sha384-abc\""), std::string::npos);
  // ...and the <noscript> fallback copy carries integrity + crossorigin too,
  // so no-JS clients still get subresource-integrity protection + CORS.
  size_t ns = r.find("<noscript");
  ASSERT_NE(ns, std::string::npos);
  size_t ns_end = r.find("</noscript>", ns);
  ASSERT_NE(ns_end, std::string::npos);
  std::string_view inside(r.data() + ns, ns_end - ns);
  EXPECT_NE(inside.find("integrity=\"sha384-abc\""), std::string_view::npos);
  EXPECT_NE(inside.find("crossorigin=\"anonymous\""), std::string_view::npos);
}

// A bare, valueless `crossorigin` (equivalent to crossorigin="anonymous") has a
// NULL decoded value. The carry loop must key on attribute PRESENCE, not on a
// non-null value, or the <noscript> fallback drops crossorigin and SRI fails
// for no-JS clients on a cross-origin sheet.
TEST(HtmlTransformFilterTest,
     AsyncCssPreservesBareCrossoriginOnNoscriptFallback) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string r = TransformHtml(
      "<html><head><link rel=\"stylesheet\" "
      "href=\"https://cdn.example.com/x.css\""
      " integrity=\"sha384-abc\" crossorigin></head>"
      "<body></body></html>",
      "http://example.com/", config, "body{color:red}");

  size_t ns = r.find("<noscript");
  ASSERT_NE(ns, std::string::npos);
  size_t ns_end = r.find("</noscript>", ns);
  ASSERT_NE(ns_end, std::string::npos);
  std::string_view inside(r.data() + ns, ns_end - ns);
  EXPECT_NE(inside.find("integrity=\"sha384-abc\""), std::string_view::npos);
  // crossorigin survives onto the fallback as a bare (valueless) attribute,
  // NOT as crossorigin="" (which would be the wrong serialization here).
  EXPECT_NE(inside.find("crossorigin"), std::string_view::npos);
  EXPECT_EQ(inside.find("crossorigin=\"\""), std::string_view::npos);
}

// A fragment with no </head> and no </body> never reaches the critical-CSS
// injection site, so nothing would paint the fold while the sheet loads. That
// is the same guaranteed flash as the CSP case and gets the same answer: the
// conversion is reverted and the stylesheet ships render-blocking.
TEST(HtmlTransformFilterTest, DocumentWithNoHeadOrBodyEndTagDoesNotDefer) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;

  std::string r = TransformHtml(
      "<!doctype html><title>x</title>"
      "<link rel=\"stylesheet\" href=\"/a.css\"><p>hi</p>",
      "http://example.com/", config, "body{color:red}");

  // Precondition: the inline block genuinely never ships here.
  EXPECT_EQ(r.find("data-pagespeed-critical"), std::string::npos)
      << "precondition: no </head> or </body> to inject before: " << r;
  // So neither does the deferral — including the loader, which ApplyAsyncCss
  // co-locates with the first deferred link precisely to survive this shape.
  EXPECT_EQ(r.find("data-pagespeed-async"), std::string::npos) << r;
  EXPECT_EQ(CountOccurrences(r, "data-pagespeed-async-loader"), 0u) << r;
  EXPECT_EQ(r.find("<noscript"), std::string::npos) << r;
  // The revert must put the link back to a STYLESHEET. Leaving it at
  // rel="preload" with the loader deleted would download the sheet and then
  // apply nothing — a permanently unstyled page, worse than the flash the
  // revert exists to prevent.
  EXPECT_NE(r.find("rel=\"stylesheet\""), std::string::npos) << r;
  EXPECT_EQ(r.find("rel=\"preload\""), std::string::npos) << r;
  EXPECT_EQ(r.find("as=\"style\""), std::string::npos) << r;
}

// --- The deferral primitive: rel="preload" as="style" ---

// Returns a config with only critical CSS + async CSS enabled, so the async
// assertions below are not perturbed by the other transforms.
HtmlTransformConfig AsyncCssOnlyConfig() {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_async_css = true;
  return config;
}

// The primitive that stops a stylesheet blocking first paint must be a
// PRELOAD. A deferred sheet is still a sheet the page needs promptly: as a
// preload it is fetched at the priority a stylesheet gets and it stays
// eligible for Early-Hints promotion, neither of which a low-priority media
// technique can offer.
TEST(HtmlTransformFilterTest, DeferredStylesheetUsesPreloadAsStyle) {
  HtmlTransformConfig config = AsyncCssOnlyConfig();

  std::string result = TransformHtml(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\" media=\"screen\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Everything before the <noscript> twin is the LIVE half of the pattern:
  // the deferred link itself plus the loader. Asserting on this slice rather
  // than the whole document is what keeps the twin's rel="stylesheet" from
  // satisfying an assertion meant for the live link.
  size_t ns = result.find("<noscript");
  ASSERT_NE(ns, std::string::npos) << result;
  std::string_view live(result.data(), ns);

  EXPECT_NE(live.find("rel=\"preload\""), std::string_view::npos) << result;
  EXPECT_NE(live.find("as=\"style\""), std::string_view::npos) << result;
  // The live link is NOT a stylesheet until the loader flips it — that is the
  // whole reason it does not block.
  EXPECT_EQ(live.find("rel=\"stylesheet\""), std::string_view::npos) << result;
  // The author's media is parked, not applied: on a preload, media is a fetch
  // CONDITION, so leaving media="screen" on it would gate the very download
  // this is trying to promote.
  EXPECT_NE(live.find("data-pagespeed-media=\"screen\""),
            std::string_view::npos)
      << result;
  // A LIVE media attribute is preceded by a space; data-pagespeed-media is
  // preceded by a '-', so this distinguishes them (a bare "media=" search
  // would match the recorded copy and pass vacuously).
  EXPECT_EQ(live.find(" media="), std::string_view::npos) << result;
  EXPECT_NE(live.find("data-pagespeed-async=\"\""), std::string_view::npos)
      << result;

  // The demoting primitive is gone from the document entirely.
  EXPECT_EQ(result.find("media=\"print\""), std::string::npos) << result;

  // Single fetch: Chrome matches a preload to its consumer on `as`, and the
  // consumer here is this SAME element after the loader's rel flip. A second
  // live <link> for the same href would be a second download.
  EXPECT_EQ(CountOccurrences(live, "href=\"/style.css\""), 1u) << result;
  EXPECT_EQ(CountOccurrences(result, "as=\"style\""), 1u) << result;
  EXPECT_EQ(CountOccurrences(live, "<link"), 1u) << result;

  // And the loader that performs the flip actually ships.
  EXPECT_NE(result.find("data-pagespeed-async-loader"), std::string::npos);
  EXPECT_EQ(result.find("onload="), std::string::npos)
      << "must not use an inline onload handler (blocked by strict CSP)";
}

// The <noscript> twin is the no-JS client's ONLY stylesheet — nothing will
// ever flip a rel for them. It must stay a plain render-blocking stylesheet
// carrying every load-affecting attribute, exactly as before the switch.
TEST(HtmlTransformFilterTest, NoscriptFallbackStillCarriesRelStylesheetAndSri) {
  HtmlTransformConfig config = AsyncCssOnlyConfig();

  std::string r = TransformHtml(
      "<html><head><link rel=\"stylesheet\" "
      "href=\"https://cdn.example.com/x.css\" media=\"screen\" "
      "integrity=\"sha384-abc\" crossorigin referrerpolicy=\"no-referrer\" "
      "type=\"text/css\" title=\"main\"></head><body></body></html>",
      "http://example.com/", config, "body{color:red}");

  size_t ns = r.find("<noscript");
  ASSERT_NE(ns, std::string::npos) << r;
  size_t ns_end = r.find("</noscript>", ns);
  ASSERT_NE(ns_end, std::string::npos) << r;
  std::string_view inside(r.data() + ns, ns_end - ns);

  EXPECT_NE(inside.find("rel=\"stylesheet\""), std::string_view::npos) << r;
  EXPECT_EQ(inside.find("rel=\"preload\""), std::string_view::npos) << r;
  EXPECT_EQ(inside.find("as=\"style\""), std::string_view::npos) << r;
  EXPECT_NE(inside.find("href=\"https://cdn.example.com/x.css\""),
            std::string_view::npos)
      << r;
  // The twin keeps the REAL media, since it is the sheet that applies.
  EXPECT_NE(inside.find("media=\"screen\""), std::string_view::npos) << r;

  // kCarryAttrs, untouched by the primitive switch.
  EXPECT_NE(inside.find("integrity=\"sha384-abc\""), std::string_view::npos)
      << r;
  EXPECT_NE(inside.find("referrerpolicy=\"no-referrer\""),
            std::string_view::npos)
      << r;
  EXPECT_NE(inside.find("type=\"text/css\""), std::string_view::npos) << r;
  EXPECT_NE(inside.find("title=\"main\""), std::string_view::npos) << r;
  // A bare, valueless `crossorigin` stays bare: crossorigin="" is a different
  // thing, and SRI on a cross-origin sheet depends on getting this right.
  EXPECT_NE(inside.find("crossorigin"), std::string_view::npos) << r;
  EXPECT_EQ(inside.find("crossorigin=\"\""), std::string_view::npos) << r;
}

// Revalidation re-runs the transform over our OWN output. Two properties have
// to hold together: re-applying changes nothing (fixed point), and stripping
// the markers hands the customer back the markup they wrote — rel restored,
// `as` gone. The second is the load-bearing one: a link left at rel="preload"
// once the loader has been removed downloads the sheet and applies nothing.
TEST(HtmlTransformFilterTest, RevalidationRoundTripsThePreloadPrimitive) {
  HtmlTransformConfig config = AsyncCssOnlyConfig();

  const char* kSource =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\" media=\"screen\">"
      "</head><body></body></html>";

  std::string first =
      TransformHtml(kSource, "http://example.com/", config, "body{color:red}");
  ASSERT_NE(first.find("rel=\"preload\""), std::string::npos) << first;

  std::string second =
      TransformHtml(first, "http://example.com/", config, "body{color:red}");
  EXPECT_EQ(first, second) << "re-applying must be a fixed point:\n" << second;
  EXPECT_EQ(CountOccurrences(second, "as=\"style\""), 1u) << second;
  EXPECT_EQ(CountOccurrences(second, "rel=\"preload\""), 1u) << second;
  EXPECT_EQ(CountOccurrences(second, "data-pagespeed-async=\"\""), 1u)
      << second;
  EXPECT_EQ(CountOccurrences(second, "data-pagespeed-async-loader"), 1u)
      << second;
  EXPECT_EQ(CountOccurrences(second, "<noscript"), 1u) << second;

  // Marker removal is deliberately NOT gated on the async-CSS switch, so
  // turning the feature off is the cleanest way to observe the un-transform.
  HtmlTransformConfig off = AsyncCssOnlyConfig();
  off.enable_async_css = false;
  std::string reverted =
      TransformHtml(first, "http://example.com/", off, "body{color:red}");

  EXPECT_NE(reverted.find("rel=\"stylesheet\""), std::string::npos)
      << "the deferred link must go back to being a stylesheet:\n"
      << reverted;
  EXPECT_NE(reverted.find("media=\"screen\""), std::string::npos) << reverted;
  EXPECT_EQ(reverted.find("rel=\"preload\""), std::string::npos) << reverted;
  EXPECT_EQ(reverted.find("as=\"style\""), std::string::npos) << reverted;
  EXPECT_EQ(reverted.find("data-pagespeed-async"), std::string::npos)
      << reverted;
  EXPECT_EQ(reverted.find("data-pagespeed-media"), std::string::npos)
      << reverted;
  EXPECT_EQ(reverted.find("<noscript"), std::string::npos) << reverted;
  EXPECT_EQ(reverted.find("/pagespeed_static/async_css."), std::string::npos)
      << reverted;
  EXPECT_EQ(CountOccurrences(reverted, "<link"), 1u)
      << "exactly the author's one link is left:\n"
      << reverted;
}

// Neither early-out moves with the primitive: a media="print" sheet is already
// non-render-blocking for screen, and a disabled sheet is inactive (its
// <noscript> twin cannot carry the live `disabled` state, so deferring would
// wrongly ACTIVATE it for no-JS clients).
TEST(HtmlTransformFilterTest, PrintSheetAndDisabledSheetStillSkipped) {
  HtmlTransformConfig config = AsyncCssOnlyConfig();

  for (const char* link :
       {"<link rel=\"stylesheet\" href=\"/p.css\" media=\"print\">",
        "<link rel=\"stylesheet\" href=\"/d.css\" disabled>"}) {
    std::string r = TransformHtml(
        std::string("<html><head>") + link + "</head><body></body></html>",
        "http://example.com/", config, "body{color:red}");

    EXPECT_NE(r.find("rel=\"stylesheet\""), std::string::npos) << link << r;
    EXPECT_EQ(r.find("rel=\"preload\""), std::string::npos) << link << r;
    EXPECT_EQ(r.find("as=\"style\""), std::string::npos) << link << r;
    EXPECT_EQ(r.find("data-pagespeed-async"), std::string::npos) << link << r;
    EXPECT_EQ(r.find("<noscript"), std::string::npos) << link << r;
    EXPECT_EQ(r.find("/pagespeed_static/async_css."), std::string::npos)
        << link << r;
    EXPECT_EQ(CountOccurrences(r, "<link"), 1u) << link << r;
  }
}

// --- Script Deferral tests ---

TEST(HtmlTransformFilterTest, ScriptDeferralAddsDefer) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_script_deferral = true;

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<script src=\"https://cdn.example.com/analytics.js\"></script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {},
      {"analytics.js"});

  EXPECT_NE(result.find("defer"), std::string::npos);
  EXPECT_NE(result.find("data-pagespeed-defer"), std::string::npos);
}

TEST(HtmlTransformFilterTest, ScriptDeferralSkipsAsyncScripts) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_script_deferral = true;

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<script async src=\"https://cdn.example.com/analytics.js\"></script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {},
      {"analytics.js"});

  // Already async — should not add defer.
  EXPECT_EQ(result.find("data-pagespeed-defer"), std::string::npos);
}

TEST(HtmlTransformFilterTest, ScriptDeferralSkipsInlineScripts) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_script_deferral = true;

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<script>console.log('hello');</script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {},
      {"analytics.js"});

  // Inline scripts (no src) should not be deferred.
  EXPECT_EQ(result.find("data-pagespeed-defer"), std::string::npos);
}

TEST(HtmlTransformFilterTest, ScriptDeferralUrlBoundaryMatching) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_script_deferral = true;

  // Pattern "s.js" should NOT match "analytics.js" (no boundary).
  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<script src=\"https://cdn.example.com/analytics.js\"></script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {}, {"s.js"});

  EXPECT_EQ(result.find("data-pagespeed-defer"), std::string::npos);

  // But "analytics.js" should match (preceded by /).
  std::string result2 = TransformHtml(
      "<html><head></head><body>"
      "<script src=\"https://cdn.example.com/analytics.js\"></script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {},
      {"analytics.js"});

  EXPECT_NE(result2.find("data-pagespeed-defer"), std::string::npos);
}

TEST(HtmlTransformFilterTest, ScriptDeferralIdempotency) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_script_deferral = true;

  std::string first = TransformHtml(
      "<html><head></head><body>"
      "<script src=\"https://cdn.example.com/analytics.js\"></script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {},
      {"analytics.js"});

  EXPECT_NE(first.find("data-pagespeed-defer"), std::string::npos);

  // Second pass — should not double-apply.
  std::string second = TransformHtml(first, "http://example.com/", config, "",
                                     nullptr, "", {}, {}, {}, {"analytics.js"});

  // Still has defer marker.
  EXPECT_NE(second.find("data-pagespeed-defer"), std::string::npos);
}

TEST(HtmlTransformFilterTest, ScriptDeferralRevalidation) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_script_deferral = true;

  // Pre-marked HTML from a previous pass.
  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<script src=\"https://cdn.example.com/analytics.js\" "
      "defer data-pagespeed-defer></script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {},
      {"analytics.js"});

  // Should strip and re-apply cleanly.
  EXPECT_NE(result.find("data-pagespeed-defer"), std::string::npos);
  EXPECT_NE(result.find("defer"), std::string::npos);
}

TEST(HtmlTransformFilterTest, ScriptDeferralDisabledByConfig) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_script_deferral = false;  // Disabled.

  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<script src=\"https://cdn.example.com/analytics.js\"></script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {},
      {"analytics.js"});

  EXPECT_EQ(result.find("data-pagespeed-defer"), std::string::npos);
}

TEST(HtmlTransformFilterTest, ScriptDeferralEmptyList) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_script_deferral = true;

  // Empty defer list — no scripts should be deferred.
  std::string result = TransformHtml(
      "<html><head></head><body>"
      "<script src=\"https://cdn.example.com/analytics.js\"></script>"
      "</body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {}, {});

  EXPECT_EQ(result.find("data-pagespeed-defer"), std::string::npos);
}

// ========== Font Preload Injection Tests ==========

TEST(HtmlTransformFilterTest, InjectsFontPreloadBeforeHead) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_font_preload = true;

  std::vector<std::string> font_urls = {"/fonts/inter.woff2"};

  std::string result = TransformHtml(
      "<html><head><title>Test</title></head><body></body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {}, {},
      font_urls);

  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_NE(result.find("as=\"font\""), std::string::npos);
  EXPECT_NE(result.find("type=\"font/woff2\""), std::string::npos);
  EXPECT_NE(result.find("crossorigin"), std::string::npos);
  EXPECT_NE(result.find("href=\"/fonts/inter.woff2\""), std::string::npos);
  EXPECT_NE(result.find("data-pagespeed-hint"), std::string::npos);
  // Should appear before </head>
  auto link_pos = result.find("as=\"font\"");
  auto head_end = result.find("</head>");
  EXPECT_LT(link_pos, head_end);
}

TEST(HtmlTransformFilterTest, InjectsMultipleFontPreloads) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_font_preload = true;

  std::vector<std::string> font_urls = {"/fonts/inter-regular.woff2",
                                        "/fonts/inter-bold.woff2"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, {}, {}, font_urls);

  EXPECT_EQ(CountOccurrences(result, "as=\"font\""), 2u);
  EXPECT_NE(result.find("inter-regular.woff2"), std::string::npos);
  EXPECT_NE(result.find("inter-bold.woff2"), std::string::npos);
}

TEST(HtmlTransformFilterTest, RejectsDataUrlInFontPreload) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_font_preload = true;

  std::vector<std::string> font_urls = {"data:font/woff2;base64,AAAA",
                                        "/fonts/inter.woff2"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, {}, {}, font_urls);

  // data: URL should be rejected, only the valid font should appear
  EXPECT_EQ(CountOccurrences(result, "as=\"font\""), 1u);
  EXPECT_NE(result.find("inter.woff2"), std::string::npos);
  EXPECT_EQ(result.find("data:font"), std::string::npos);
}

TEST(HtmlTransformFilterTest, FontPreloadNotInjectedWhenDisabled) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_font_preload = false;  // disabled

  std::vector<std::string> font_urls = {"/fonts/inter.woff2"};

  std::string result = TransformHtml("<html><head></head><body></body></html>",
                                     "http://example.com/", config, "", nullptr,
                                     "", {}, {}, {}, {}, font_urls);

  EXPECT_EQ(result.find("as=\"font\""), std::string::npos);
}

TEST(HtmlTransformFilterTest, FontPreloadRevalidationCleanup) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_font_preload = true;

  std::vector<std::string> font_urls = {"/fonts/inter.woff2"};

  // Simulate revalidation: input already has a pagespeed font preload link
  std::string result = TransformHtml(
      "<html><head>"
      "<link rel=\"preload\" as=\"font\" href=\"/fonts/old.woff2\" "
      "data-pagespeed-hint=\"\">"
      "</head><body></body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, {}, {},
      font_urls);

  // Old hint should be removed, new one injected
  EXPECT_EQ(result.find("old.woff2"), std::string::npos);
  EXPECT_NE(result.find("inter.woff2"), std::string::npos);
}

// ========== Meta-CSP gating of inline injections (2.S9) ==========

TEST(HtmlTransformFilterTest, RestrictiveMetaCspBlocksCriticalCss) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self'\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Inline critical CSS would be dropped by the CSP, so it must NOT be injected.
  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos);
}

TEST(HtmlTransformFilterTest, PermissiveMetaCspAllowsCriticalCss) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self' 'unsafe-inline'\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos);
}

TEST(HtmlTransformFilterTest, NoMetaCspAllowsCriticalCss) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head><title>Test</title></head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos);
}

TEST(HtmlTransformFilterTest, ReportOnlyMetaCspDoesNotBlockCriticalCss) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  // Report-Only never blocks rendering, so it must NOT gate our injection.
  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy-Report-Only\" "
      "content=\"style-src 'self'\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos);
}

TEST(HtmlTransformFilterTest, MultipleMetaCspsCombineRestrictively) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  // First policy permits inline, second forbids it. Restrictive combination
  // means inline must be blocked.
  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'unsafe-inline'\">"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self'\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos);
}

TEST(HtmlTransformFilterTest, RestrictiveMetaCspBlocksAsyncCssDeferral) {
  // FOUC-coupling regression: under a restrictive style CSP the inline critical
  // CSS is suppressed, so the origin stylesheet must NOT be converted to async
  // (that would leave the page unstyled until the loader flips the rel).
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_async_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self'\">"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Stylesheet stays render-blocking: not deferred, no async markers.
  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_EQ(result.find("as=\"style\""), std::string::npos);
  EXPECT_EQ(result.find("data-pagespeed-async"), std::string::npos);
  // And no inline critical CSS.
  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos);
}

TEST(HtmlTransformFilterTest, PermissiveMetaCspAllowsAsyncCssDeferral) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_async_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self' 'unsafe-inline'\">"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // Stylesheet IS deferred and critical CSS IS injected.
  EXPECT_NE(result.find("rel=\"preload\""), std::string::npos);
  EXPECT_NE(result.find("as=\"style\""), std::string::npos);
  EXPECT_NE(result.find("data-pagespeed-async"), std::string::npos);
  EXPECT_NE(result.find("data-pagespeed-critical"), std::string::npos);
}

// Source-order hole: the <link> is converted at StartElement, BEFORE the CSP
// <meta> that governs the inline <style> injected at </head>. Gating the
// conversion on the metas seen SO FAR therefore misses this ordering, and the
// page ships a deferred sheet with no inline block to bridge it — the exact
// FOUC the CSP gate exists to prevent. The deferral must not survive the pass.
TEST(HtmlTransformFilterTest,
     LinkBeforeCspMetaIsNotDeferredWhenInlineStyleWillBeSuppressed) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_async_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\">"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self'\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body { color: red; }");

  // The CSP suppresses the inline block (this half already worked).
  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos);
  // So nothing may be left deferred: no primitive, no marker, no twin, no
  // loader.
  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos)
      << "sheet deferred with no inline block to bridge it: " << result;
  EXPECT_EQ(result.find("as=\"style\""), std::string::npos) << result;
  EXPECT_EQ(result.find("data-pagespeed-async"), std::string::npos);
  EXPECT_EQ(result.find("<noscript"), std::string::npos);
  EXPECT_EQ(result.find("/pagespeed_static/async_css."), std::string::npos);
  // The original render-blocking stylesheet survives, unmarked.
  EXPECT_NE(result.find("rel=\"stylesheet\""), std::string::npos);
  EXPECT_EQ(CountOccurrences(result, "<link"), 1u)
      << "reverting must leave exactly the original link: " << result;
}

// The revert must restore the link EXACTLY, so a page whose CSP suppresses the
// inline block converges instead of churning its media attribute across
// revalidation passes.
TEST(HtmlTransformFilterTest, RevertedDeferralRestoresTheOriginalMedia) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_async_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  const char* html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/style.css\" media=\"screen\">"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self'\">"
      "</head><body></body></html>";

  std::string first =
      TransformHtml(html, "http://example.com/", config, "body{color:red}");
  EXPECT_NE(first.find("media=\"screen\""), std::string::npos)
      << "the original media must come back: " << first;
  EXPECT_EQ(first.find("data-pagespeed-media"), std::string::npos)
      << "the revert must not leave the swap bookkeeping behind: " << first;

  std::string second =
      TransformHtml(first, "http://example.com/", config, "body{color:red}");
  EXPECT_EQ(first, second) << "a reverted page must be a fixed point";
}

// Multiple sheets before the meta: every one of them reverts, not just the
// first (the loader is injected once, with the first, and must go too).
TEST(HtmlTransformFilterTest, AllLinksRevertWhenTheInlineBlockIsSuppressed) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_async_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/b.css\">"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self'\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body{color:red}");

  EXPECT_EQ(CountOccurrences(result, "data-pagespeed-async"), 0u) << result;
  EXPECT_EQ(CountOccurrences(result, "as=\"style\""), 0u) << result;
  EXPECT_EQ(CountOccurrences(result, "rel=\"stylesheet\""), 2u) << result;
  EXPECT_EQ(CountOccurrences(result, "<noscript"), 0u) << result;
  EXPECT_EQ(CountOccurrences(result, "<link"), 2u) << result;
}

// The CSP is not the only way the inline block can fail to ship: critical CSS
// containing a `</style` terminator aborts the injection outright (XSS guard).
// Same coupling, same remedy — the deferral must not ship without it.
TEST(HtmlTransformFilterTest,
     CriticalCssRejectedByTheXssGuardAlsoRevertsTheDeferral) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_async_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head><link rel=\"stylesheet\" href=\"/style.css\"></head>"
      "<body></body></html>",
      "http://example.com/", config, "body{content:\"</style>\"}");

  EXPECT_EQ(result.find("data-pagespeed-critical"), std::string::npos)
      << "precondition: the XSS guard must refuse this critical block";
  EXPECT_EQ(result.find("data-pagespeed-async"), std::string::npos) << result;
  EXPECT_EQ(result.find("rel=\"preload\""), std::string::npos) << result;
  EXPECT_EQ(result.find("as=\"style\""), std::string::npos) << result;
  EXPECT_NE(result.find("rel=\"stylesheet\""), std::string::npos) << result;
  EXPECT_EQ(result.find("/pagespeed_static/async_css."), std::string::npos)
      << result;
}

// The control: with the meta FIRST (the ordering that already worked) nothing
// is ever converted, so the revert path is not what produces the safe result.
TEST(HtmlTransformFilterTest, CspBeforeLinkStillNeverConvertsInTheFirstPlace) {
  HtmlTransformConfig config;
  config.enable_critical_css = true;
  config.enable_async_css = true;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;

  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"style-src 'self'\">"
      "<link rel=\"stylesheet\" href=\"/style.css\" media=\"screen\">"
      "</head><body></body></html>",
      "http://example.com/", config, "body{color:red}");

  EXPECT_NE(result.find("media=\"screen\""), std::string::npos);
  EXPECT_EQ(result.find("data-pagespeed-async"), std::string::npos);
}

TEST(HtmlTransformFilterTest, RestrictiveMetaCspBlocksSpeculationRules) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_speculation_rules = true;

  std::vector<std::string> spec_urls = {"https://example.com/page1"};

  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src 'self'\">"
      "</head><body><p>Content</p></body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, spec_urls);

  EXPECT_EQ(result.find("speculationrules"), std::string::npos);
}

TEST(HtmlTransformFilterTest, PermissiveMetaCspAllowsSpeculationRules) {
  HtmlTransformConfig config;
  config.enable_critical_css = false;
  config.enable_lazy_load = false;
  config.enable_image_dimensions = false;
  config.enable_lcp_preload = false;
  config.enable_preconnect_injection = false;
  config.enable_speculation_rules = true;

  std::vector<std::string> spec_urls = {"https://example.com/page1"};

  std::string result = TransformHtml(
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" "
      "content=\"script-src 'self' 'unsafe-inline'\">"
      "</head><body><p>Content</p></body></html>",
      "http://example.com/", config, "", nullptr, "", {}, {}, spec_urls);

  EXPECT_NE(result.find("speculationrules"), std::string::npos);
}

}  // namespace
}  // namespace pagespeed
