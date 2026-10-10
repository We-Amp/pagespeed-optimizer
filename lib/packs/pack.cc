// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/packs/pack.h"

#include <algorithm>

#include "absl/strings/str_cat.h"
#include "lib/packs/url_norm.h"

namespace pagespeed::packs {

std::string_view KindName(Kind kind) {
  switch (kind) {
    case Kind::kCanonical:
      return "canonical";
    case Kind::kTitle:
      return "title";
    case Kind::kDescription:
      return "description";
    case Kind::kHreflang:
      return "hreflang";
    case Kind::kJsonLd:
      return "jsonld";
  }
  return "";
}

std::string_view ModeName(Mode mode) {
  return mode == Mode::kEnforce ? "enforce" : "report";
}

std::string_view OnPresentName(OnPresent on_present) {
  switch (on_present) {
    case OnPresent::kKeep:
      return "keep";
    case OnPresent::kRepair:
      return "repair";
    case OnPresent::kReplace:
      return "replace";
  }
  return "";
}

Mode EffectiveMode(Mode global, Mode site, bool rule_enforce) {
  const Mode rule = rule_enforce ? Mode::kEnforce : Mode::kReport;
  return std::min({global, site, rule});
}

absl::Status CheckUrlValue(std::string_view value) {
  if (value.empty()) return absl::InvalidArgumentError("URL is empty");
  if (value.size() > kMaxUrlBytes) {
    return absl::InvalidArgumentError(
        absl::StrCat("URL is longer than ", kMaxUrlBytes, " bytes"));
  }
  for (char c : value) {
    if (static_cast<unsigned char>(c) <= 0x20 ||
        static_cast<unsigned char>(c) == 0x7f) {
      return absl::InvalidArgumentError(
          "URL contains whitespace or a control character");
    }
  }
  if (!NormUrl(value).has_value()) {
    return absl::InvalidArgumentError(
        "URL must be absolute (http:// or https://) with a host");
  }
  return absl::OkStatus();
}

size_t Utf8Length(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

}  // namespace pagespeed::packs
