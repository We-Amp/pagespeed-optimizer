// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — Critical-CSS validator tests
//
// Two halves: the pure document synthesis, and the CDP-driven validation run
// against a scripted browser (test/test_util/cdp_scripted_peer.h).

#include "src/browser/critical_css_validator.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/visual_regression_gate.h"
#include "test/test_util/cdp_scripted_peer.h"
#include "test/test_util/png_image.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;
using test::EncodePng;
using test::MakeSolidImage;

constexpr char kPage[] =
    "<!doctype html><html><head>"
    "<title>Home</title>"
    "<link rel=\"stylesheet\" href=\"/_astro/base.css\">"
    "<link rel=\"preconnect\" href=\"https://fonts.example\">"
    "<style>body{margin:0}</style>"
    "</head><body><h1>Hi</h1></body></html>";

// Everything between <style data-pagespeed-critical> and its </style>, or "" if
// absent.
std::string InjectedStyleBody(const std::string& doc) {
  const std::string open = "<style data-pagespeed-critical>";
  size_t a = doc.find(open);
  if (a == std::string::npos) return {};
  a += open.size();
  size_t b = doc.find("</style>", a);
  if (b == std::string::npos) return {};
  return doc.substr(a, b - a);
}

// The document with the injected style ELEMENT removed entirely.
std::string WithoutInjectedStyle(const std::string& doc) {
  const std::string open = "<style data-pagespeed-critical>";
  size_t a = doc.find(open);
  if (a == std::string::npos) return doc;
  size_t b = doc.find("</style>", a);
  if (b == std::string::npos) return doc;
  return doc.substr(0, a) + doc.substr(b + 8);
}

// ---------------------------------------------------------------------------
// Document synthesis
// ---------------------------------------------------------------------------

TEST(BuildValidationDocumentsTest, BothDocumentsCarryNoExternalStylesheetLink) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;

  // The declared sheet is gone from both — otherwise the reference would apply
  // it twice and the candidate would apply the full sheet it is supposed to be
  // rendering WITHOUT.
  EXPECT_EQ(docs.reference.find("base.css"), std::string::npos);
  EXPECT_EQ(docs.candidate.find("base.css"), std::string::npos);
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);

  // A non-stylesheet <link> is left alone: it is not a source of styling and
  // removing it would be an unexplained difference from the served page.
  EXPECT_NE(docs.reference.find("preconnect"), std::string::npos);
  EXPECT_NE(docs.candidate.find("preconnect"), std::string::npos);
}

TEST(BuildValidationDocumentsTest, PreExistingStyleBlocksAreRemovedFromBoth) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;

  // The page's own <style> body is already part of the combined sheet (it is
  // the seed of it), so leaving it in place would apply it to the candidate for
  // free — the candidate would look better than the block being judged.
  EXPECT_EQ(docs.reference.find("body{margin:0}"), std::string::npos);
  EXPECT_EQ(docs.candidate.find("body{margin:0}"), std::string::npos);
  EXPECT_EQ(docs.style_blocks_removed, 1u);
}

TEST(BuildValidationDocumentsTest,
     ReferenceCarriesFullCssCandidateCarriesCriticalOnly) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}\n.b{color:blue}",
                                       ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;

  EXPECT_EQ(InjectedStyleBody(docs.reference), ".a{color:red}\n.b{color:blue}");
  EXPECT_EQ(InjectedStyleBody(docs.candidate), ".a{color:red}");
}

TEST(BuildValidationDocumentsTest, DocumentsDifferOnlyInTheInjectedStyleBody) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}\n.b{color:blue}",
                                       ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;

  // The fairness invariant. Any other byte that differs is a difference the
  // pixel diff would silently attribute to the CSS.
  EXPECT_EQ(WithoutInjectedStyle(docs.reference),
            WithoutInjectedStyle(docs.candidate));
  EXPECT_TRUE(DocumentsDifferOnlyInStyleBody(docs.reference, docs.candidate));
}

TEST(BuildValidationDocumentsTest, DocumentsDifferOnlyInStyleBodyRejectsSkew) {
  // The predicate has to be able to say no, or it is decoration.
  EXPECT_FALSE(DocumentsDifferOnlyInStyleBody(
      "<html><style data-pagespeed-critical>a</style><p>x</html>",
      "<html><style data-pagespeed-critical>a</style><p>y</html>"));
  // Missing marker on one side.
  EXPECT_FALSE(DocumentsDifferOnlyInStyleBody(
      "<html><style data-pagespeed-critical>a</style></html>",
      "<html></html>"));
  // Same body, same surroundings.
  EXPECT_TRUE(DocumentsDifferOnlyInStyleBody(
      "<html><style data-pagespeed-critical>a</style><p>x</html>",
      "<html><style data-pagespeed-critical>bb</style><p>x</html>"));
}

