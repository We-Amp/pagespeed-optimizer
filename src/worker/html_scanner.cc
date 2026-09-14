// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTML Scanner Implementation

#include "src/worker/html_scanner.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "lib/base/string_util.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/empty_html_filter.h"
#include "lib/html/html_element.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_name.h"
#include "lib/html/html_node.h"
#include "lib/html/html_parse.h"

namespace pagespeed {

namespace {

// Extract the origin (scheme + host) from a URL string.
// Returns empty string if the URL is not absolute (no scheme://host).
std::string ExtractOrigin(std::string_view url) {
  // Must start with http:// or https:// or //
  size_t scheme_end;
  if (url.starts_with("https://")) {
    scheme_end = 8;
  } else if (url.starts_with("http://")) {
    scheme_end = 7;
  } else if (url.starts_with("//")) {
    scheme_end = 2;
  } else {
    return "";
  }
  // Find the end of the host (next / or end of string).
  size_t host_end = url.find('/', scheme_end);
  if (host_end == std::string_view::npos) {
    return std::string(url);
  }
  return std::string(url.substr(0, host_end));
}

// Extract the origin from a page URL for same-origin comparison.
std::string ExtractPageOrigin(std::string_view page_url) {
  return ExtractOrigin(page_url);
}

// Priority bucket for preconnect origins: lower number = higher priority.
// 0 = render-blocking (stylesheets, sync scripts)
// 1 = other head resources (preload, etc.)
// 2 = body resources (images, async scripts)
constexpr int kPriorityRenderBlocking = 0;
constexpr int kPriorityHeadResource = 1;
constexpr int kPriorityBodyResource = 2;

// Case-insensitive whole-value match for simple attribute values.
bool AttrValueEquals(const char* value, std::string_view expected) {
  return value != nullptr && net_instaweb::StringCaseEqual(value, expected);
}

// True when the element's direct parent is a <picture> element.  <picture>
// is not an interned HtmlName keyword, so compare the tag name string.
bool ParentIsPicture(const net_instaweb::HtmlElement* element) {
  const net_instaweb::HtmlElement* parent = element->parent();
  return parent != nullptr &&
         net_instaweb::StringCaseEqual(parent->name_str(), "picture");
}

// Internal filter that collects element information during parsing.
// This extends EmptyHtmlFilter to track elements as they are encountered.
class ElementCollectorFilter : public net_instaweb::EmptyHtmlFilter {
 public:
  explicit ElementCollectorFilter(HtmlScanResult* result) : result_(result) {}

  [[nodiscard]] const char* Name() const override { return "ElementCollector"; }

  void set_page_url(std::string_view url) {
    page_origin_ = ExtractPageOrigin(url);
  }

  void StartDocument() override {
    depth_ = 0;
    element_index_ = 0;
    in_style_ = false;
    in_body_ = false;
    hero_candidate_found_ = false;
    body_img_count_ = 0;
    ancestor_is_hero_.clear();
    seen_origins_.clear();
    origin_entries_.clear();
  }

