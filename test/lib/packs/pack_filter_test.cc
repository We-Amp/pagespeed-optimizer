// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/packs/pack_filter.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "lib/base/string_writer.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_parse.h"
#include "lib/html/html_writer_filter.h"
#include "lib/packs/pack_loader.h"
#include "src/worker/html_transform_filter.h"

namespace pagespeed::packs {
namespace {

std::shared_ptr<const Pack> MakePack(std::string_view rules,
                                     std::string_view site_mode = "enforce") {
  const std::string json =
      absl::StrCat(R"({"pack":{"id":"edge-seo","version":"0.1.0"},)",
                   R"("sites":[{"host":"t.test","mode":")", site_mode,
                   R"("}],"rules":[)", rules, "]}");
  auto p = LoadPack(json, "");
  EXPECT_TRUE(p.ok()) << p.status() << "\n" << json;
  return p.ok() ? std::make_shared<const Pack>(*std::move(p)) : nullptr;
}

constexpr char kCanonicalKeep[] =
    R"({"id":"c","kind":"canonical","enforce":true,)"
    R"("value":{"template":"https://t.test{path}"}})";
constexpr char kCanonicalReplace[] =
    R"({"id":"c","kind":"canonical","enforce":true,"on_present":"replace",)"
    R"("value":{"template":"https://t.test{path}"}})";

struct FilterRun {
  std::string html;
  bool modified = false;
  bool would_modify = false;
  SkipReason skip = SkipReason::kNone;
  std::vector<PackDecision> decisions;
};

FilterRun RunFilter(std::shared_ptr<const Pack> pack, std::string_view url,
                    std::string_view html, Mode mode = Mode::kEnforce,
                    PackFilterOptions options = {}, bool with_transform = false,
                    bool all_transforms = false) {
  net_instaweb::HtmlKeywords::Init();
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  auto page = ParsePageUrl(url);
  EXPECT_TRUE(page.has_value());
  PackFilter filter(&parser, std::move(pack), *page, mode, options);
  parser.AddFilter(&filter);

  std::unique_ptr<pagespeed::HtmlTransformFilter> transform;
  if (with_transform) {
    pagespeed::HtmlTransformConfig cfg;
    cfg.enable_critical_css = false;
    cfg.enable_lazy_load = all_transforms;
    cfg.enable_image_dimensions = false;
    cfg.enable_lcp_preload = false;
    cfg.enable_preconnect_injection = false;
    transform = std::make_unique<pagespeed::HtmlTransformFilter>(
        &parser, cfg, "", nullptr, page->host, page->scheme);
    parser.AddFilter(transform.get());
  }
  FilterRun run;
  net_instaweb::StringWriter writer(&run.html);
  net_instaweb::HtmlWriterFilter wf(&parser);
  wf.set_writer(&writer);
  parser.AddFilter(&wf);
  EXPECT_TRUE(parser.StartParse(url));
  // The worker's sequence: the whole document, then FinishParse.
  parser.ParseText(html);
  parser.FinishParse();
  run.modified = filter.modified();
  run.would_modify = filter.would_modify();
  run.skip = filter.skip_reason();
  run.decisions = filter.decisions();
  return run;
}

TEST(PackFilterTest, HeadElementSeenEarlyIsRewritableAtEndDocument) {
  // The canonical is the first thing in <head>; a large body follows. The
  // filter decides at EndDocument and still rewrites the early element.
  std::string html =
      "<html><head><link rel=\"canonical\" href=\"https://x.test/\">"
      "<title>T</title></head><body>";
  for (int i = 0; i < 2000; ++i) html += "<p>filler paragraph</p>\n";
  html += "</body></html>";
  FilterRun r =
      RunFilter(MakePack(kCanonicalReplace), "https://t.test/x", html);
  EXPECT_EQ(r.skip, SkipReason::kNone);
  EXPECT_TRUE(r.modified);
  EXPECT_NE(r.html.find("href=\"https://t.test/x\""), std::string::npos);
  EXPECT_EQ(r.html.find("x.test"), std::string::npos);
}

