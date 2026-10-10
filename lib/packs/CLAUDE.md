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
- The example pack lives in `packs/examples/edge-seo.json` and is loaded by the loader test.