TEST(BuildValidationDocumentsTest, LinksAndStylesInsideCommentsSurvive) {
  // A commented-out stylesheet link does not style anything, so removing it
  // would be an unexplained difference from the page the visitor gets.
  std::string page =
      "<html><head><!-- <link rel=stylesheet href=/dead.css> -->"
      "<link rel=stylesheet href=/live.css></head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_NE(docs.reference.find("dead.css"), std::string::npos);
  EXPECT_EQ(docs.reference.find("live.css"), std::string::npos);
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
}

TEST(BuildValidationDocumentsTest, LinkTextInsideScriptIsNotMarkup) {
  std::string page =
      "<html><head><script>var s='<link rel=stylesheet href=/x.css>';</script>"
      "<link rel=stylesheet href=/real.css></head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_NE(docs.reference.find("x.css"), std::string::npos)
      << "a string inside <script> is text, not a stylesheet link";
  EXPECT_EQ(docs.reference.find("real.css"), std::string::npos);
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
}

TEST(BuildValidationDocumentsTest, QuotedAngleBracketInsideLinkTagIsNotTagEnd) {
  std::string page =
      "<html><head><link rel=\"stylesheet\" title=\"a>b\" href=/s.css>"
      "<meta name=keep></head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
  EXPECT_EQ(docs.reference.find("s.css"), std::string::npos);
  EXPECT_NE(docs.reference.find("<meta name=keep>"), std::string::npos)
      << "the tag scan stopped at the quoted '>' and ate the following markup";
}

TEST(BuildValidationDocumentsTest, MultiValuedRelIsStillAStylesheet) {
  std::string page =
      "<html><head><link rel=\"alternate stylesheet\" href=/alt.css>"
      "</head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
}

TEST(BuildValidationDocumentsTest, RelPreloadAsStyleIsNotAStylesheetLink) {
  // PR-E's primitive. A preload applies nothing on its own; the stylesheet it
  // points at is separately declared and separately stripped.
  std::string page =
      "<html><head><link rel=preload as=style href=/p.css>"
      "</head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.stylesheet_links_removed, 0u);
  EXPECT_NE(docs.reference.find("p.css"), std::string::npos);
}

TEST(BuildValidationDocumentsTest, HandlesHeadlessAndFragmentDocuments) {
  // No <head>: the injector falls through to </body>, then to document start.
  auto no_head = BuildValidationDocuments(
      "<html><body><link rel=stylesheet href=/a.css><p>x</p></body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(no_head.ok) << no_head.error;
  EXPECT_EQ(no_head.reference.find("a.css"), std::string::npos);
  EXPECT_EQ(InjectedStyleBody(no_head.reference), ".a{color:red}");
  EXPECT_TRUE(
      DocumentsDifferOnlyInStyleBody(no_head.reference, no_head.candidate));

  // A bare fragment with neither.
  auto fragment = BuildValidationDocuments("<p>only a paragraph</p>",
                                           ".a{color:red}", ".a{}");
  ASSERT_TRUE(fragment.ok) << fragment.error;
  EXPECT_EQ(InjectedStyleBody(fragment.reference), ".a{color:red}");
  EXPECT_NE(fragment.reference.find("only a paragraph"), std::string::npos);
  EXPECT_TRUE(
      DocumentsDifferOnlyInStyleBody(fragment.reference, fragment.candidate));
}

TEST(BuildValidationDocumentsTest, FullCssContainsStyleTerminatorFailsClosed) {
  // InjectCriticalCss refuses CSS containing `</style` — and reports that
  // refusal as success=TRUE with an EMPTY document. A caller checking only
  // `success` would hand an empty reference to the renderer, which would then
  // diff a blank page against a blank page and validate a page it never looked
  // at. This is the sharpest fail-closed case in the file.
  auto docs = BuildValidationDocuments(
      kPage, ".a{content:\"</style>\"}\n.b{color:blue}", ".a{color:red}");
  EXPECT_FALSE(docs.ok);
  EXPECT_TRUE(docs.reference.empty());
  EXPECT_TRUE(docs.candidate.empty());
  EXPECT_NE(docs.error.find("injection"), std::string::npos) << docs.error;
}

TEST(BuildValidationDocumentsTest, CriticalCssStyleTerminatorFailsClosed) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}",
                                       ".a{content:\"</style \"}");
  EXPECT_FALSE(docs.ok);
  EXPECT_TRUE(docs.candidate.empty());
}

