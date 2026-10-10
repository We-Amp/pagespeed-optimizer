# Packs Library

Loader, matching model and head-rule filter for declarative transform packs
(JSON). The loader, matcher and templates are pure functions over strings; the
filter (`pack_filter`) is the only part that touches HTML. Nothing here is
wired into the worker yet.

## Key Files
- `pack.h` -- model structs (`Pack`, `Site`, `Rule`, `RuleValue`), every limit as a named `constexpr`
- `pack_loader.h` -- `LoadPack()` / `LoadPackFile()`: strict parse and validation; errors read `<file>: <json path>: <reason>`
- `matcher.h` -- host matching (`FindSite`), path globs and RE2 (`MatchRulePath`), per-kind precedence (`SelectRules`), table and hreflang-cluster lookup
- `template.h` -- placeholder templates: `ParseTemplate()`, `ExpandTemplate()` with per-context escaping and a size cap
- `url_norm.h` -- `NormUrl()` equality normalization, `NormalizeHreflangCode()`

- `page_facts.h` -- what the traversal collects (element handles, values)
- `planner.h` -- `BuildPlan()`: pure facts-in, plan-out; one `PackDecision` per rule (canonical, title, description)
- `decision.h` -- `PackDecision` (values as hashes only) and the JSONL line encoder
- `pack_filter.h` -- `PackFilter`, an `EmptyHtmlFilter`: collects facts while streaming, decides and mutates at `EndDocument`

## Testing
```bash
bazel test //test/lib/packs/...
# regenerate the golden files after an intended behavior change; review the diff
bazel run //tools/packs:fixture_runner -- --update "$PWD/packs/edge-seo/fixtures"
```

## Gotchas
- Unknown keys, duplicate keys and unknown `{placeholders}` are load errors on purpose.
- JSON objects are unordered: hreflang entries are sorted by code.
- A `{` that does not open a `{identifier}` token is literal (JSON templates need no escaping).
- - The example pack lives in `packs/edge-seo/pack.example.json`; the loader test loads it against the real product version.
- A rule-level `exclude_paths` REPLACES `defaults.exclude_paths` (likewise `paths`); it does not add to them.
- `/shop/**` requires the slash: it matches `/shop/` and below, not `/shop`.
- jsonld rules take a template only (no table); an unknown or out-of-range `{n}` capture group is a load error.
- Globs and `path_regex` are compiled as Latin-1, so matching is byte-exact (`.` matches `\xff`); a non-ASCII pattern is matched as its UTF-8 bytes.
- Limits: 32 globs per list, 2000 distinct globs per pack; a rule's globs run as one RE2::Set.
- Table values that become title or description text must go through `EscapeForHtmlText()`; `ExpandTemplate(kHtmlText)` escapes the whole output.
- `PackFilter` mutates only at `EndDocument` and relies on the whole document being one flush window (`ParseText` then `FinishParse`). If any node it must touch is not rewritable it drops the whole plan (`not_rewritable`).
- Report-only (effective mode below enforce) records decisions and leaves the document byte-identical; `modified()` stays false so the caller can skip the cache write.
- The filter never sees the device class; the fixture runner proves the output is the same whatever else follows it in the pass.
- `HtmlKeywords::Init()` must have run before entity decoding; `BuildPlan()` calls it (idempotent).
- Elements inside svg, math, template, noscript, noembed and noframes are ignored; a page without an explicit `<head>` is skipped.
- Decision `old`/`new` are FNV-1a hashes of the normalized values, never text.
- Attribute values: the kernel's decoded value is empty for non-ASCII bytes and for entities such as `&eacute;`. The filter reads `EscapedAttributeValue()` and decodes it with `SafeDecodeHtmlEntities()`, and writes with `AddEscapedAttribute`/`SetEscapedValue` after escaping only `& < > \" '` (`EscapeAttributeValue`), so UTF-8 survives. An entity-encoded value that `SafeDecodeHtmlEntities` cannot decode to UTF-8 (`&eacute;`) compares as written, so under `replace` it is rewritten even if it means the same text.
- Every pack write sets the attribute's quote style to double quotes; an unquoted attribute otherwise stays unquoted around a value that may hold spaces or quotes.
- An element with other `rel` tokens (`rel="canonical alternate"`) is never rewritten in place: the `canonical` token is removed and a new element is inserted.
- A `<head>` the source never closes ends at `<body>` or the first element that is not head content; inserts go before that element. If an inert element (svg, noscript, ...) is still open there, the page is skipped (`malformed_head`).
- The kernel re-serializes some tag whitespace (`<head  >`, `<TITLE >`). When `modified()` is false the caller must serve the original bytes, not the filter's output.