  void StartElement(net_instaweb::HtmlElement* element) override {
    net_instaweb::HtmlName::Keyword keyword = element->keyword();

    // The worker caches its OWN optimized output and re-scans THAT on
    // revalidation passes (the cache slot flips raw-origin <-> reprocessed).
    // The reprocessed variant carries nodes the raw-origin HTML never had — the
    // injected critical <style data-pagespeed-critical>, the async <noscript
    // data-pagespeed-async-fallback> + its child <link>, the async-loader
    // <script data-pagespeed-async-loader>, and preconnect/preload <link/script
    // data-pagespeed-hint>. TemplateDetector::HashStructure hashes the whole
    // element tree and CriticalCssExtractor keys criticality off element_index,
    // so collecting these injected nodes makes the scan differ between the raw
    // and reprocessed passes: the template hash flaps (a redundant browser
    // analysis + a wasted profile slot) and the index cutoff shifts. Excluding
    // them keeps result_->elements a fixed point.
    //
    // Only markers the worker puts on its OWN injected nodes belong here.
    // data-pagespeed-async / data-pagespeed-media (on the deferred primary
    // <link>) and data-pagespeed-defer (on a real <script>) sit on ORIGINAL
    // customer elements that ARE present in the raw scan, so they are
    // deliberately excluded from this set — skipping them would flap the hash
    // in the other direction. data-pagespeed-inlined (css_cache_inliner's
    // <style>) is likewise omitted on purpose: that inlined copy lives only in
    // the transient offline browser-analysis input, never in the cached/served
    // bytes the worker re-scans, so it cannot reach this scan. Bookkeeping below
    // (depth_, ancestor_is_hero_, stylesheet/SRI/origin collection) still runs
    // for injected nodes so the EndElement balance and the stylesheet/origin
    // lists stay correct.
    const bool worker_injected =
        element->FindAttribute("data-pagespeed-critical") != nullptr ||
        element->FindAttribute("data-pagespeed-hint") != nullptr ||
        element->FindAttribute("data-pagespeed-async-fallback") != nullptr ||
        element->FindAttribute("data-pagespeed-async-loader") != nullptr;

    // The customer's own <link>, async-deferred in place: it ships as
    // rel="preload" as="style" with its media parked in data-pagespeed-media.
    // Every place below that would recognize the raw <link rel="stylesheet">
    // has to recognize this too, or the scan of a reprocessed variant differs
    // from the scan of the raw origin and the derived state (stylesheet list,
    // template hash, preconnect priorities) flaps between passes.
    const bool deferred_primary =
        element->FindAttribute("data-pagespeed-async") != nullptr;

    // Collect element information
    CollectedElement collected;
    collected.tag_name = std::string(element->name_str());
    collected.depth = depth_;
    // Advance element_index_ only for collected (non-injected) elements so a
    // real element keeps the SAME index across raw and reprocessed passes;
    // otherwise CriticalCssExtractor's "first N elements" cutoff
    // (element_index < max_elements) would not be a fixed point.
    if (!worker_injected) {
      collected.element_index = element_index_++;
    }

    // Extract id attribute
    const char* id_value = element->AttributeValue(net_instaweb::HtmlName::kId);
    if (id_value != nullptr) {
      collected.id = id_value;
    }

    // Extract class attribute and split into individual classes
    const char* class_value =
        element->AttributeValue(net_instaweb::HtmlName::kClass);
    if (class_value != nullptr) {
      std::vector<std::string_view> class_parts = absl::StrSplit(
          class_value, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty());
      for (const auto& part : class_parts) {
        collected.classes.push_back(std::string(part));
      }
    }

    if (!worker_injected && result_->elements.size() < 100000) {
      result_->elements.emplace_back(std::move(collected));
    }

    // Check for stylesheet links
    if (keyword == net_instaweb::HtmlName::kLink) {
      const char* rel = element->AttributeValue(net_instaweb::HtmlName::kRel);
      // Skip the worker's own async-CSS <noscript> fallback copy: it duplicates
      // the deferred primary <link> (same href). On a revalidation re-scan
      // of our own optimized output, collecting it as a second origin sheet
      // doubles the preload Early-Hints (worker.cc) and combined_css gather, and
      // flaps the num_stylesheets component of the template hash. Keeping only
      // the primary stabilizes the stylesheet LIST across raw-vs-reprocessed
      // passes; mirrors the data-pagespeed-critical <style> skip below. The
      // element-tree component of the template hash is stabilized separately, at
      // the top of StartElement, by excluding all worker-injected nodes from
      // result_->elements (the fallback link is one of them).
      //
      // A deferred primary is rel="preload" as="style", NOT rel="stylesheet" —
      // on a re-scan of our own output it is still the page's origin
      // stylesheet, so match it too or the reprocessed pass would see zero
      // stylesheets (the fallback copy being skipped just below): the template
      // hash would flap 1<->0 and the sheet would lose its Early-Hints entry.
      // Keyed on data-pagespeed-async so ordinary preloads (LCP image, fonts)
      // are not mistaken for stylesheets.
      const bool is_stylesheet =
          rel != nullptr &&
          (std::string_view(rel) == "stylesheet" ||
           (std::string_view(rel) == "preload" && deferred_primary));
      if (is_stylesheet &&
          element->FindAttribute("data-pagespeed-async-fallback") == nullptr) {
        StylesheetLink link;
        const char* href =
            element->AttributeValue(net_instaweb::HtmlName::kHref);
        if (href != nullptr) {
          link.href = href;
        }
        // For a deferred primary the author's media lives in
        // data-pagespeed-media; the live `media` attribute was removed (on a
        // preload it is a fetch condition, not the sheet's applicability).
        // Reporting the RECORDED media is what keeps media-dependent decisions
        // — chiefly StylesheetMediaIsPrint in the Early-Hints emitters — giving
        // the same answer on the raw and reprocessed passes. One value is
        // NORMALIZED rather than round-tripped: an author <link> with no media
        // scans as "" raw and as "all" reprocessed, because ApplyAsyncCss
        // records the default explicitly. Every media-keyed decision treats the
        // two identically (neither is "print"), so the decisions match; only
        // the string differs, and nothing downstream compares it for equality.
        const net_instaweb::HtmlElement::Attribute* media_attr =
            deferred_primary
                ? element->FindAttribute("data-pagespeed-media")
                : element->FindAttribute(net_instaweb::HtmlName::kMedia);
        const char* media = (media_attr != nullptr)
                                ? media_attr->DecodedValueOrNull()
                                : nullptr;
        if (media != nullptr) {
          link.media = media;
        }
        result_->stylesheets.push_back(std::move(link));
      }
    }

    // SRI: collect URLs referenced with an integrity attribute (browsers
    // enforce the hash on <script> and <link> loads, so an optimized
    // variant served at the same URL would be refused).  Presence-based:
    // pinning on a malformed/empty integrity value only skips an
    // optimization, never breaks a page.
    if ((keyword == net_instaweb::HtmlName::kLink ||
         keyword == net_instaweb::HtmlName::kScript) &&
        element->FindAttribute(net_instaweb::HtmlName::kIntegrity) != nullptr &&
        result_->integrity_pinned_urls.size() < 1000) {
      const char* pinned_url =
          element->AttributeValue(keyword == net_instaweb::HtmlName::kLink
                                      ? net_instaweb::HtmlName::kHref
                                      : net_instaweb::HtmlName::kSrc);
      if (pinned_url != nullptr && pinned_url[0] != '\0') {
        result_->integrity_pinned_urls.emplace_back(pinned_url);
      }
    }

    // <script src> collection for the analysis resource map.  Like the
    // stylesheet/SRI/origin lists this runs for worker-injected nodes too
    // (an extra entry only adds a map candidate, never flaps the hash).
    if (keyword == net_instaweb::HtmlName::kScript &&
        result_->script_srcs.size() < 1000) {
      const char* script_src =
          element->AttributeValue(net_instaweb::HtmlName::kSrc);
      if (script_src != nullptr && script_src[0] != '\0') {
        result_->script_srcs.emplace_back(script_src);
      }
    }

    // Author <base href> detection.  An href-less <base target> sets no base
    // URL, so it must not suppress the analysis-time base injection.  Only the
    // first base-with-href sets the document base URL (HTML spec), so the
    // captured value is first-wins.
    if (keyword == net_instaweb::HtmlName::kBase && !result_->has_base_href) {
      const char* base_href =
          element->AttributeValue(net_instaweb::HtmlName::kHref);
      if (base_href != nullptr) {
        result_->has_base_href = true;
        result_->base_href = base_href;
      }
    }

    // Track when we enter a <style> tag (skip previously-injected
    // critical CSS to avoid duplication on revalidation passes).
    if (keyword == net_instaweb::HtmlName::kStyle) {
      if (element->FindAttribute("data-pagespeed-critical") == nullptr) {
        in_style_ = true;
      }
    }

    ++depth_;

    // Track when we enter <body>
    if (keyword == net_instaweb::HtmlName::kBody) {
      in_body_ = true;
    }

    // Hero container tracking for LCP candidate detection
    bool is_hero_container = keyword == net_instaweb::HtmlName::kHeader ||
                             keyword == net_instaweb::HtmlName::kMain ||
                             keyword == net_instaweb::HtmlName::kSection ||
                             keyword == net_instaweb::HtmlName::kArticle ||
                             HasHeroClass(class_value);

    bool ancestor_is_hero =
        !ancestor_is_hero_.empty() && ancestor_is_hero_.back();
    ancestor_is_hero_.push_back(is_hero_container || ancestor_is_hero);

    // Origin extraction for preconnect hints.
    // Collect cross-origin origins with priority:
    //   - Render-blocking: <link rel="stylesheet" href>, <script src> (no async/defer)
    //   - Head resources: other <link href> elements
    //   - Body resources: <img src>, <script src> with async/defer
    {
      std::string_view src_url;
      int priority = kPriorityBodyResource;
      // Whether the motivating resource fetches in CORS mode.  Browsers key
      // connection reuse on the request mode, so the emitted preconnect must
      // carry crossorigin exactly when the resource does — otherwise it warms
      // a pool the page never uses.  Evidence: an explicit crossorigin
      // attribute, ES module scripts (always CORS), and font preloads (font
      // fetches are always CORS).  Plain stylesheets/scripts/images are
      // no-cors, which is also the fail-safe default.
      bool crossorigin = false;

      if (keyword == net_instaweb::HtmlName::kLink) {
        const char* href =
            element->AttributeValue(net_instaweb::HtmlName::kHref);
        if (href != nullptr && href[0] != '\0') {
          src_url = href;
          const char* rel =
              element->AttributeValue(net_instaweb::HtmlName::kRel);
          // A deferred primary is the page's stylesheet wearing a preload's
          // rel; classify it the same as the raw <link rel="stylesheet"> it
          // was, so a cross-origin sheet keeps its preconnect priority across
          // the raw and reprocessed passes.
          if (rel != nullptr &&
              (std::string_view(rel) == "stylesheet" ||
               (std::string_view(rel) == "preload" && deferred_primary))) {
            priority = kPriorityRenderBlocking;
          } else {
            priority = in_body_ ? kPriorityBodyResource : kPriorityHeadResource;
          }
          crossorigin =
              element->FindAttribute(net_instaweb::HtmlName::kCrossorigin) !=
                  nullptr ||
              AttrValueEquals(
                  element->AttributeValue(net_instaweb::HtmlName::kAs), "font");
        }
      } else if (keyword == net_instaweb::HtmlName::kScript) {
        const char* src = element->AttributeValue(net_instaweb::HtmlName::kSrc);
        if (src != nullptr && src[0] != '\0') {
          src_url = src;
          bool is_async = element->FindAttribute("async") != nullptr;
          bool is_defer = element->FindAttribute("defer") != nullptr;
          if (!is_async && !is_defer && !in_body_) {
            priority = kPriorityRenderBlocking;
          } else {
            priority = in_body_ ? kPriorityBodyResource : kPriorityHeadResource;
          }
          crossorigin =
              element->FindAttribute(net_instaweb::HtmlName::kCrossorigin) !=
                  nullptr ||
              AttrValueEquals(
                  element->AttributeValue(net_instaweb::HtmlName::kType),
                  "module");
        }
      } else if (keyword == net_instaweb::HtmlName::kImg) {
        const char* src = element->AttributeValue(net_instaweb::HtmlName::kSrc);
        if (src != nullptr && src[0] != '\0') {
          src_url = src;
          priority = kPriorityBodyResource;
          crossorigin = element->FindAttribute(
                            net_instaweb::HtmlName::kCrossorigin) != nullptr;
        }
      }

      if (!src_url.empty()) {
        // First-seen wins: dedup decides BOTH inclusion and the crossorigin
        // bit.  For a mixed-mode origin (fetched both no-cors and CORS),
        // warming one of its pools still beats the old always-crossorigin
        // behavior, and one entry per origin keeps the 4-entry budget simple.
        std::string origin = ExtractOrigin(src_url);
        if (!origin.empty() && origin != page_origin_ &&
            seen_origins_.find(origin) == seen_origins_.end()) {
          seen_origins_.insert(origin);
          origin_entries_.push_back({std::move(origin), priority, crossorigin});
        }
      }
    }

    // LCP candidate detection for <img> in body.  Beacon/hidden images
    // (1x1 tracking pixels etc.) are never the LCP element — considering
    // them would preload a beacon at high priority ahead of real content.
    if (in_body_ && keyword == net_instaweb::HtmlName::kImg) {
      const char* src = element->AttributeValue(net_instaweb::HtmlName::kSrc);
      if (src != nullptr && !std::string_view(src).starts_with("data:")) {
        const char* srcset =
            element->AttributeValue(net_instaweb::HtmlName::kSrcset);
        const auto* sizes_attr = element->FindAttribute("sizes");
        const char* sizes_val = (sizes_attr != nullptr)
                                    ? sizes_attr->DecodedValueOrNull()
                                    : nullptr;

        bool in_hero = !ancestor_is_hero_.empty() && ancestor_is_hero_.back();
        bool eligible = !IsUnlikelyLcpImage(*element);
        if (eligible && in_hero && !hero_candidate_found_) {
          // Hero candidate: override any previous fallback
          result_->lcp_candidate.src = src;
          result_->lcp_candidate.srcset = (srcset != nullptr) ? srcset : "";
          result_->lcp_candidate.sizes =
              (sizes_val != nullptr) ? sizes_val : "";
          result_->lcp_candidate.element_index = element_index_ - 1;
          result_->lcp_candidate.in_picture = ParentIsPicture(element);
          hero_candidate_found_ = true;
        } else if (eligible && !hero_candidate_found_ && body_img_count_ < 50 &&
                   result_->lcp_candidate.src.empty()) {
          // Fallback: first body image (if no hero found yet)
          result_->lcp_candidate.src = src;
          result_->lcp_candidate.srcset = (srcset != nullptr) ? srcset : "";
          result_->lcp_candidate.sizes =
              (sizes_val != nullptr) ? sizes_val : "";
          result_->lcp_candidate.element_index = element_index_ - 1;
          result_->lcp_candidate.in_picture = ParentIsPicture(element);
        }
        ++body_img_count_;
      }
    }
  }