TEST(PackFilterTest, ReportModeLeavesTheBytesAlone) {
  const std::string html =
      "<html><head><title>T</title></head><body></body></html>";
  FilterRun r = RunFilter(MakePack(kCanonicalKeep), "https://t.test/x", html,
                          Mode::kReport);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.would_modify);
  ASSERT_EQ(r.decisions.size(), 1u);
  EXPECT_EQ(r.decisions[0].mode, Mode::kReport);
  EXPECT_EQ(r.decisions[0].action, Action::kInsert);
}

TEST(PackFilterTest, SiteModeCapsTheRule) {
  const std::string html = "<html><head></head><body></body></html>";
  FilterRun r =
      RunFilter(MakePack(kCanonicalKeep, "report"), "https://t.test/x", html);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
}

TEST(PackFilterTest, RuleWithoutEnforceIsReportOnly) {
  const std::string html = "<html><head></head><body></body></html>";
  FilterRun r =
      RunFilter(MakePack(R"({"id":"c","kind":"canonical",)"
                         R"("value":{"template":"https://t.test{path}"}})"),
                "https://t.test/x", html);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.would_modify);
}

TEST(PackFilterTest, OwnOutputIsAFixedPoint) {
  const std::string html =
      "<html><head><link rel=canonical href=a><link rel=canonical href=b>"
      "<title></title><meta name=description content=\"\"></head><body>"
      "</body></html>";
  auto pack = MakePack(
      R"({"id":"c","kind":"canonical","enforce":true,"on_present":"replace",)"
      R"("value":{"template":"https://t.test{path}"}},)"
      R"({"id":"t","kind":"title","enforce":true,"on_present":"repair",)"
      R"("value":{"template":"T"}},)"
      R"({"id":"d","kind":"description","enforce":true,"on_present":"repair",)"
      R"("value":{"template":"D"}})");
  FilterRun once = RunFilter(pack, "https://t.test/x", html);
  EXPECT_TRUE(once.modified);
  FilterRun twice = RunFilter(pack, "https://t.test/x", once.html);
  EXPECT_EQ(twice.html, once.html);
  EXPECT_FALSE(twice.modified);
}

TEST(PackFilterTest, PackOutlivesTheCallersReference) {
  auto pack = MakePack(kCanonicalKeep);
  net_instaweb::HtmlKeywords::Init();
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  PackFilter filter(&parser, pack, *ParsePageUrl("https://t.test/x"),
                    Mode::kEnforce);
  pack.reset();  // the filter holds the only reference now
  parser.AddFilter(&filter);
  std::string out;
  net_instaweb::StringWriter writer(&out);
  net_instaweb::HtmlWriterFilter wf(&parser);
  wf.set_writer(&writer);
  parser.AddFilter(&wf);
  ASSERT_TRUE(parser.StartParse("https://t.test/x"));
  parser.ParseText("<html><head></head></html>");
  parser.FinishParse();
  EXPECT_TRUE(filter.modified());
  EXPECT_NE(out.find("rel=\"canonical\""), std::string::npos);
}

TEST(PackFilterTest, NullPackPassesThrough) {
  const std::string html = "<html><head></head><body></body></html>";
  FilterRun r = RunFilter(nullptr, "https://t.test/x", html);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
}

TEST(PackFilterTest, HostNotListedAndNoHeadAreSkippedWhole) {
  const std::string html = "<html><body></body></html>";
  EXPECT_EQ(
      RunFilter(MakePack(kCanonicalKeep), "https://other.test/x", html).skip,
      SkipReason::kHostNotListed);
  FilterRun r = RunFilter(MakePack(kCanonicalKeep), "https://t.test/x", html);
  EXPECT_EQ(r.skip, SkipReason::kNoHead);
  EXPECT_EQ(r.html, html);
  EXPECT_TRUE(r.decisions.empty());
}