TEST(BuildValidationDocumentsTest, CssCarryingTheInjectionMarkerFailsClosed) {
  // CSS containing the marker the injector writes makes "the injected style
  // body" ambiguous — the pair would no longer be two documents differing in
  // one place, and the fairness invariant is what the whole comparison rests
  // on. The invariant is checked before either document is rendered.
  auto docs = BuildValidationDocuments(
      kPage, ".a{color:red}",
      ".a{content:\"<style data-pagespeed-critical>\"}");
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("outside the injected style body"),
            std::string::npos)
      << docs.error;
  EXPECT_TRUE(docs.reference.empty());
  EXPECT_TRUE(docs.candidate.empty());
}

TEST(BuildValidationDocumentsTest, EmptyInputsFailClosed) {
  // An empty reference stylesheet would render the same blank fold as an empty
  // candidate: diff 0, "validated", and the gate opens on nothing.
  EXPECT_FALSE(BuildValidationDocuments(kPage, "", ".a{}").ok);
  EXPECT_FALSE(BuildValidationDocuments(kPage, ".a{}", "").ok);
  EXPECT_FALSE(BuildValidationDocuments("", ".a{}", ".a{}").ok);
}

TEST(BuildValidationDocumentsTest, OversizedDocumentFailsClosed) {
  std::string big_css(4096, 'x');
  auto docs = BuildValidationDocuments(kPage, big_css, ".a{}",
                                       /*max_document_bytes=*/1024);
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("too large"), std::string::npos) << docs.error;
}

TEST(BuildValidationDocumentsTest, UnclosedStyleElementFailsClosed) {
  // Nothing sensible can be stripped out of a document whose <style> never
  // ends: the rest of the page is inside it.
  auto docs = BuildValidationDocuments(
      "<html><head><style>body{margin:0}</head><body>x</body></html>", ".a{}",
      ".b{}");
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("unterminated"), std::string::npos) << docs.error;
}

TEST(BuildValidationDocumentsTest, UnterminatedCommentFailsClosed) {
  auto docs = BuildValidationDocuments(
      "<html><head><!-- never closed <link rel=stylesheet href=/a.css>", ".a{}",
      ".b{}");
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("unterminated"), std::string::npos) << docs.error;
}

// ---------------------------------------------------------------------------
// The validation run
// ---------------------------------------------------------------------------

class CriticalCssValidatorCdpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(peer_.Start());
    gate_ = std::make_unique<VisualRegressionGate>(peer_.client());
    identical_ = EncodePng(MakeSolidImage(10, 10, 128, 128, 128), 10, 10);
    // 4 of 100 pixels black => diff ratio 0.04.
    auto pixels = MakeSolidImage(10, 10, 128, 128, 128);
    for (int i = 0; i < 4; ++i) {
      pixels[i * 4 + 0] = 0;
      pixels[i * 4 + 1] = 0;
      pixels[i * 4 + 2] = 0;
    }
    four_pixels_off_ = EncodePng(pixels, 10, 10);

    docs_ = BuildValidationDocuments(kPage, ".a{color:red}\n.b{color:blue}",
                                     ".a{color:red}");
    ASSERT_TRUE(docs_.ok) << docs_.error;
  }

  // The gate is destroyed BEFORE the peer's client, which is the ordering every
  // owner of one has to honour — see VisualRegressionGate's lifetime note.
  void TearDown() override {
    gate_.reset();
    peer_.Stop();
  }

  // Answer both captures, giving capture 2 `second` and capture 1 `first`.
  void ScriptTwoCaptures(const std::vector<uint8_t>& first,
                         const std::vector<uint8_t>& second) {
    peer_.SetResponder([this, first, second](int id, const std::string& method,
                                             const json& cmd) -> bool {
      if (method == "Page.setDocumentContent") {
        peer_.RespondToCommand(id, json::object());
        peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                        cmd.value("sessionId", ""));
        return true;
      }
      if (method == "Page.captureScreenshot") {
        ++shots_;
        peer_.RespondToCommand(
            id, {{"data", test::Base64Encode(shots_ == 1 ? first : second)}});
        return true;
      }
      return false;
    });
  }

  ValidationVerdict RunValidation(float threshold) {
    ValidationVerdict verdict;
    bool done = false;
    ValidateCriticalCss(gate_.get(), docs_, 10, 10, threshold,
                        [&](ValidationVerdict v) {
                          verdict = std::move(v);
                          done = true;
                        });
    peer_.PumpUntil([&done] { return done; }, 120);
    EXPECT_TRUE(done) << "validation never completed";
    return verdict;
  }

  test::ScriptedCdpPeer peer_;
  std::unique_ptr<VisualRegressionGate> gate_;
  ValidationDocuments docs_;
  std::vector<uint8_t> identical_;
  std::vector<uint8_t> four_pixels_off_;
  int shots_ = 0;
};