  void EndElement(net_instaweb::HtmlElement* element) override {
    if (depth_ > 0) --depth_;

    // Pop the hero ancestor stack
    if (!ancestor_is_hero_.empty()) {
      ancestor_is_hero_.pop_back();
    }

    // Track when we exit a <style> tag
    if (element->keyword() == net_instaweb::HtmlName::kStyle) {
      in_style_ = false;
    }

    // Track when we exit <body>
    if (element->keyword() == net_instaweb::HtmlName::kBody) {
      in_body_ = false;
    }
  }

  void EndDocument() override {
    // Sort origins by priority (render-blocking first) and cap at 4.
    std::stable_sort(origin_entries_.begin(), origin_entries_.end(),
                     [](const OriginEntry& a, const OriginEntry& b) {
                       return a.priority < b.priority;
                     });
    size_t count = std::min(origin_entries_.size(), size_t{4});
    for (size_t i = 0; i < count; ++i) {
      result_->third_party_origins.push_back(
          {std::move(origin_entries_[i].origin),
           origin_entries_[i].crossorigin});
    }
  }

  void Characters(net_instaweb::HtmlCharactersNode* characters) override {
    // Collect content of <style> tags
    if (in_style_) {
      std::string_view content = characters->contents();
      // Cap inline CSS at 2MB to prevent unbounded growth on malformed HTML.
      if (result_->inline_css.size() + content.size() <=
          size_t{2} * 1024 * 1024) {
        if (!result_->inline_css.empty()) {
          result_->inline_css += "\n";
        }
        result_->inline_css += content;
      }
    }
  }

