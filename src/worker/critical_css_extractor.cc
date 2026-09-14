// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Critical CSS Extractor Implementation

#include "src/worker/critical_css_extractor.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "lib/base/string_util.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed {

struct CssRule {
  std::string selector;
  std::string body;  // includes braces
};

// Depth bound shared by every recursive walk over the rule tree (ProcessRules,
// CollectSelectorContexts). They must agree: a walk that gives up earlier than
// ProcessRules reports a rule as absent that ProcessRules will happily emit.
inline constexpr int kMaxRuleRecursionDepth = 5;

namespace {

// Skip whitespace and comments in CSS
size_t SkipWhitespaceAndComments(std::string_view css, size_t pos) {
  while (pos < css.size()) {
    // Skip whitespace
    while (pos < css.size() &&
           (std::isspace(static_cast<unsigned char>(css[pos])) != 0)) {
      ++pos;
    }

    // Check for comment start
    if (pos + 1 < css.size() && css[pos] == '/' && css[pos + 1] == '*') {
      pos += 2;
      // Find comment end
      while (pos + 1 < css.size() &&
             !(css[pos] == '*' && css[pos + 1] == '/')) {
        ++pos;
      }
      if (pos + 1 < css.size()) {
        pos += 2;  // Skip */
      } else {
        return std::string_view::npos;  // Unterminated comment
      }
    } else {
      break;
    }
  }
  return pos;
}

// Find the end of a CSS block (matching braces)
size_t FindBlockEnd(std::string_view css, size_t start) {
  int depth = 0;
  bool in_string = false;
  char string_char = 0;

  for (size_t i = start; i < css.size(); ++i) {
    char c = css[i];

    // Skip CSS comments (/* ... */) outside strings.
    if (!in_string && c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      i += 2;
      while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) {
        ++i;
      }
      if (i + 1 < css.size()) ++i;  // skip past '/'
      continue;
    }

    // Handle strings
    if (!in_string && (c == '"' || c == '\'')) {
      in_string = true;
      string_char = c;
    } else if (in_string && c == string_char) {
      // Count consecutive preceding backslashes: odd = quote is escaped,
      // even (including zero) = quote is real.
      size_t num_backslashes = 0;
      for (size_t j = i; j > 0 && css[j - 1] == '\\'; --j) {
        ++num_backslashes;
      }
      if (num_backslashes % 2 == 0) {
        in_string = false;
      }
    } else if (!in_string) {
      if (c == '{') {
        ++depth;
      } else if (c == '}') {
        if (depth <= 0) {
          // Unmatched closing brace — treat as end of block.
          return i + 1;
        }
        --depth;
        if (depth == 0) {
          return i + 1;
        }
      }
    }
  }
  return css.size();
}

// Find the first '{' that is not inside a CSS string or [...] attribute
// selector, starting from `start`.  Returns npos if none found.
size_t FindOpenBrace(std::string_view css, size_t start) {
  bool in_string = false;
  char string_char = 0;
  bool in_bracket = false;  // inside [...]

  for (size_t i = start; i < css.size(); ++i) {
    char c = css[i];

    if (in_string) {
      if (c == string_char) {
        size_t num_backslashes = 0;
        for (size_t j = i; j > 0 && css[j - 1] == '\\'; --j) {
          ++num_backslashes;
        }
        if (num_backslashes % 2 == 0) {
          in_string = false;
        }
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      in_string = true;
      string_char = c;
    } else if (c == '[') {
      in_bracket = true;
    } else if (c == ']') {
      in_bracket = false;
    } else if (c == '{' && !in_bracket) {
      return i;
    }
  }
  return std::string_view::npos;
}

// Find the first top-level ';' in [start, limit) that terminates a statement
// at-rule (no block), skipping strings, comments, and [...] attribute
// selectors. Returns npos if none. Used to recognize block-less at-rules such
// as the Tailwind v4 layer-order declaration `@layer theme,base,...;` and
// `@import url(...);`, which the brace-oriented parser would otherwise fuse
// with the next rule's selector.
size_t FindTopLevelSemicolon(std::string_view css, size_t start, size_t limit) {
  bool in_string = false;
  char string_char = 0;
  bool in_bracket = false;
  for (size_t i = start; i < limit && i < css.size(); ++i) {
    char c = css[i];
    if (in_string) {
      if (c == string_char) {
        size_t num_backslashes = 0;
        for (size_t j = i; j > 0 && css[j - 1] == '\\'; --j) ++num_backslashes;
        if (num_backslashes % 2 == 0) in_string = false;
      }
      continue;
    }
    if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      i += 2;
      while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) ++i;
      continue;
    }
    if (c == '"' || c == '\'') {
      in_string = true;
      string_char = c;
    } else if (c == '[') {
      in_bracket = true;
    } else if (c == ']') {
      in_bracket = false;
    } else if (c == ';' && !in_bracket) {
      return i;
    }
  }
  return std::string_view::npos;
}

// Parse CSS rules from input
std::vector<CssRule> ParseCssRules(std::string_view css) {
  std::vector<CssRule> rules;
  size_t pos = 0;

  while (pos < css.size()) {
    pos = SkipWhitespaceAndComments(css, pos);
    if (pos >= css.size()) {
      break;
    }

    // Find the start of the block, skipping braces inside strings and
    // attribute selectors (e.g., div[data-x="{"] { ... }).
    size_t brace_pos = FindOpenBrace(css, pos);

    // A block-less statement AT-rule (only '@...;' — never bare non-at text,
    // which stays malformed garbage) ends at a top-level ';' before the next
    // '{'. Capture it with an empty body so ProcessRules can preserve it (a
    // leading `@layer a,b,c;` establishes cascade order for the layers below).
    size_t limit =
        (brace_pos == std::string_view::npos) ? css.size() : brace_pos;
    size_t semi_pos = FindTopLevelSemicolon(css, pos, limit);
    if (semi_pos != std::string_view::npos) {
      std::string_view stmt_sv =
          absl::StripAsciiWhitespace(css.substr(pos, semi_pos - pos));
      if (!stmt_sv.empty() && stmt_sv.front() == '@') {
        rules.push_back({std::string(stmt_sv), std::string()});
        pos = semi_pos + 1;
        continue;
      }
    }

    if (brace_pos == std::string_view::npos) {
      break;
    }

    // Extract selector
    std::string_view selector_sv = css.substr(pos, brace_pos - pos);
    std::string selector(absl::StripAsciiWhitespace(selector_sv));

    // Find block end
    size_t block_end = FindBlockEnd(css, brace_pos);

    // Extract body including braces
    std::string body(css.substr(brace_pos, block_end - brace_pos));

    if (!selector.empty()) {
      rules.push_back({std::move(selector), std::move(body)});
    }

    pos = block_end;
  }

  return rules;
}