TEST_F(CriticalCssValidatorCdpTest, DiffBelowThresholdMarksValidated) {
  ScriptTwoCaptures(identical_, identical_);
  ValidationVerdict v = RunValidation(kDefaultValidationDiffThreshold);
  EXPECT_TRUE(v.validated) << v.failure_reason;
  EXPECT_FLOAT_EQ(v.diff_ratio, 0.0f);
  EXPECT_TRUE(v.failure_reason.empty());
  EXPECT_EQ(shots_, 2);
}

TEST_F(CriticalCssValidatorCdpTest, DiffAboveThresholdMarksNotValidated) {
  ScriptTwoCaptures(identical_, four_pixels_off_);
  ValidationVerdict v = RunValidation(0.005f);
  EXPECT_FALSE(v.validated);
  EXPECT_FLOAT_EQ(v.diff_ratio, 0.04f);
  EXPECT_FALSE(v.failure_reason.empty());
}

TEST_F(CriticalCssValidatorCdpTest,
       ThresholdIsTheOnlyThingThatMovesTheVerdict) {
  // The same measurement, both sides of the threshold. Without this the
  // "validated" tests could be passing for any reason at all.
  ScriptTwoCaptures(identical_, four_pixels_off_);
  ValidationVerdict lenient = RunValidation(0.10f);
  EXPECT_TRUE(lenient.validated) << lenient.failure_reason;
  EXPECT_FLOAT_EQ(lenient.diff_ratio, 0.04f);
}

TEST_F(CriticalCssValidatorCdpTest, ScreenshotFailureMarksNotValidated) {
  peer_.SetResponder(
      [this](int id, const std::string& method, const json& cmd) -> bool {
        if (method == "Page.setDocumentContent") {
          peer_.RespondToCommand(id, json::object());
          peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                          cmd.value("sessionId", ""));
          return true;
        }
        if (method == "Page.captureScreenshot") {
          peer_.RespondError(id, -32000, "renderer gone");
          return true;
        }
        return false;
      });

  ValidationVerdict v = RunValidation(kDefaultValidationDiffThreshold);
  EXPECT_FALSE(v.validated);
  EXPECT_LT(v.diff_ratio, 0.0f) << "no measurement was made; do not claim one";
  EXPECT_FALSE(v.failure_reason.empty());
}

TEST_F(CriticalCssValidatorCdpTest, SecondRenderFailureMarksNotValidated) {
  int targets = 0;
  peer_.SetResponder([&](int id, const std::string& method,
                         const json& cmd) -> bool {
    if (method == "Target.createTarget" && ++targets == 2) {
      peer_.RespondError(id, -32000, "out of targets");
      return true;
    }
    if (method == "Page.setDocumentContent") {
      peer_.RespondToCommand(id, json::object());
      peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                      cmd.value("sessionId", ""));
      return true;
    }
    if (method == "Page.captureScreenshot") {
      peer_.RespondToCommand(id, {{"data", test::Base64Encode(identical_)}});
      return true;
    }
    return false;
  });

  ValidationVerdict v = RunValidation(kDefaultValidationDiffThreshold);
  EXPECT_FALSE(v.validated)
      << "the candidate was never rendered; there is nothing to validate";
  EXPECT_LT(v.diff_ratio, 0.0f);
}

TEST_F(CriticalCssValidatorCdpTest, ChromeUnavailableMarksNotValidated) {
  ValidationVerdict verdict;
  bool done = false;
  ValidateCriticalCss(/*gate=*/nullptr, docs_, 10, 10,
                      kDefaultValidationDiffThreshold,
                      [&](ValidationVerdict v) {
                        verdict = std::move(v);
                        done = true;
                      });
  ASSERT_TRUE(done) << "the callback must fire even with no browser";
  EXPECT_FALSE(verdict.validated);
  EXPECT_LT(verdict.diff_ratio, 0.0f);
  EXPECT_FALSE(verdict.failure_reason.empty());
}

