// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Deriving the stored origin Cache-Control state from the header.
//
// The flags these produce are what every later decision reads: whether an
// entry may be stored, when it goes stale, what goes out downstream. The two
// cases worth staring at are the qualified forms (`private="Set-Cookie"` is
// not `private`) and header-present (an origin that sent `Cache-Control:
// max-age=0` is saying something quite different from an origin that sent no
// Cache-Control at all, and only this bit tells them apart).

#include "lib/cache/origin_cache_control.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/classify/alternate_metadata.h"

namespace pagespeed {
namespace {

using AM = AlternateMetadata;

OriginCacheControl Parse(std::string_view header) {
  OriginCacheControl out;
  AccumulateOriginCacheControl(header, &out);
  return out;
}

// --- header presence -----------------------------------------------------

TEST(OriginCacheControl, HeaderPresentIsSetEvenForAnEmptyValue) {
  // "the origin sent Cache-Control" and "the origin asked for a lifetime" are
  // different questions; the per-content-type freshness defaults turn on the
  // first one.
  const OriginCacheControl cc = Parse("");
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginHeaderPresent);
  EXPECT_EQ(cc.max_age, 0u);
}

TEST(OriginCacheControl, NoCallMeansNoHeader) {
  OriginCacheControl cc;
  EXPECT_EQ(cc.cc_flags, 0u);
}

// --- the directive table -------------------------------------------------

TEST(OriginCacheControl, EachDirectiveMapsToItsFlag) {
  EXPECT_TRUE(Parse("no-cache").cc_flags & AM::kCCOriginNoCache);
  EXPECT_TRUE(Parse("must-revalidate").cc_flags & AM::kCCOriginMustRevalidate);
  EXPECT_TRUE(Parse("proxy-revalidate").cc_flags &
              AM::kCCOriginProxyRevalidate);
  EXPECT_TRUE(Parse("no-store").cc_flags & AM::kCCOriginNoStore);
  EXPECT_TRUE(Parse("private").cc_flags & AM::kCCOriginPrivate);
  EXPECT_TRUE(Parse("public").cc_flags & AM::kCCOriginPublic);
  EXPECT_TRUE(Parse("immutable").cc_flags & AM::kCCOriginImmutable);
  EXPECT_TRUE(Parse("no-transform").cc_flags & AM::kCCOriginNoTransform);
}

TEST(OriginCacheControl, DirectiveNamesAreCaseInsensitive) {
  EXPECT_TRUE(Parse("NO-STORE").cc_flags & AM::kCCOriginNoStore);
  EXPECT_TRUE(Parse("Public").cc_flags & AM::kCCOriginPublic);
  EXPECT_EQ(Parse("MAX-AGE=42").max_age, 42u);
}

TEST(OriginCacheControl, UnknownDirectivesAreIgnored) {
  const OriginCacheControl cc = Parse("surrogate-control=whatever, max-age=5");
  EXPECT_EQ(cc.max_age, 5u);
  EXPECT_EQ(cc.cc_flags, AM::kCCOriginHeaderPresent);
}

// --- lifetimes -----------------------------------------------------------

TEST(OriginCacheControl, MaxAgeAndSMaxage) {
  const OriginCacheControl cc = Parse("max-age=600, s-maxage=1200");
  EXPECT_EQ(cc.max_age, 600u);
  EXPECT_EQ(cc.s_maxage, 1200u);
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginSMaxagePresent);
}

TEST(OriginCacheControl, SMaxagePresentIsSetEvenAtZero) {
  const OriginCacheControl cc = Parse("s-maxage=0");
  EXPECT_EQ(cc.s_maxage, 0u);
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginSMaxagePresent);
}

TEST(OriginCacheControl, DeltaSecondsSaturatesRatherThanWrapping) {
  EXPECT_EQ(ParseCacheControlSeconds("4294967295"), 4294967295u);
  EXPECT_EQ(ParseCacheControlSeconds("99999999999999"), 4294967295u);
}