// Convert string to lowercase for case-insensitive comparison
std::string ToLower(std::string_view str) {
  return net_instaweb::AsciiToLower(str);
}

// Check if a word boundary exists around "print" at position pos in str
bool HasWordBoundary(std::string_view str, size_t pos, size_t len) {
  if (pos > 0 &&
      (std::isalnum(static_cast<unsigned char>(str[pos - 1])) != 0)) {
    return false;
  }
  size_t end = pos + len;
  if (end < str.size() &&
      (std::isalnum(static_cast<unsigned char>(str[end])) != 0)) {
    return false;
  }
  return true;
}

// Check if the rule is an @media print rule
bool IsMediaPrintRule(std::string_view selector) {
  std::string lower = ToLower(selector);
  if (!absl::StartsWith(lower, "@media")) {
    return false;
  }
  // Look for "print" as a whole word after @media
  size_t pos = 6;  // length of "@media"
  while (pos < lower.size()) {
    size_t found = lower.find("print", pos);
    if (found == std::string::npos) {
      return false;
    }
    if (HasWordBoundary(lower, found, 5)) {
      return true;
    }
    pos = found + 1;
  }
  return false;
}

// Match an at-rule by NAME, not by prefix: `@import` must not match
// `@importantly`. The boundary is "next char cannot continue a CSS ident"
// rather than the container helper's whitespace-or-brace set, because a
// minifier legally emits `@import"x.css"` and `@charset"utf-8"` with no
// separator at all.
bool AtRuleNameIs(std::string_view selector, std::string_view name) {
  if (!absl::StartsWithIgnoreCase(selector, name)) return false;
  if (selector.size() == name.size()) return true;
  unsigned char next = static_cast<unsigned char>(selector[name.size()]);
  if (next >= 0x80) return false;  // non-ASCII continues an ident
  return (std::isalnum(next) == 0) && next != '_' && next != '-' &&
         next != '\\';
}

// Check if the rule is an @-rule that should always be included
bool IsAlwaysIncludedAtRule(std::string_view selector) {
  // Always include @charset, @import, @font-face, @keyframes.
  //
  // @property / @counter-style / @font-palette-values / @font-feature-values /
  // @position-try are registrations, not styling: a RETAINED rule referencing
  // an unregistered custom property, counter style, palette, feature set or
  // position-try tactic is invalid at computed-value time, so dropping the
  // registration makes the rules that survived compute the WRONG value rather
  // than merely omitting a rule. @namespace likewise re-interprets the type
  // selectors of every rule that survives. None may be filtered out.
  return AtRuleNameIs(selector, "@charset") ||
         AtRuleNameIs(selector, "@import") ||
         AtRuleNameIs(selector, "@namespace") ||
         AtRuleNameIs(selector, "@font-face") ||
         AtRuleNameIs(selector, "@font-feature-values") ||
         AtRuleNameIs(selector, "@font-palette-values") ||
         AtRuleNameIs(selector, "@property") ||
         AtRuleNameIs(selector, "@position-try") ||
         AtRuleNameIs(selector, "@counter-style") ||
         AtRuleNameIs(selector, "@keyframes") ||
         AtRuleNameIs(selector, "@-webkit-keyframes") ||
         AtRuleNameIs(selector, "@-moz-keyframes");
}

// Try to parse a CSS length value ("Npx", "Nem", "Nrem") starting at
// pos.  Returns the equivalent pixel value or -1 if unrecognized.
// em/rem are converted using a 16px base (browser default).
int ParseCssLength(std::string_view s, size_t pos) {
  // Skip whitespace
  while (pos < s.size() &&
         (std::isspace(static_cast<unsigned char>(s[pos])) != 0))
    ++pos;
  if (pos >= s.size() ||
      (std::isdigit(static_cast<unsigned char>(s[pos])) == 0))
    return -1;
  int int_part = 0;
  while (pos < s.size() &&
         (std::isdigit(static_cast<unsigned char>(s[pos])) != 0)) {
    if (int_part > 10000) return -1;  // Overflow guard before multiply.
    int_part = int_part * 10 + (s[pos] - '0');
    ++pos;
  }
  // Handle optional fractional part (e.g., "1.5em").
  double value = int_part;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    double frac = 0.0;
    double divisor = 10.0;
    while (pos < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[pos])) != 0)) {
      frac += (s[pos] - '0') / divisor;
      divisor *= 10.0;
      ++pos;
    }
    value += frac;
  }

  // Match unit (case-insensitive).
  std::string_view rest = s.substr(pos);
  std::string rest_lower = ToLower(rest.substr(0, 3));
  if (rest_lower.starts_with("px")) {
    return static_cast<int>(value);
  }
  if (rest_lower.starts_with("rem") || rest_lower.starts_with("em")) {
    // 1em/1rem = 16px (browser default root font-size).
    return static_cast<int>(value * 16.0);
  }
  return -1;
}

