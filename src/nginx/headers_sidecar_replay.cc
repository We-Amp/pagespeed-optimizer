// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/nginx/headers_sidecar_replay.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "lib/cache/headers_sidecar.h"
#include "lib/classify/alternate_metadata.h"
#include "src/nginx/headers_sidecar_record.h"

namespace pagespeed {

namespace {

bool NameIn(std::span<const std::string_view> names, std::string_view name) {
  for (std::string_view candidate : names) {
    if (HeaderNameEquals(candidate, name)) return true;
  }
  return false;
}

constexpr uint8_t kClassifiedAndRequired =
    AlternateMetadata::kFlagHeadersClassified |
    AlternateMetadata::kFlagHeadersSidecarRequired;

}  // namespace

bool SidecarReadRequired(uint8_t entry_flags, bool agent_markdown) {
  return !agent_markdown &&
         (entry_flags & kClassifiedAndRequired) == kClassifiedAndRequired;
}

bool SidecarFieldCollidesWithServePath(std::string_view name) {
  return NameIn(HeadersReproducedByEntryMetadata(), name) ||
         NameIn(HeadersStampedPerResponse(), name);
}

SidecarServePlan PlanSidecarServe(uint8_t entry_flags, bool agent_markdown,
                                  std::optional<std::string_view> payload) {
  SidecarServePlan plan;
  if (!SidecarReadRequired(entry_flags, agent_markdown)) {
    plan.outcome = SidecarServeOutcome::kNotRequired;
    return plan;
  }
  std::optional<std::vector<HeaderField>> parsed;
  if (payload.has_value()) {
    parsed = ParseHeadersSidecar(*payload);
  }
  if (!parsed.has_value() || parsed->empty()) {
    // The gate never stores an empty block, so an empty parse is not "nothing
    // to carry": it is a block this entry cannot be served with.
    plan.outcome = SidecarServeOutcome::kMissing;
    return plan;
  }
  for (const HeaderField& field : *parsed) {
    // By construction the gate never stores a name the serve path builds
    // (Cache-Control, validators, Expires, Content-*, Date, Age, ...): the
    // builder owns those and the block owns the rest, so the two cannot
    // produce conflicting lines.  A block that breaks this did not come from
    // the gate (a damaged or foreign cache file).  Every build refuses to
    // serve with it — the request becomes a MISS, the caller logs it, and the
    // record arm rewrites the block from the origin.  Deliberately not an
    // assert: a damaged file on disk must not take a debug server down.
    if (SidecarFieldCollidesWithServePath(field.name)) {
      plan.outcome = SidecarServeOutcome::kMissing;
      plan.fields.clear();
      plan.carried_vary = false;
      plan.carried_nosniff = false;
      plan.foreign_field = true;
      return plan;
    }
    if (HeaderNameEquals(field.name, "vary")) {
      plan.carried_vary = true;
    } else if (HeaderNameEquals(field.name, "x-content-type-options")) {
      plan.carried_nosniff = true;
    }
  }
  plan.fields = std::move(*parsed);
  plan.outcome = SidecarServeOutcome::kReplay;
  return plan;
}

bool ComputedVaryListsAccept(bool entry_varies_accept,
                             bool sidecar_carried_vary) {
  return entry_varies_accept && !sidecar_carried_vary;
}

bool ConditionalRevalidationAllowed(uint8_t entry_flags, bool expired_by_age) {
  const bool classified =
      (entry_flags & AlternateMetadata::kFlagHeadersClassified) != 0;
  return classified || !expired_by_age;
}

bool IsSidecarCarriedName(std::string_view name) {
  return NameIn(HeadersSidecarAllowlist(), name);
}

bool SidecarLinkCarriesTarget(std::span<const HeaderField> fields,
                              std::string_view url) {
  for (const HeaderField& field : fields) {
    if (!HeaderNameEquals(field.name, "link")) continue;
    std::string_view rest = field.value;
    while (!rest.empty()) {
      // A link-value starts with `<target>`; skip to the next link-value
      // after it (the next comma outside the angle brackets).
      size_t open = rest.find('<');
      if (open == std::string_view::npos) break;
      size_t close = rest.find('>', open + 1);
      if (close == std::string_view::npos) break;
      if (rest.substr(open + 1, close - open - 1) == url) return true;
      size_t comma = rest.find(',', close + 1);
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1);
    }
  }
  return false;
}

bool StaleErrorDropsCarriedNames(uint8_t entry_flags) {
  return (entry_flags & AlternateMetadata::kFlagHeadersClassified) != 0;
}

bool StaleErrorServeHasHeaders(uint8_t entry_flags, bool have_block) {
  return have_block || !SidecarReadRequired(entry_flags,
                                            /*agent_markdown=*/false);
}

}  // namespace pagespeed
