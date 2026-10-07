# Examples gallery data generator

Generates the measured before/after data behind the live optimization gallery at
[modpagespeed.com/examples/](https://modpagespeed.com/examples/).

## What it does

`generate.mjs` reads the catalog in [`website/src/data/examples.ts`](../../website/src/data/examples.ts)
and, for each example, fetches the demo page from the live Apache + mod_pagespeed
1.1 backend twice:

- **before** — `…/<page>?PageSpeed=off` (optimization off)
- **after** — `…/<page>?PageSpeedFilters=<filter>` (only that filter)

It then captures a source diff and measures the byte/request impact, writing
[`website/src/data/examples-data.json`](../../website/src/data/examples-data.json),
which the Astro pages read at build time. The Astro pages render the live
before/after iframes regardless; this committed data adds the source diff and the
measured-impact tiles.

## When to run

Run it **after the demo host has the example pages deployed** and **before
building/deploying the website**, then commit the regenerated
`examples-data.json`.

It also doubles as a **cache warm-up**: resource-rewriting filters are async on a
cold cache (the first request for a novel filter set returns pass-through HTML),
so the run polls each "after" URL until it settles, which leaves the demo
backend's cache warm for real visitors.

## Usage

Requires Node ≥ 22 (uses native fetch + TypeScript import of the catalog).

```bash
cd pagespeed-optimizer   # the repository root
node tools/examples-gallery/generate.mjs                       # all examples
node tools/examples-gallery/generate.mjs --only combine_css,rewrite_images
DEMO_ORIGIN=https://demo-httpd-1.1.modpagespeed.com node tools/examples-gallery/generate.mjs
```

`--only` merges into the existing JSON (partial runs don't wipe other entries).

## Output shape

```jsonc
{
  "generated_at": "ISO-8601",
  "origin": "https://demo-httpd-1.1.modpagespeed.com",
  "examples": {
    "combine_css": {
      "settled": true,         // false if the async rewrite never settled
      "attempts": 2,
      "before": { "html": "…", "htmlBytes": 0, "requestCount": 0, "resourceBytes": 0, … },
      "after":  { "html": "…", "htmlBytes": 0, "requestCount": 0, "resourceBytes": 0, … },
      "savings": { "htmlBytesPct": 0, "requests": 0, "resourceBytesPct": 0, "totalBytesPct": 0 },
      "diff": [{ "t": "-|+| |@", "s": "line" }]
    }
  }
}
```

Entries that error or never settle still render from whatever was captured; the
gallery degrades to live-iframes-only for those.