// Try to parse a single media query (e.g., "screen and (min-width: 1024px)")
// and check if it should be excluded for the given viewport.
// Returns: 1 = exclude (no overlap), 0 = include (overlaps), -1 = unknown
// (conservatively include).
int EvaluateSingleMediaQuery(std::string_view query,
                             CapabilityMask::Viewport viewport) {
  std::string_view condition = absl::StripAsciiWhitespace(query);

  // Strip leading "screen and", "all and", or just "screen" / "all"
  // to get to the parenthesized conditions.
  for (const char* prefix : {"screen and ", "all and ", "only screen and "}) {
    if (absl::StartsWith(condition, prefix)) {
      condition = condition.substr(std::string_view(prefix).size());
      break;
    }
  }

  int media_min = -1;
  int media_max = -1;

  // Try to find (min-width: Npx) and (max-width: Npx)
  auto parse_condition = [&](std::string_view cond) -> bool {
    cond = absl::StripAsciiWhitespace(cond);
    if (cond.empty() || cond.front() != '(') return false;
    cond = cond.substr(1);  // skip '('

    if (absl::StartsWith(cond, "min-width:") ||
        absl::StartsWith(cond, "min-width :")) {
      size_t colon = cond.find(':');
      if (colon == std::string_view::npos) return false;
      int val = ParseCssLength(cond, colon + 1);
      if (val < 0) return false;
      media_min = val;
      return true;
    }
    if (absl::StartsWith(cond, "max-width:") ||
        absl::StartsWith(cond, "max-width :")) {
      size_t colon = cond.find(':');
      if (colon == std::string_view::npos) return false;
      int val = ParseCssLength(cond, colon + 1);
      if (val < 0) return false;
      media_max = val;
      return true;
    }
    return false;
  };

  // Look for " and " to split combined conditions.
  size_t and_pos = condition.find(") and (");
  if (and_pos != std::string_view::npos) {
    std::string_view first = condition.substr(0, and_pos + 1);
    std::string_view second = condition.substr(and_pos + 6);
    if (!parse_condition(first) || !parse_condition(second)) {
      return -1;  // Can't parse — conservatively include
    }
  } else {
    if (!parse_condition(condition)) {
      return -1;  // Can't parse — conservatively include
    }
  }

  // Check overlap with viewport range.
  auto vp_range = CapabilityMask::ViewportWidthRange(viewport);

  if (media_min < 0) media_min = 0;
  if (media_max < 0) media_max = 65535;

  if (static_cast<uint32_t>(media_max) < vp_range.min_px ||
      static_cast<uint32_t>(media_min) > vp_range.max_px) {
    return 1;  // exclude
  }
  return 0;  // include
}

// Check if a @media rule should be excluded for the given viewport.
// Handles comma-separated media query lists (include if ANY subquery
// matches).  Returns true if the query should be excluded.
bool ShouldExcludeMediaForViewport(std::string_view media_rule,
                                   CapabilityMask::Viewport viewport) {
  std::string lower = ToLower(media_rule);

  // Extract everything after "@media".
  size_t media_end = 6;  // length of "@media"
  std::string_view full_condition =
      absl::StripAsciiWhitespace(std::string_view(lower).substr(media_end));

  // Split on commas for media query lists.
  // e.g., "@media (max-width: 479px), (min-width: 1024px)"
  // Include if ANY subquery matches the viewport.
  bool any_includes = false;
  bool all_parseable = true;
  size_t pos = 0;
  while (pos < full_condition.size()) {
    size_t comma = full_condition.find(',', pos);
    if (comma == std::string_view::npos) comma = full_condition.size();
    std::string_view subquery = full_condition.substr(pos, comma - pos);
    int result = EvaluateSingleMediaQuery(subquery, viewport);
    if (result == 0) {
      any_includes = true;
      break;  // At least one subquery matches — include the rule.
    }
    if (result == -1) {
      all_parseable = false;
    }
    pos = comma + 1;
  }

  if (any_includes) return false;
  if (!all_parseable) return false;  // Conservatively include.
  return true;                       // All subqueries excluded.
}

// Check if a selector represents a container at-rule whose inner rules
// should be recursed into (@layer, @supports).
bool IsContainerAtRule(std::string_view selector) {
  // Check for @layer or @supports with a word boundary (space, '{', or
  // end-of-string) to avoid matching e.g. "@layerX".
  auto check = [&](std::string_view prefix) -> bool {
    if (!absl::StartsWithIgnoreCase(selector, prefix)) return false;
    if (selector.size() == prefix.size()) return true;
    char next = selector[prefix.size()];
    return next == ' ' || next == '\t' || next == '\n' || next == '\r' ||
           next == '{';
  };
  return check("@layer") || check("@supports");
}

using net_instaweb::HexDigitValue;

// Append a Unicode codepoint as UTF-8 bytes.
void AppendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Advance pos past a CSS escape sequence (backslash already consumed, pos
// points at the character after the backslash).  Handles both hex escapes
// (\3a, \00003a) and literal escapes (\:, \[).
void SkipCssEscape(std::string_view s, size_t& pos) {
  if (pos >= s.size()) return;
  if (std::isxdigit(static_cast<unsigned char>(s[pos])) != 0) {
    // Hex escape: consume 1-6 hex digits.
    size_t hex_start = pos;
    while (pos < s.size() && pos - hex_start < 6 &&
           (std::isxdigit(static_cast<unsigned char>(s[pos])) != 0)) {
      ++pos;
    }
    // Consume optional trailing whitespace (part of the escape).
    if (pos < s.size() && s[pos] == ' ') ++pos;
  } else {
    // Literal escape: skip one character.
    ++pos;
  }
}

// Unescape a CSS identifier: handles both backslash-literal escapes
// (e.g., `lg\:grid` → `lg:grid`, `w-\[100px\]` → `w-[100px]`) and
// CSS hex escapes per CSS Syntax Module Level 3 §4.3.11
// (e.g., `\3a` → `:`, `\00003a` → `:`, `\5b` → `[`).
// A double backslash (`\\`) produces a literal backslash.
std::string UnescapeCssIdent(std::string_view s) {
  std::string result;
  result.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      ++i;
      if (std::isxdigit(static_cast<unsigned char>(s[i])) != 0) {
        // CSS hex escape: consume 1-6 hex digits.
        size_t start = i;
        while (i < s.size() && i - start < 6 &&
               (std::isxdigit(static_cast<unsigned char>(s[i])) != 0)) {
          ++i;
        }
        // Optional trailing whitespace is consumed as part of the escape.
        if (i < s.size() && s[i] == ' ') ++i;
        // Parse the hex value.
        char32_t codepoint = 0;
        for (size_t j = start; j < i && s[j] != ' '; ++j) {
          codepoint = codepoint * 16 + HexDigitValue(s[j]);
        }
        // Per spec: 0 and values > 0x10FFFF map to U+FFFD.
        if (codepoint == 0 || codepoint > 0x10FFFF) {
          codepoint = 0xFFFD;
        }
        AppendUtf8(result, codepoint);
        --i;  // loop will ++i
      } else {
        // Literal escape: backslash + next char.
        result += s[i];
      }
    } else {
      result += s[i];
    }
  }
  return result;
}

// Strip the outer braces from a block body: "{ ... }" → " ... ".
std::string_view StripOuterBraces(std::string_view body) {
  if (body.size() >= 2 && body.front() == '{' && body.back() == '}') {
    return body.substr(1, body.size() - 2);
  }
  return body;
}

