# Packs Library

Loader and matching model for declarative transform packs (JSON). Leaf
library: pure functions over strings, no HTML parsing, no wiring into the
worker.

## Key Files
- `pack.h` -- model structs (`Pack`, `Site`, `Rule`, `RuleValue`), every limit as a named `constexpr`
- `pack_loader.h` -- `LoadPack()` / `LoadPackFile()`: strict parse and validation; errors read `<file>: <json path>: <reason>`
- `matcher.h` -- host matching (`FindSite`), path globs and RE2 (`MatchRulePath`), per-kind precedence (`SelectRules`), table and hreflang-cluster lookup
- `template.h` -- placeholder templates: `ParseTemplate()`, `ExpandTemplate()` with per-context escaping and a size cap
- `url_norm.h` -- `NormUrl()` equality normalization, `NormalizeHreflangCode()`

## Testing
```bash
bazel test //test/lib/packs/...
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