TEST(OriginCacheControl, MalformedDeltaSecondsIsZeroNotHuge) {
  // Failing toward "no lifetime" keeps a garbled header from granting a long
  // one; the other direction would be a cache poisoning shape.
  EXPECT_EQ(ParseCacheControlSeconds("abc"), 0u);
  EXPECT_EQ(ParseCacheControlSeconds("12x"), 0u);
  EXPECT_EQ(ParseCacheControlSeconds(""), 0u);
  EXPECT_EQ(ParseCacheControlSeconds("-5"), 0u);
}

TEST(OriginCacheControl, DeltaSecondsToleratesQuotesAndLeadingSpace) {
  EXPECT_EQ(ParseCacheControlSeconds("\"600\""), 600u);
  EXPECT_EQ(ParseCacheControlSeconds("  600"), 600u);
}

// --- the qualified forms (RFC 9111 §5.2.2.4 / §5.2.2.7) ------------------

TEST(OriginCacheControl, BarePrivateIsBlanket) {
  const OriginCacheControl cc = Parse("private");
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPrivate);
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPrivateBare);
  EXPECT_FALSE(cc.cc_flags & AM::kCCOriginPrivateQualified);
}

TEST(OriginCacheControl, QualifiedPrivateIsNotBlanket) {
  // `private="Set-Cookie", max-age=600` restricts ONE field and explicitly
  // permits storing the rest. Reading it as blanket would destroy the entry.
  const OriginCacheControl cc = Parse("private=\"Set-Cookie\", max-age=600");
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPrivate);
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPrivateQualified);
  EXPECT_FALSE(cc.cc_flags & AM::kCCOriginPrivateBare);
  EXPECT_EQ(cc.max_age, 600u);
}

TEST(OriginCacheControl, EmptyQualifierIsStillTheQualifiedForm) {
  // The grammar permits an empty field-name list, so the test is the presence
  // of '=', not a non-empty argument.
  const OriginCacheControl cc = Parse("private=\"\"");
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPrivateQualified);
  EXPECT_FALSE(cc.cc_flags & AM::kCCOriginPrivateBare);
}

TEST(OriginCacheControl, QuotedFieldListCannotManufactureABareDirective) {
  // The comma split is not quoted-string aware: `private="A, B"` arrives as
  // `private="A` plus a stranded `B"`. The first is still qualified and the
  // second matches no directive -- what must never happen is the fragment
  // reading back as a BARE private.
  const OriginCacheControl cc = Parse("private=\"A, B\"");
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPrivateQualified);
  EXPECT_FALSE(cc.cc_flags & AM::kCCOriginPrivateBare);
}

TEST(OriginCacheControl, NoCacheHasTheSameTwoForms) {
  const OriginCacheControl bare = Parse("no-cache");
  EXPECT_TRUE(bare.cc_flags & AM::kCCOriginNoCacheBare);
  EXPECT_FALSE(bare.cc_flags & AM::kCCOriginNoCacheQualified);

  const OriginCacheControl qualified = Parse("no-cache=\"Set-Cookie\"");
  EXPECT_TRUE(qualified.cc_flags & AM::kCCOriginNoCacheQualified);
  EXPECT_FALSE(qualified.cc_flags & AM::kCCOriginNoCacheBare);
}

// --- accumulation across header lines ------------------------------------

TEST(OriginCacheControl, FlagsAccumulateAcrossLines) {
  OriginCacheControl cc;
  AccumulateOriginCacheControl("public", &cc);
  AccumulateOriginCacheControl("max-age=300", &cc);
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPublic);
  EXPECT_EQ(cc.max_age, 300u);
}

TEST(OriginCacheControl, BareSeenSurvivesAQualifiedOccurrenceElsewhere) {
  // An intermediary appending a bare `private` after the origin's qualified
  // one must win: bare-seen is the monotone signal a destructive consumer
  // keys on, so it must not be washed out by the qualified bit.
  OriginCacheControl cc;
  AccumulateOriginCacheControl("private=\"Set-Cookie\"", &cc);
  AccumulateOriginCacheControl("private", &cc);
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPrivateBare);
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPrivateQualified);
}