// Find the last space in a CSS selector that is a descendant combinator
// (not part of a hex escape like `\3a ` and not inside parentheses like
// `:where(.dark, .dark *)`).  A space is part of a hex escape if the
// preceding characters are: backslash + 1-6 hex digits + this space.
size_t FindLastCombinatorSpace(std::string_view s) {
  int paren_depth = 0;
  for (size_t i = s.size(); i > 0;) {
    --i;
    char c = s[i];
    // Track parenthesis depth (walking backward: ')' opens, '(' closes).
    if (c == ')') {
      ++paren_depth;
      continue;
    }
    if (c == '(') {
      if (paren_depth > 0) --paren_depth;
      continue;
    }
    // Skip spaces inside parenthesized pseudo-functions like :where(), :is().
    if (c != ' ' || paren_depth > 0) continue;
    // Check if this space is consumed by a preceding hex escape.
    // Walk backward: count hex digits, then check for backslash.
    size_t j = i;
    int hex_count = 0;
    while (j > 0 && hex_count < 6 &&
           (std::isxdigit(static_cast<unsigned char>(s[j - 1])) != 0)) {
      --j;
      ++hex_count;
    }
    if (hex_count > 0 && j > 0 && s[j - 1] == '\\') {
      // This space is part of a hex escape — skip it.
      continue;
    }
    return i;
  }
  return std::string_view::npos;
}

// Extract the @layer name from a `@layer <name> {` selector: the trimmed text
// between "@layer" and end-of-string. Anonymous layers (`@layer {`) yield "".
std::string ExtractLayerName(std::string_view selector) {
  if (!absl::StartsWithIgnoreCase(selector, "@layer")) return "";
  std::string_view rest = selector.substr(6);  // past "@layer"
  return std::string(absl::StripAsciiWhitespace(rest));
}

// Extract the normalized @media condition from a `@media <cond> {` selector:
// the text after "@media", whitespace-collapsed and lowercased (media features
// are case-insensitive) so the same query keys identically regardless of
// source spacing/case.
std::string ExtractMediaCondition(std::string_view selector) {
  if (!absl::StartsWithIgnoreCase(selector, "@media")) return "";
  std::string_view rest = selector.substr(6);  // past "@media"
  std::string collapsed;
  collapsed.reserve(rest.size());
  bool in_ws = false;
  for (char c : absl::StripAsciiWhitespace(rest)) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!in_ws) {
        collapsed += ' ';
        in_ws = true;
      }
    } else {
      collapsed +=
          static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      in_ws = false;
    }
  }
  return collapsed;
}

// Append a cascade-layer segment to a ">"-joined layer path (outermost first).
std::string JoinLayer(const std::string& path, const std::string& name) {
  if (path.empty()) return name;
  return absl::StrCat(path, ">", name);
}

// Combine nested @media conditions with " and " (a rule inside two nested
// @media blocks applies only when both hold).
std::string JoinMedia(const std::string& outer, const std::string& inner) {
  if (inner.empty()) return outer;
  if (outer.empty()) return inner;
  return absl::StrCat(outer, " and ", inner);
}

// Census of the contexts the sheet places each normalized selector in.
// Mirrors ProcessRules' context tracking, but walks EVERY @media block rather
// than only the ones ProcessRules recurses into, so a responsive override of a
// selector is seen even when ProcessRules would emit its @media block
// wholesale. Only consulted by the relaxed force-include fallback.
//
// @supports is transparent for identity (as it is everywhere else in this
// file), so a rule that appears both bare and inside an @supports probe
// collapses to ONE context and both copies are force-included. That is
// accepted, bounded over-inclusion: the two copies are alternatives the
// cascade already resolves.
void CollectSelectorContexts(
    const std::vector<CssRule>& rules, CapabilityMask::Viewport viewport,
    int max_depth, const std::string& layer_path,
    const std::string& media_condition,
    absl::flat_hash_map<std::string, SelectorContexts>& out) {
  for (const auto& rule : rules) {
    if (rule.body.empty()) continue;
    if (IsContainerAtRule(rule.selector)) {
      if (max_depth <= 0) continue;
      bool is_layer = absl::StartsWithIgnoreCase(rule.selector, "@layer");
      std::string inner_layer_path =
          is_layer ? JoinLayer(layer_path, ExtractLayerName(rule.selector))
                   : layer_path;
      CollectSelectorContexts(ParseCssRules(StripOuterBraces(rule.body)),
                              viewport, max_depth - 1, inner_layer_path,
                              media_condition, out);
      continue;
    }
    if (IsMediaPrintRule(rule.selector)) continue;
    if (absl::StartsWithIgnoreCase(rule.selector, "@media")) {
      if (max_depth <= 0) continue;
      if (ShouldExcludeMediaForViewport(rule.selector, viewport)) continue;
      CollectSelectorContexts(
          ParseCssRules(StripOuterBraces(rule.body)), viewport, max_depth - 1,
          layer_path,
          JoinMedia(media_condition, ExtractMediaCondition(rule.selector)),
          out);
      continue;
    }
    if (absl::StartsWith(rule.selector, "@")) continue;
    SelectorContexts& entry = out[NormalizeSelector(rule.selector)];
    entry.layer_paths.insert(layer_path);
    entry.media_conditions.insert(media_condition);
  }
}

}  // namespace

std::string NormalizeSelector(std::string_view selector) {
  std::string_view trimmed = absl::StripAsciiWhitespace(selector);
  std::string out;
  out.reserve(trimmed.size());
  bool in_ws = false;
  for (char c : trimmed) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!in_ws) {
        out += ' ';
        in_ws = true;
      }
    } else {
      out += c;
      in_ws = false;
    }
  }
  return out;
}

CriticalCssExtractor::CriticalCssExtractor() { BuildMeasuredFoldIndex(); }

CriticalCssExtractor::CriticalCssExtractor(CriticalCssConfig config)
    : config_(std::move(config)) {
  BuildMeasuredFoldIndex();
}

CriticalCssExtractor::~CriticalCssExtractor() = default;

void CriticalCssExtractor::BuildMeasuredFoldIndex() {
  for (const std::string& token : config_.measured_above_fold_selectors) {
    // Rejects "", "*", and a bare "#"/"." in one condition. A wildcard token
    // would promote every element in the document to above-the-fold, which is
    // the failure mode this whole change exists to avoid overshooting into.
    if (token.size() < 2) continue;
    std::string value = token.substr(1);
    if (token[0] == '#') {
      measured_fold_ids_.insert(std::move(value));
    } else if (token[0] == '.') {
      measured_fold_classes_.insert(std::move(value));
    }
    // Every other shape — a bare tag, a compound, an image selector such as
    // "img#hero" — is inert on purpose. See measured_above_fold_selectors.
  }
}