 private:
  // Returns true if any class token matches a hero-related pattern.
  // A token matches if it equals the pattern or starts with "pattern-"
  // (hyphen-delimited prefix). Case-insensitive.
  static bool HasHeroClass(const char* class_value) {
    if (class_value == nullptr) return false;
    // Split into individual class tokens
    std::vector<std::string_view> tokens = absl::StrSplit(
        class_value, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty());

    static constexpr std::string_view kPatterns[] = {"hero", "banner",
                                                     "masthead", "above-fold"};

    for (const auto& token : tokens) {
      std::string lower_token(token);
      net_instaweb::AsciiToLowerInPlace(lower_token);
      for (const auto& pattern : kPatterns) {
        // Exact match or prefix match with hyphen separator
        if (lower_token == pattern || (lower_token.starts_with(pattern) &&
                                       lower_token.size() > pattern.size() &&
                                       lower_token[pattern.size()] == '-')) {
          return true;
        }
      }
    }
    return false;
  }

  // Origin entry with priority for sorting.
  struct OriginEntry {
    std::string origin;
    int priority;
    bool crossorigin;
  };

  HtmlScanResult* result_;
  int depth_{0};
  int element_index_{0};
  bool in_style_{false};

  // LCP candidate tracking
  std::vector<bool> ancestor_is_hero_;
  bool in_body_ = false;
  bool hero_candidate_found_ = false;
  int body_img_count_ = 0;