TEST_F(CriticalCssValidatorCdpTest, UnbuiltDocumentsMarkNotValidatedNoRender) {
  ValidationDocuments broken;
  broken.ok = false;
  broken.error = "injection aborted";

  ValidationVerdict verdict;
  bool done = false;
  ValidateCriticalCss(gate_.get(), broken, 10, 10,
                      kDefaultValidationDiffThreshold,
                      [&](ValidationVerdict v) {
                        verdict = std::move(v);
                        done = true;
                      });
  ASSERT_TRUE(done);
  EXPECT_FALSE(verdict.validated);
  peer_.RunLoop();
  EXPECT_EQ(peer_.CountCommands("Target.createTarget"), 0u)
      << "a refused document pair must not reach the browser at all";
}

TEST_F(CriticalCssValidatorCdpTest, ComparisonOfNoPixelsMarksNotValidated) {
  // CompareScreenshots reports passed=true with total_pixels==0 when the
  // compare region is empty (VisualRegressionGateTest.ZeroViewportHeight pins
  // that). "Nothing differed because nothing was compared" is not a
  // measurement, and must not open the gate.
  ScriptTwoCaptures(identical_, identical_);

  ValidationVerdict verdict;
  bool done = false;
  ValidateCriticalCss(gate_.get(), docs_, 10, /*viewport_height=*/0,
                      kDefaultValidationDiffThreshold,
                      [&](ValidationVerdict v) {
                        verdict = std::move(v);
                        done = true;
                      });
  peer_.PumpUntil([&done] { return done; }, 120);
  ASSERT_TRUE(done);
  EXPECT_FALSE(verdict.validated)
      << "a comparison over zero pixels validated the page";
}

TEST_F(CriticalCssValidatorCdpTest, BothDocumentsReachTheBrowserUnaltered) {
  ScriptTwoCaptures(identical_, identical_);
  RunValidation(kDefaultValidationDiffThreshold);

  std::vector<std::string> sent;
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.value("method", "") == "Page.setDocumentContent") {
      sent.push_back(cmd.value("params", json::object()).value("html", ""));
    }
  }
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0], docs_.reference);
  EXPECT_EQ(sent[1], docs_.candidate);
}

TEST_F(CriticalCssValidatorCdpTest, TearingDownTheBrowserRefusesInFlight) {
  // The reachable crash path, and the reason the gate is an owned member
  // rather than something a callback keeps alive: Chrome exits, the manager
  // drops its CDP components, and a comparison is still in flight holding a
  // raw client pointer and a 60 s timer. Once the client is gone that timer is
  // the ONLY way the capture can finish, so it is the common path.
  //
  // Teardown order here mirrors the manager's exactly: gate first, client
  // second.
  peer_.SetResponder(
      [this](int id, const std::string& method, const json&) -> bool {
        if (method != "Page.setDocumentContent") return false;
        peer_.RespondToCommand(id, json::object());
        // No lifecycle event: the capture is left genuinely in flight.
        return true;
      });

  ValidationVerdict verdict;
  verdict.validated = true;  // must be overwritten
  bool done = false;
  ValidateCriticalCss(gate_.get(), docs_, 10, 10,
                      kDefaultValidationDiffThreshold,
                      [&](ValidationVerdict v) {
                        verdict = std::move(v);
                        done = true;
                      });
  peer_.PumpUntil([&] { return peer_.SawCommand("Page.setDocumentContent"); },
                  40);
  ASSERT_FALSE(done) << "precondition: the comparison must still be in flight";

  gate_.reset();
  ASSERT_TRUE(done) << "an in-flight comparison must be resolved, not orphaned";
  EXPECT_FALSE(verdict.validated)
      << "a browser that went away confirmed nothing";
  EXPECT_LT(verdict.diff_ratio, 0.0f);

  // Now the client dies too. Nothing may touch it afterwards: drain the loop
  // long enough for a leaked timeout timer to have fired.
  peer_.Stop();
  SUCCEED();
}

TEST_F(CriticalCssValidatorCdpTest, BothCapturesUseTheRequestedViewport) {
  ScriptTwoCaptures(identical_, identical_);
  ValidationVerdict v = RunValidation(kDefaultValidationDiffThreshold);
  ASSERT_TRUE(v.validated) << v.failure_reason;

  // Both documents must be measured at the SAME viewport, or the comparison is
  // between two different pages.
  int seen = 0;
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.value("method", "") != "Emulation.setDeviceMetricsOverride") {
      continue;
    }
    auto params = cmd.value("params", json::object());
    EXPECT_EQ(params.value("width", 0), 10);
    EXPECT_EQ(params.value("height", 0), 10);
    ++seen;
  }
  EXPECT_EQ(seen, 2) << "each capture sets its own viewport";
}

}  // namespace
}  // namespace pagespeed