bool CriticalCssExtractor::MeasuredFoldContains(
    const CollectedElement& element) const {
  if (!element.id.empty() && measured_fold_ids_.contains(element.id)) {
    return true;
  }
  if (measured_fold_classes_.empty()) return false;
  for (const std::string& cls : element.classes) {
    if (measured_fold_classes_.contains(cls)) return true;
  }
  return false;
}

CriticalCssResult CriticalCssExtractor::Extract(
    const std::vector<CollectedElement>& elements, std::string_view css,
    CapabilityMask::Viewport viewport,
    const absl::flat_hash_set<RuleIdentity>* force_include) {
  CriticalCssResult result;

  if (css.empty()) {
    result.success = true;
    return result;
  }

  // Parse CSS into rules
  std::vector<CssRule> rules = ParseCssRules(css);
  result.total_rules = static_cast<int>(rules.size());

  std::optional<ForceIncludeIndex> index;
  if (force_include != nullptr) {
    index.emplace(BuildForceIncludeIndex(rules, viewport, *force_include));
  }

  int critical_count = 0;
  result.critical_css =
      ProcessRules(rules, elements, viewport, critical_count,
                   kMaxRuleRecursionDepth, /*inside_layer=*/false,
                   /*layer_path=*/"", /*media_condition=*/"",
                   index.has_value() ? &index.value() : nullptr);
  result.critical_rules = critical_count;
  result.success = true;
  return result;
}

bool CriticalCssExtractor::ForceIncludeIndex::MatchesRelaxed(
    const std::string& normalized_selector, const std::string& layer_path,
    const std::string& media_condition) const {
  if (!relaxable.contains(normalized_selector)) return false;
  auto it = sheet_contexts.find(normalized_selector);
  // An absent census entry resolves to no match: the walks bound recursion
  // independently, so the census can stop short of a rule ProcessRules reaches.
  if (it == sheet_contexts.end()) return false;
  const SelectorContexts& contexts = it->second;
  // Cross-layer occurrences are genuine ambiguity. The sole layer must also BE
  // this rule's layer: the two walks bound recursion independently, so the
  // census can have recorded a different single occurrence than the one in
  // hand.
  if (contexts.layer_paths.size() != 1) return false;
  if (*contexts.layer_paths.begin() != layer_path) return false;
  // Prefer the unconditional occurrence, else the only one there is.
  if (contexts.media_conditions.contains("")) return media_condition.empty();
  if (contexts.media_conditions.size() != 1) return false;
  return media_condition == *contexts.media_conditions.begin();
}

CriticalCssExtractor::ForceIncludeIndex
CriticalCssExtractor::BuildForceIncludeIndex(
    const std::vector<CssRule>& rules, CapabilityMask::Viewport viewport,
    const absl::flat_hash_set<RuleIdentity>& force_include) {
  ForceIncludeIndex index(force_include);
  for (const auto& id : force_include) {
    if (id.layer_path.empty() && id.media_condition.empty()) {
      index.relaxable.insert(id.normalized_selector);
    }
  }
  if (!index.relaxable.empty()) {
    CollectSelectorContexts(rules, viewport, kMaxRuleRecursionDepth,
                            /*layer_path=*/"", /*media_condition=*/"",
                            index.sheet_contexts);
  }
  return index;
}

std::string CriticalCssExtractor::ProcessRules(
    const std::vector<CssRule>& rules,
    const std::vector<CollectedElement>& elements,
    CapabilityMask::Viewport viewport, int& critical_count, int max_depth,
    bool inside_layer, const std::string& layer_path,
    const std::string& media_condition, const ForceIncludeIndex* force_index) {
  std::string critical_css;

  for (const auto& rule : rules) {
    // Statement at-rules (no block body): a bare `@layer a,b,c;` layer-order
    // declaration, `@import`, or `@charset`. These carry cascade-ordering /
    // load semantics, not a matchable selector, so preserve them as-is. Emitted
    // in source order, so a leading layer-order statement lands before the rules
    // that reference the layers.
    if (rule.body.empty()) {
      if (absl::StartsWithIgnoreCase(rule.selector, "@layer") ||
          IsAlwaysIncludedAtRule(rule.selector)) {
        if (!critical_css.empty()) {
          critical_css += '\n';
        }
        critical_css += rule.selector;
        critical_css += ";";
      }
      continue;
    }

    // Handle container at-rules (@layer, @supports): recurse into inner rules.
    if (IsContainerAtRule(rule.selector)) {
      if (max_depth <= 0) continue;
      std::string_view inner = StripOuterBraces(rule.body);
      std::vector<CssRule> inner_rules = ParseCssRules(inner);
      // Propagate inside_layer=true when entering a @layer block, and extend
      // the identity layer path. @supports is transparent for identity.
      bool is_layer = absl::StartsWithIgnoreCase(rule.selector, "@layer");
      std::string inner_layer_path =
          is_layer ? JoinLayer(layer_path, ExtractLayerName(rule.selector))
                   : layer_path;
      std::string inner_critical =
          ProcessRules(inner_rules, elements, viewport, critical_count,
                       max_depth - 1, inside_layer || is_layer,
                       inner_layer_path, media_condition, force_index);
      if (!inner_critical.empty()) {
        if (!critical_css.empty()) {
          critical_css += '\n';
        }
        critical_css += rule.selector;
        critical_css += " {\n";
        critical_css += inner_critical;
        critical_css += "\n}";
      }
      continue;
    }

    bool include = false;

    // Skip @media print rules
    if (IsMediaPrintRule(rule.selector)) {
      continue;
    }

    // Always include certain @-rules
    if (IsAlwaysIncludedAtRule(rule.selector)) {
      include = true;
    }

    // Check if we should include this selector
    if (!include) {
      include = ShouldIncludeSelector(rule.selector, elements, viewport);
    }

    // Force-include augmentation: a rule the DOM matcher cannot capture (an
    // attribute selector, a state-conditional variant that never renders under
    // a static JS-off sample) is still emitted when its identity — keyed by the
    // enclosing @layer path + @media condition + normalized selector — is in
    // the force_include set. @-rules never carry a DOM selector, so they are
    // excluded from the identity lookup.
    //
    // A coverage-derived identity has no wrapper context to key on (see
    // ForceIncludeIndex), so a primary-key miss falls back to the uniqueness-
    // guarded relaxed match.
    if (!include && force_index != nullptr &&
        !absl::StartsWith(rule.selector, "@")) {
      std::string normalized = NormalizeSelector(rule.selector);
      RuleIdentity id{layer_path, media_condition, normalized};
      if (force_index->exact.contains(id) ||
          force_index->MatchesRelaxed(normalized, layer_path,
                                      media_condition)) {
        include = true;
      }
    }

    if (include) {
      // When inside a @layer and the rule is a @media block whose body
      // exceeds the wholesale size threshold, recurse into the inner rules
      // and filter per-rule instead of including the entire block.
      if (inside_layer && absl::StartsWithIgnoreCase(rule.selector, "@media") &&
          static_cast<int>(rule.body.size()) >
              config_.max_wholesale_media_bytes &&
          max_depth > 0) {
        std::string_view inner = StripOuterBraces(rule.body);
        std::vector<CssRule> inner_rules = ParseCssRules(inner);
        std::string inner_media =
            JoinMedia(media_condition, ExtractMediaCondition(rule.selector));
        std::string inner_critical = ProcessRules(
            inner_rules, elements, viewport, critical_count, max_depth - 1,
            inside_layer, layer_path, inner_media, force_index);
        if (!inner_critical.empty()) {
          if (!critical_css.empty()) {
            critical_css += '\n';
          }
          critical_css += rule.selector;
          critical_css += " {\n";
          critical_css += inner_critical;
          critical_css += "\n}";
        }
      } else {
        if (!critical_css.empty()) {
          critical_css += "\n";
        }
        critical_css += rule.selector;
        critical_css += " ";
        critical_css += rule.body;
        ++critical_count;
      }
    }
  }

  return critical_css;
}