  // Origin extraction for preconnect hints
  std::string page_origin_;
  std::unordered_set<std::string> seen_origins_;
  std::vector<OriginEntry> origin_entries_;
};

}  // namespace

bool IsInvisibleElement(const net_instaweb::HtmlElement& element) {
  using net_instaweb::HtmlName;

  // Tiny explicit dimensions: tracking beacons and tracking iframes declare
  // 0/1 px attributes.  Either axis at <= 1 px suffices — no visible
  // element declares it.
  // Surrounding ASCII whitespace is trimmed first: browsers parse
  // dimension attributes leniently, so width=" 1" is a 1px beacon too.
  auto is_tiny_dimension = [](const net_instaweb::HtmlElement::Attribute* a) {
    if (a == nullptr) return false;
    const char* value = a->DecodedValueOrNull();
    if (value == nullptr) return false;
    std::string_view v = absl::StripAsciiWhitespace(value);
    if (v.empty()) return false;
    int parsed = 0;
    for (char c : v) {
      if (c < '0' || c > '9') return false;  // not a plain pixel integer
      parsed = parsed * 10 + (c - '0');
      if (parsed > 1) return false;
    }
    return true;  // "0", "1" (and leading-zero forms thereof)
  };
  if (is_tiny_dimension(element.FindAttribute(HtmlName::kWidth)) ||
      is_tiny_dimension(element.FindAttribute(HtmlName::kHeight))) {
    return true;
  }

  // The hidden attribute removes the element from rendering entirely.
  if (element.FindAttribute("hidden") != nullptr) return true;

  // Inline style that removes the element from rendering.  Whitespace is
  // stripped before matching so "display : none" variants are caught.
  const char* style = element.AttributeValue(HtmlName::kStyle);
  if (style != nullptr) {
    std::string normalized;
    for (const char* p = style; *p != '\0'; ++p) {
      char c = *p;
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
      normalized.push_back(net_instaweb::LowerChar(c));
    }
    if (normalized.find("display:none") != std::string::npos ||
        normalized.find("visibility:hidden") != std::string::npos) {
      return true;
    }
  }

  return false;
}

bool IsUnlikelyLcpImage(const net_instaweb::HtmlElement& element) {
  // An image the markup declares invisible cannot be the LCP element.
  return IsInvisibleElement(element);
}

HtmlScanner::HtmlScanner() {
  // Initialize HTML keywords (idempotent)
  net_instaweb::HtmlKeywords::Init();
}

HtmlScanResult HtmlScanner::Scan(std::string_view url, std::string_view html) {
  HtmlScanResult result;

  // Create parser with a discarding message handler (the vendored
  // canonical kernel actively uses its MessageHandler).
  net_instaweb::NullMessageHandler message_handler;
  auto parser = std::make_unique<net_instaweb::HtmlParse>(&message_handler);

  // Create element collector filter
  auto collector = std::make_unique<ElementCollectorFilter>(&result);
  collector->set_page_url(url);
  parser->AddFilter(collector.get());

  // Note: We do NOT add HtmlWriterFilter - scanner doesn't produce output HTML

  // Parse the HTML
  if (!parser->StartParse(url)) {
    result.success = false;
    result.error_message = "Failed to start parsing: invalid URL";
    return result;
  }

  parser->ParseText(html);
  parser->FinishParse();

  result.success = true;
  return result;
}

}  // namespace pagespeed