TEST(OriginCacheControl, ALaterLifetimeOverwritesAnEarlierOne) {
  OriginCacheControl cc;
  AccumulateOriginCacheControl("max-age=100", &cc);
  AccumulateOriginCacheControl("max-age=200", &cc);
  EXPECT_EQ(cc.max_age, 200u);
}

TEST(OriginCacheControl, WhitespaceAroundDirectivesAndNamesIsTolerated) {
  const OriginCacheControl cc = Parse("  public ,  max-age =  60 ");
  EXPECT_TRUE(cc.cc_flags & AM::kCCOriginPublic);
  EXPECT_EQ(cc.max_age, 60u);
}

TEST(OriginCacheControl, NullOutIsANoOpRatherThanACrash) {
  AccumulateOriginCacheControl("public", nullptr);
}

// ===========================================================================
// Historical-behaviour pin: the pre-consolidation implementation as an oracle
// ===========================================================================
//
// The nginx front stage no longer carries its own copy of this parse -- it
// calls AccumulateOriginCacheControl per header line (#1300). So this is no
// longer a comparison between two live implementations, and it is NOT kept
// alive as a drift tripwire between copies: there is one copy.
//
// What it is now is what VaryStorabilityEquivalence has been since #1299, and
// for the same reason. The implementation below is the derivation as it stood
// BEFORE consolidation, transcribed verbatim -- down to nginx's own
// ngx_strncasecmp fold, transcribed rather than assumed equivalent, because
// "it is just tolower" is exactly the kind of assumption that hides a
// difference on the bytes either side of the A-Z range. Comparing the shared
// implementation against it pins the SHIPPED behaviour of every front end that
// ever wrote one of these entries: an unintended change to the flags a stored
// entry carries goes red here, and stored entries outlive the code that wrote
// them.
//
// That makes it a behaviour pin with a fixed reference, not a duplicate under
// observation. Retiring it is a decision about how long that reference stays
// interesting -- and one to take together with the Vary oracle, which plays
// exactly this role, rather than as a side effect of a rewire.