bool CriticalCssExtractor::ShouldIncludeSelector(
    std::string_view selector, const std::vector<CollectedElement>& elements,
    CapabilityMask::Viewport viewport) {
  // Handle @media rules
  if (absl::StartsWithIgnoreCase(selector, "@media")) {
    if (IsMediaPrintRule(selector)) return false;
    if (ShouldExcludeMediaForViewport(selector, viewport)) return false;
    return true;
  }

  // Check against always-include selectors
  std::string selector_lower = ToLower(selector);
  for (const auto& always_include : config_.always_include_selectors) {
    std::string pattern_lower = ToLower(always_include);
    // Match exactly or as part of a selector list
    if (selector_lower == pattern_lower ||
        selector_lower.find(pattern_lower + ",") != std::string::npos ||
        selector_lower.find(", " + pattern_lower) != std::string::npos ||
        selector_lower.find("," + pattern_lower) != std::string::npos) {
      return true;
    }
  }

  // For universal selector, always include
  if (selector == "*" || selector.find('*') == 0) {
    return true;
  }

  // Check if selector matches any collected element
  return SelectorMatchesAnyElement(selector, elements);
}

bool CriticalCssExtractor::SelectorMatchesAnyElement(
    std::string_view selector, const std::vector<CollectedElement>& elements) {
  // Handle selector lists (comma-separated).
  // Must skip commas inside functional pseudo-classes like :is(h1, h2),
  // :where(), :has(), :not(), etc.
  size_t pos = 0;
  while (pos < selector.size()) {
    // Find next top-level comma (not inside parentheses).
    int paren_depth = 0;
    size_t comma_pos = std::string_view::npos;
    for (size_t i = pos; i < selector.size(); ++i) {
      char c = selector[i];
      if (c == '(') {
        ++paren_depth;
      } else if (c == ')') {
        if (paren_depth > 0) --paren_depth;
      } else if (c == ',' && paren_depth == 0) {
        comma_pos = i;
        break;
      }
    }
    std::string_view single_selector;
    if (comma_pos == std::string_view::npos) {
      single_selector = selector.substr(pos);
      pos = selector.size();
    } else {
      single_selector = selector.substr(pos, comma_pos - pos);
      pos = comma_pos + 1;
    }

    // Trim whitespace
    single_selector = absl::StripAsciiWhitespace(single_selector);
    if (single_selector.empty()) {
      continue;
    }

    // For complex selectors (with spaces, >, +, ~), use the last part.
    // This is a simplification - we check if the final element matches.
    // Use FindLastCombinatorSpace to skip spaces that are part of CSS
    // hex escapes (e.g., `\3a ` where the space is consumed by the escape).
    std::string_view final_selector = single_selector;
    size_t space_pos = FindLastCombinatorSpace(single_selector);
    if (space_pos != std::string_view::npos) {
      final_selector = single_selector.substr(space_pos + 1);
    }
    // Also handle combinators
    for (char combinator : {'+', '>', '~'}) {
      size_t comb_pos = final_selector.rfind(combinator);
      if (comb_pos != std::string_view::npos &&
          comb_pos + 1 < final_selector.size()) {
        final_selector =
            absl::StripAsciiWhitespace(final_selector.substr(comb_pos + 1));
      }
    }

    // Check each element
    for (const auto& element : elements) {
      // Skip excluded elements.
      //
      // NOTE (known interaction, deliberately unchanged here): this runs BEFORE
      // the measured-fold check below, so `max_depth` and the footer/lazy/defer
      // pattern lists still veto an element a browser reported inside the
      // viewport — e.g. a deeply-nested visible element, or a visible one whose
      // class merely contains "defer". The result is under-inclusion, which is
      // the safe direction (that element's rules are simply not treated as
      // critical, exactly as today), so measured evidence is not given the power
      // to overrule an explicit exclusion in this change. A follow-up issue
      // tracks whether it should.
      if (IsElementExcluded(element)) {
        continue;
      }

      // Check if element is within our "above the fold" criteria.
      // IsElementExcluded already filtered depth > max_depth, so the
      // depth check here is implicit.  An element is critical if it's
      // among the first N elements OR explicitly included (a measured
      // above-the-fold descriptor, or the header/nav/hero heuristics).
      bool is_critical = element.element_index < config_.max_elements ||
                         IsElementIncluded(element);

      if (!is_critical) {
        continue;
      }

      if (SimpleSelectorMatchesElement(final_selector, element)) {
        return true;
      }
    }
  }

  return false;
}