TEST(PackFilterTest, SizeLimitLeavesThePageUntouched) {
  const std::string html = "<html><head></head><body></body></html>";
  PackFilterOptions opts;
  opts.max_added_bytes = 8;
  FilterRun r = RunFilter(MakePack(kCanonicalKeep), "https://t.test/x", html,
                          Mode::kEnforce, opts);
  EXPECT_EQ(r.skip, SkipReason::kSizeLimit);
  EXPECT_EQ(r.html, html);
  EXPECT_FALSE(r.modified);
}

TEST(PackFilterTest, OnlyTheFirstHeadIsUsed) {
  const std::string html =
      "<html><head></head><head></head><body></body></html>";
  FilterRun r = RunFilter(MakePack(kCanonicalKeep), "https://t.test/x", html);
  EXPECT_EQ(r.html,
            "<html><head><link rel=\"canonical\" href=\"https://t.test/x\">"
            "</head><head></head><body></body></html>");
}

TEST(PackFilterTest, AttributeValuesAreQuoted) {
  auto pack =
      MakePack(R"({"id":"c","kind":"canonical","enforce":true,)"
               R"("value":{"template":"https://t.test{path}?a=1&b=2"}})");
  FilterRun r = RunFilter(pack, "https://t.test/x",
                          "<html><head></head><body></body></html>");
  EXPECT_NE(r.html.find("href=\"https://t.test/x?a=1&amp;b=2\""),
            std::string::npos)
      << r.html;
}

TEST(PackFilterTest, TitleTextIsEscaped) {
  auto pack = MakePack(
      R"({"id":"t","kind":"title","enforce":true,)"
      R"("value":{"template":"</title><script>x</script> & {path}"}})");
  FilterRun r = RunFilter(pack, "https://t.test/x",
                          "<html><head></head><body></body></html>");
  EXPECT_NE(r.html.find("<title>&lt;/title&gt;&lt;script&gt;x&lt;/script&gt; "
                        "&amp; /x</title>"),
            std::string::npos)
      << r.html;
}

TEST(PackFilterTest, PageWithUnclosedHeadDoesNotCrash) {
  FilterRun r = RunFilter(MakePack(kCanonicalKeep), "https://t.test/x",
                          "<html><head><title>t");
  EXPECT_EQ(r.skip, SkipReason::kNone);
  FilterRun garbage =
      RunFilter(MakePack(kCanonicalKeep), "https://t.test/x",
                "<<<>>><head><link rel=><meta name=description");
  (void)garbage;
}

TEST(PackFilterTest, OutputDoesNotDependOnTheOtherTransforms) {
  // The pack never sees the device class; what follows it in the pass may
  // differ per class, and the head it produced is the same.
  const std::string html =
      "<html><head><title>T</title></head><body><img src=\"/a.jpg\">"
      "</body></html>";
  auto pack = MakePack(kCanonicalKeep);
  FilterRun a = RunFilter(pack, "https://t.test/x", html, Mode::kEnforce, {},
                          true, false);
  FilterRun b =
      RunFilter(pack, "https://t.test/x", html, Mode::kEnforce, {}, true, true);
  auto head = [](const std::string& s) {
    return s.substr(0, s.find("</head>"));
  };
  EXPECT_EQ(head(a.html), head(b.html));
  EXPECT_NE(head(a.html).find("rel=\"canonical\""), std::string::npos);
  EXPECT_EQ(a.decisions, b.decisions);
}

TEST(PackFilterTest, UnclosedInertElementInHeadSkipsThePage) {
  const std::string html =
      "<html><head><svg><title>t</title></head><body><p>x</p></body></html>";
  FilterRun r = RunFilter(MakePack(kCanonicalKeep), "https://t.test/x", html);
  EXPECT_EQ(r.skip, SkipReason::kMalformedHead);
  EXPECT_FALSE(r.modified);
  EXPECT_TRUE(r.decisions.empty());
}

TEST(PackFilterTest, HeadWithoutCloseEndsAtBody) {
  FilterRun r =
      RunFilter(MakePack(kCanonicalKeep), "https://t.test/x",
                "<html><head><title>T</title>\n<body><p>hi</p></body></html>");
  EXPECT_EQ(r.html,
            "<html><head><title>T</title>\n<link rel=\"canonical\" "
            "href=\"https://t.test/x\"><body><p>hi</p></body></html>");
}

TEST(PackFilterTest, ManyTitlesDoNotInvalidateTheOpenOne) {
  // More titles than a vector's first allocation: the element being read
  // must survive the growth.
  std::string html = "<html><head>";
  for (int i = 0; i < 40; ++i) html += absl::StrCat("<title>t", i, "</title>");
  html += "</head><body></body></html>";
  auto pack = MakePack(
      R"({"id":"t","kind":"title","enforce":true,"on_present":"repair",)"
      R"("value":{"template":"T"}})");
  FilterRun r = RunFilter(pack, "https://t.test/x", html);
  EXPECT_TRUE(r.modified);
  EXPECT_NE(r.html.find("<title>t0</title>"), std::string::npos);
  EXPECT_EQ(r.html.find("<title>t1</title>"), std::string::npos);
}

TEST(PackFilterTest, SizeLimitCountsTheEscapedBytes) {
  // 40 '&' are 200 bytes once escaped.
  auto pack = MakePack(
      R"({"id":"d","kind":"description","enforce":true,)"
      R"("value":{"template":"&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&"}})");
  PackFilterOptions opts;
  opts.max_added_bytes = 150;
  const std::string html = "<html><head></head><body></body></html>";
  EXPECT_EQ(
      RunFilter(pack, "https://t.test/x", html, Mode::kEnforce, opts).skip,
      SkipReason::kSizeLimit);
  opts.max_added_bytes = 400;
  EXPECT_EQ(
      RunFilter(pack, "https://t.test/x", html, Mode::kEnforce, opts).skip,
      SkipReason::kNone);
}

TEST(PackFilterTest, ParsePageUrl) {
  auto p = ParsePageUrl("HTTPS://Www.Example.com:8443/a/b?x=1#frag");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->scheme, "https");
  EXPECT_EQ(p->host, "www.example.com");
  EXPECT_EQ(p->path, "/a/b");
  EXPECT_EQ(p->query, "x=1");
  EXPECT_EQ(ParsePageUrl("https://a.test")->path, "/");
  EXPECT_EQ(ParsePageUrl("https://a.test?q=1")->path, "/");
  EXPECT_FALSE(ParsePageUrl("ftp://a.test/").has_value());
  EXPECT_FALSE(ParsePageUrl("/relative").has_value());
  EXPECT_FALSE(ParsePageUrl("https://user@a.test/").has_value());
}

TEST(PackFilterTest, FilterContract) {
  net_instaweb::NullMessageHandler handler;
  net_instaweb::HtmlParse parser(&handler);
  auto url = *ParsePageUrl("https://t.test/");
  PackFilter keep(&parser, MakePack(kCanonicalKeep), url, Mode::kEnforce);
  EXPECT_FALSE(keep.CanModifyUrls());
  EXPECT_EQ(keep.GetScriptUsage(),
            net_instaweb::HtmlFilter::kNeverInjectsScripts);
  PackFilter replace(&parser, MakePack(kCanonicalReplace), url, Mode::kEnforce);
  EXPECT_TRUE(replace.CanModifyUrls());
  PackFilter report(&parser, MakePack(kCanonicalReplace), url, Mode::kReport);
  EXPECT_FALSE(report.CanModifyUrls());
}

}  // namespace
}  // namespace pagespeed::packs