namespace legacy {

// Transcribed from nginx's ngx_strncasecmp: folds ONLY A-Z, unsigned compare,
// early return on a shared NUL.
int NgxStrncasecmp(const unsigned char* s1, const unsigned char* s2, size_t n) {
  while (n != 0) {
    unsigned int c1 = *s1++;
    unsigned int c2 = *s2++;
    c1 = (c1 >= 'A' && c1 <= 'Z') ? (c1 | 0x20) : c1;
    c2 = (c2 >= 'A' && c2 <= 'Z') ? (c2 | 0x20) : c2;
    if (c1 == c2) {
      if (c1 != 0) {
        n--;
        continue;
      }
      return 0;
    }
    return static_cast<int>(c1) - static_cast<int>(c2);
  }
  return 0;
}

bool CcNameEq(std::string_view token, const char* name) {
  size_t len = std::strlen(name);
  if (token.size() != len) return false;
  return NgxStrncasecmp(reinterpret_cast<const unsigned char*>(token.data()),
                        reinterpret_cast<const unsigned char*>(name), len) == 0;
}

uint32_t ParseUintSaturating(std::string_view val) {
  while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
  if (!val.empty() && val.front() == '"') val.remove_prefix(1);
  if (!val.empty() && val.back() == '"') val.remove_suffix(1);
  while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
  if (val.empty()) return 0;
  uint64_t result = 0;
  for (char c : val) {
    if (c < '0' || c > '9') return 0;
    result = result * 10 + static_cast<uint64_t>(c - '0');
    if (result > UINT32_MAX) return UINT32_MAX;
  }
  return static_cast<uint32_t>(result);
}

// The pre-consolidation per-header-line body, unchanged.
void ParseInline(std::string_view header_value, OriginCacheControl* result) {
  std::string_view val(header_value);
  while (!val.empty()) {
    size_t comma = val.find(',');
    std::string_view token;
    if (comma == std::string_view::npos) {
      token = val;
      val = {};
    } else {
      token = val.substr(0, comma);
      val.remove_prefix(comma + 1);
    }
    while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
    while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
    if (token.empty()) continue;

    std::string_view name = token;
    std::string_view value;
    size_t eq = token.find('=');
    if (eq != std::string_view::npos) {
      name = token.substr(0, eq);
      value = token.substr(eq + 1);
      while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    }

    static constexpr struct {
      const char* name;
      uint16_t flag;
    } kCCDirectives[] = {
        {"no-cache", AM::kCCOriginNoCache},
        {"must-revalidate", AM::kCCOriginMustRevalidate},
        {"proxy-revalidate", AM::kCCOriginProxyRevalidate},
        {"no-store", AM::kCCOriginNoStore},
        {"private", AM::kCCOriginPrivate},
        {"public", AM::kCCOriginPublic},
        {"immutable", AM::kCCOriginImmutable},
        {"no-transform", AM::kCCOriginNoTransform},
    };
    const bool has_argument = (eq != std::string_view::npos);

    bool matched = false;
    for (const auto& d : kCCDirectives) {
      if (CcNameEq(name, d.name)) {
        result->cc_flags |= d.flag;
        if (d.flag == AM::kCCOriginPrivate) {
          result->cc_flags |= has_argument ? AM::kCCOriginPrivateQualified
                                           : AM::kCCOriginPrivateBare;
        } else if (d.flag == AM::kCCOriginNoCache) {
          result->cc_flags |= has_argument ? AM::kCCOriginNoCacheQualified
                                           : AM::kCCOriginNoCacheBare;
        }
        matched = true;
        break;
      }
    }
    if (!matched) {
      if (CcNameEq(name, "max-age")) {
        result->max_age = ParseUintSaturating(value);
      } else if (CcNameEq(name, "s-maxage")) {
        result->s_maxage = ParseUintSaturating(value);
        result->cc_flags |= AM::kCCOriginSMaxagePresent;
      }
    }
  }
}

}  // namespace legacy

TEST(OriginCacheControlEquivalence,
     AgreesWithThePreConsolidationImplementation) {
  const std::vector<std::string> pieces = {
      "",
      " ",
      "public",
      "PUBLIC",
      "private",
      "private=\"Set-Cookie\"",
      "private=\"\"",
      "private=\"A, B\"",
      "no-cache",
      "no-cache=\"Set-Cookie\"",
      "no-store",
      "immutable",
      "must-revalidate",
      "proxy-revalidate",
      "no-transform",
      "max-age=600",
      "max-age =  60",
      "max-age=\"600\"",
      "max-age=abc",
      "max-age=99999999999999",
      "s-maxage=0",
      "s-maxage=1200",
      "surrogate-control=x",
      "publicx",
      "priv",
  };

  size_t compared = 0;
  for (const std::string& a : pieces) {
    for (const std::string& b : pieces) {
      for (const char* sep : {",", ", ", " , ", ",,", ""}) {
        std::string input = a;
        input += sep;
        input += b;

        OriginCacheControl got;
        AccumulateOriginCacheControl(input, &got);

        OriginCacheControl want;
        // The shared function sets header-present per call; the front stage
        // set it once per response after its line loop. Same result for the
        // >= 1 line case this corpus exercises, and both leave it clear when
        // there is no header at all (no call is made).
        want.cc_flags |= AM::kCCOriginHeaderPresent;
        legacy::ParseInline(input, &want);

        EXPECT_EQ(got.cc_flags, want.cc_flags)
            << "flags diverged on: [" << input << "]";
        EXPECT_EQ(got.max_age, want.max_age)
            << "max-age diverged on: [" << input << "]";
        EXPECT_EQ(got.s_maxage, want.s_maxage)
            << "s-maxage diverged on: [" << input << "]";
        ++compared;
      }
    }
  }
  EXPECT_EQ(compared, pieces.size() * pieces.size() * 5);
}

}  // namespace
}  // namespace pagespeed