bool CriticalCssExtractor::SimpleSelectorMatchesElement(
    std::string_view selector, const CollectedElement& element) {
  if (selector.empty()) {
    return false;
  }

  // Universal selector matches everything
  if (selector == "*") {
    return true;
  }

  std::string selector_str(selector);
  size_t pos = 0;

  // Parse the simple selector into components
  std::string tag_name;
  std::string id;
  std::vector<std::string> classes;

  // Tailwind v4 (and any framework using `@custom-variant`) scopes variant
  // rules with zero-specificity `:where()`/`:is()` wrappers, e.g.
  // `.dark\:bg-stone-900:where(.dark, .dark *)`. Per CSS semantics `:where()`
  // has zero specificity and is a scoping hint, not a gating predicate — so we
  // must treat it as non-constraining for retention. Two cases:
  //   1. `A:where(B)` — when the outer part `A` matches, `:where(B)` is ignored
  //      (`saw_where_is` lets the final check skip the broken-off pseudo).
  //   2. `:where(B)` / `:is(B)` with no matching outer part — desugar by
  //      evaluating the inner alternatives so the rule is retained when any
  //      inner alternative matches (rescues leading-pseudo selectors).
  bool saw_where_is = false;
  bool where_is_inner_matched = false;

  while (pos < selector_str.size()) {
    char c = selector_str[pos];

    if (c == '#') {
      // ID selector
      size_t end = pos + 1;
      while (
          end < selector_str.size() &&
          ((std::isalnum(static_cast<unsigned char>(selector_str[end])) != 0) ||
           selector_str[end] == '-' || selector_str[end] == '_' ||
           selector_str[end] == '\\')) {
        if (selector_str[end] == '\\' && end + 1 < selector_str.size()) {
          ++end;  // skip backslash
          SkipCssEscape(selector_str, end);
        } else {
          ++end;
        }
      }
      id = UnescapeCssIdent(selector_str.substr(pos + 1, end - pos - 1));
      pos = end;
    } else if (c == '.') {
      // Class selector
      size_t end = pos + 1;
      while (
          end < selector_str.size() &&
          ((std::isalnum(static_cast<unsigned char>(selector_str[end])) != 0) ||
           selector_str[end] == '-' || selector_str[end] == '_' ||
           selector_str[end] == '\\')) {
        if (selector_str[end] == '\\' && end + 1 < selector_str.size()) {
          ++end;  // skip backslash
          SkipCssEscape(selector_str, end);
        } else {
          ++end;
        }
      }
      classes.push_back(
          UnescapeCssIdent(selector_str.substr(pos + 1, end - pos - 1)));
      pos = end;
    } else if (c == ':') {
      // Pseudo-class/element. `:where(...)`/`:is(...)` are zero-specificity
      // scoping wrappers (CSS Selectors Level 4); desugar them rather than
      // dropping the rule.
      std::string_view rest(selector_str);
      rest = rest.substr(pos);
      bool is_where = absl::StartsWithIgnoreCase(rest, ":where(");
      bool is_is = !is_where && absl::StartsWithIgnoreCase(rest, ":is(");
      if (!is_where && !is_is) {
        // Any other pseudo (`:hover`, `:focus`, `::before`, `:nth-child(2)`,
        // ...): drop its constraint but keep parsing the rest of the compound,
        // so a following `:where()`/`:is()` wrapper — which may hold the only
        // DOM-matchable token (e.g. `:hover:where(.foo)`) — is still desugared.
        // Skip the pseudo name and any functional `(...)` argument; the outer
        // tag/id/class decision below is unchanged for the common `a:hover`
        // case (the pseudo simply contributes nothing).
        size_t p = pos + 1;  // past ':'
        if (p < selector_str.size() && selector_str[p] == ':') {
          ++p;  // pseudo-element `::`
        }
        while (
            p < selector_str.size() &&
            ((std::isalnum(static_cast<unsigned char>(selector_str[p])) != 0) ||
             selector_str[p] == '-' || selector_str[p] == '_')) {
          ++p;
        }
        if (p < selector_str.size() && selector_str[p] == '(') {
          int pd = 0;
          for (; p < selector_str.size(); ++p) {
            if (selector_str[p] == '(') {
              ++pd;
            } else if (selector_str[p] == ')') {
              if (--pd == 0) {
                ++p;  // consume the closing ')'
                break;
              }
            }
          }
        }
        pos = p;
        continue;
      }
      // Locate the matching close paren for the `:where(` / `:is(` opener.
      size_t open_paren = selector_str.find('(', pos);
      int paren_depth = 0;
      size_t close_paren = std::string::npos;
      for (size_t i = open_paren; i < selector_str.size(); ++i) {
        if (selector_str[i] == '(') {
          ++paren_depth;
        } else if (selector_str[i] == ')') {
          --paren_depth;
          if (paren_depth == 0) {
            close_paren = i;
            break;
          }
        }
      }
      if (close_paren == std::string::npos) {
        // Malformed (unbalanced) — give up on this pseudo conservatively.
        break;
      }
      saw_where_is = true;
      // Evaluate the inner selector list. `:where()`/`:is()` accept a
      // forgiving selector list; the rule applies if ANY alternative matches,
      // so recurse on the last compound part of each alternative.
      std::string_view inner = selector_str;
      inner = inner.substr(open_paren + 1, close_paren - open_paren - 1);
      size_t inner_pos = 0;
      while (inner_pos < inner.size()) {
        int depth = 0;
        size_t comma = std::string_view::npos;
        for (size_t i = inner_pos; i < inner.size(); ++i) {
          char ic = inner[i];
          if (ic == '(') {
            ++depth;
          } else if (ic == ')') {
            if (depth > 0) --depth;
          } else if (ic == ',' && depth == 0) {
            comma = i;
            break;
          }
        }
        std::string_view alt;
        if (comma == std::string_view::npos) {
          alt = inner.substr(inner_pos);
          inner_pos = inner.size();
        } else {
          alt = inner.substr(inner_pos, comma - inner_pos);
          inner_pos = comma + 1;
        }
        alt = absl::StripAsciiWhitespace(alt);
        if (alt.empty()) {
          continue;
        }
        // For a complex inner alternative (descendant/combinator, e.g.
        // `.dark *`), match on its rightmost compound part, mirroring
        // SelectorMatchesAnyElement.
        size_t alt_space = FindLastCombinatorSpace(alt);
        if (alt_space != std::string_view::npos) {
          alt = alt.substr(alt_space + 1);
        }
        for (char combinator : {'+', '>', '~'}) {
          size_t comb = alt.rfind(combinator);
          if (comb != std::string_view::npos && comb + 1 < alt.size()) {
            alt = absl::StripAsciiWhitespace(alt.substr(comb + 1));
          }
        }
        // A bare universal (`*`) inside :where()/:is() matches any element.
        if (alt == "*" || SimpleSelectorMatchesElement(alt, element)) {
          where_is_inner_matched = true;
        }
      }
      // Continue parsing after the wrapper (e.g. `A:where(B):is(C)`), so the
      // outer part decision below still sees any tag/id/class on either side.
      pos = close_paren + 1;
    } else if (c == '[') {
      // Attribute selector - skip for now (would need more complex matching).
      break;
    } else if ((std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '-' ||
               c == '_' || c == '\\') {
      // Tag name
      size_t end = pos;
      while (
          end < selector_str.size() &&
          ((std::isalnum(static_cast<unsigned char>(selector_str[end])) != 0) ||
           selector_str[end] == '-' || selector_str[end] == '_' ||
           selector_str[end] == '\\')) {
        if (selector_str[end] == '\\' && end + 1 < selector_str.size()) {
          ++end;  // skip backslash
          SkipCssEscape(selector_str, end);
        } else {
          ++end;
        }
      }
      tag_name = UnescapeCssIdent(selector_str.substr(pos, end - pos));
      pos = end;
    } else {
      ++pos;
    }
  }

  // Match against element
  // Tag name must match (case-insensitive)
  if (!tag_name.empty()) {
    if (!absl::EqualsIgnoreCase(tag_name, element.tag_name)) {
      return false;
    }
  }

  // ID must match exactly
  if (!id.empty()) {
    if (id != element.id) {
      return false;
    }
  }

  // All classes must be present
  for (const auto& cls : classes) {
    bool found = false;
    for (const auto& elem_cls : element.classes) {
      if (cls == elem_cls) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  // At least one component must have matched. The outer simple selector
  // (tag/id/classes) gated above; if it is present and non-empty it matched,
  // so the rule is retained regardless of any `:where()`/`:is()` wrapper
  // (zero-specificity scoping is non-constraining for retention).
  if (!tag_name.empty() || !id.empty() || !classes.empty()) {
    return true;
  }

  // No matchable outer part (e.g. a selector that leads with `:where(...)` /
  // `:is(...)`): retain the rule if any inner alternative of the desugared
  // `:where()`/`:is()` matched. This is what rescues Tailwind v4 dark-mode
  // rules whose only matchable token lives inside the scoping wrapper.
  return saw_where_is && where_is_inner_matched;
}

bool CriticalCssExtractor::ContainsPattern(
    std::string_view str, const std::vector<std::string>& patterns) {
  std::string str_lower = ToLower(str);
  for (const auto& pattern : patterns) {
    std::string pattern_lower = ToLower(pattern);
    if (str_lower.find(pattern_lower) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool CriticalCssExtractor::IsElementExcluded(const CollectedElement& element) {
  // Check depth
  if (element.depth > config_.max_depth) {
    return true;
  }

  // Check tag name patterns
  if (ContainsPattern(element.tag_name, config_.exclude_tag_patterns)) {
    return true;
  }

  // Check ID patterns
  if (!element.id.empty() &&
      ContainsPattern(element.id, config_.exclude_id_patterns)) {
    return true;
  }

  // Check class patterns
  for (const auto& cls : element.classes) {
    if (ContainsPattern(cls, config_.exclude_class_patterns)) {
      return true;
    }
  }

  return false;
}

bool CriticalCssExtractor::IsElementIncluded(const CollectedElement& element) {
  // Measured evidence first. The patterns below are guesses about which markup
  // tends to sit at the top of a page; this is a browser reporting that it laid
  // this element out inside the viewport.
  if (MeasuredFoldContains(element)) {
    return true;
  }

  // Check tag name patterns
  if (ContainsPattern(element.tag_name, config_.include_tag_patterns)) {
    return true;
  }

  // Check ID patterns
  if (!element.id.empty() &&
      ContainsPattern(element.id, config_.include_id_patterns)) {
    return true;
  }

  // Check class patterns
  for (const auto& cls : element.classes) {
    if (ContainsPattern(cls, config_.include_class_patterns)) {
      return true;
    }
  }

  return false;
}

bool CriticalCssIsSufficient(float coverage_ratio, size_t critical_css_bytes,
                             size_t deferred_css_bytes,
                             bool external_css_unresolved,
                             const AsyncCssSufficiencyConfig& cfg) {
  // Fail-safe (dominant): a declared external stylesheet was not resolved from
  // cache, so the bytes we would defer are UNMEASURED and the inlined critical
  // CSS was derived WITHOUT that sheet. `deferred_css_bytes` here is the cold,
  // inline-only blob (not the real sheet), which would otherwise trip the
  // small-sheet escape hatch below and defer an unmeasured sheet -> FOUC.
  // Never defer a sheet we could not measure; keep it render-blocking. The
  // caller marks the variant for revalidation, so async re-enables once the
  // sheet caches and a real decision can be made.
  if (external_css_unresolved) return false;
  // Gate disabled -> legacy behavior (always defer when critical CSS exists).
  if (cfg.min_coverage_ratio <= 0.0f) return true;
  // A small sheet has a trivial FOUC window even with thin critical CSS.
  if (deferred_css_bytes < cfg.min_deferred_css_bytes) return true;
  if (deferred_css_bytes == 0) return true;
  // The byte ratio is a hard floor: it measures what was ACTUALLY inlined
  // against what would ACTUALLY be deferred. A browser profile's rule-level
  // coverage_ratio is measured pre-extraction and can wildly contradict the
  // extracted bytes (live incident: coverage=0.39 claimed while 830 B of
  // critical CSS deferred a 115 KB sheet -> FOUC). Gate on the pessimistic of
  // the two; unknown (negative) or NaN coverage leaves the byte ratio alone.
  float ratio = static_cast<float>(critical_css_bytes) /
                static_cast<float>(deferred_css_bytes);
  if (coverage_ratio >= 0.0f && coverage_ratio < ratio) {
    ratio = coverage_ratio;
  }
  return ratio >= cfg.min_coverage_ratio;
}

}  // namespace pagespeed
