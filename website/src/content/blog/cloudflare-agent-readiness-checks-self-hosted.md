---
title: "Cloudflare's agent-readiness checks on your own server"
description: 'Line by line: what each isitagentready.com check looks for, how rare a pass is, and what a server you run yourself can do about it, without a CDN.'
date: 2026-10-21
author: 'Otto van der Schaaf'
tags: ['ai-readability', 'ai-agents', 'nginx', 'apache', 'robots-txt']
draft: true
---

Markdown content negotiation passes on 3.9% of the 200,000 most visited domains, after Cloudflare filtered out categories such as redirects and ad servers. That is its Radar number, published on 2026-04-17 with its Agent Readiness score. If [isitagentready.com](https://isitagentready.com/) showed you a column of fails, you are in the majority.

Most fixes are a file or a header, and none below needs a CDN. Line by line: what each check in Cloudflare's scan looks for, and what your own server can do about it.

## What the score is, and what it is not

The announcement (2026-04-17) scores four dimensions:

- **Discoverability:** robots.txt, sitemap.xml, Link headers (RFC 8288).
- **Content:** Markdown for Agents, meaning a Markdown response to `Accept: text/markdown`.
- **Bot access control:** Content Signals, AI bot rules in robots.txt, Web Bot Auth.
- **Capabilities:** Agent Skills, API Catalog, OAuth server discovery, MCP Server Card, WebMCP.

It also checks agentic commerce standards, "but these do not currently count towards the score." And it leaves out a file many guides lead with: "By default, we only check whether the site correctly handles Markdown content negotiation, and do not check for llms.txt."

The test page, as read on 2026-10-13, lists checks the announcement did not name, such as DNS for AI Discovery, OAuth Protected Resource, Auth.md, ARD and MPP; the "do not count" quote named only x402, UCP and ACP. Its scan options list three site types: "All Checks", "Content Site" and "API / Application".

Radar's baseline from the announcement, same filtered set:

| Check | In the dataset |
| --- | ---: |
| robots.txt present | 78% |
| Content Signals declared | 4% |
| Markdown content negotiation | 3.9% |
| MCP Server Cards and API Catalogs, together | fewer than 15 sites in the entire dataset |

## Discoverability: robots.txt, sitemap, Link headers

Serve a `robots.txt` at the root, list your sitemap in it, and serve the sitemap. If your CMS generates a sitemap, the only work is the `Sitemap:` line.

RFC 8288 (October 2017) puts links in the HTTP response, so an agent finds a resource without parsing your HTML. The announcement's one example points at an API catalog:

```http
Link: </.well-known/api-catalog>; rel="api-catalog"
```

`api-catalog` is the only rel value the announcement names; it does not say which others count. Point a Link header only at something you publish. On nginx:

```nginx
add_header Link '</.well-known/api-catalog>; rel="api-catalog"' always;
```

Put it in every location that sets its own add_header, or it is dropped there.

On Apache, with mod_headers:

```apache
Header add Link "</.well-known/api-catalog>; rel=\"api-catalog\""
```

IIS can send the same header from its configuration. DNS for AI Discovery is a DNS record, not a server setting.

## Content: Markdown on `Accept: text/markdown`

An agent asks for `text/markdown`; the server answers with a Markdown copy of the same page at the same URL, plus `Vary: Accept`, which tells a standards-following shared cache to store the Markdown and HTML copies separately. Check that your CDN honours `Vary: Accept` on HTML; some ignore it or stop caching. The announcement reports "up to 80% token reduction in some cases."

The three routes, a static `.md` per page, the mod_pagespeed 2.1 optimizer worker's experimental rendered copy, or Cloudflare's toggle, are in [Accept: text/markdown on nginx and Apache](/blog/accept-text-markdown-nginx-apache/), with tested configs.

Negotiation fixes the format, not an empty page. In [our scan of the top 1,000 sites](/blog/can-ai-read-your-website/) (2026-05-28), 403 of the 711 we could read, 57%, showed a non-rendering agent a nearly empty page. Markdown for Agents converts the HTML your origin sends; for pages that need a browser, Cloudflare's docs point to a separate Browser Run /markdown endpoint. A static export of a JavaScript-only page is empty too; the worker renders the DOM before it converts.

## Bot access control: Content Signals, AI bot rules, Web Bot Auth

Cloudflare's Content Signals Policy (2025-09-24) defines three signals, `search`, `ai-input` and `ai-train`, set in robots.txt. The agent-readiness announcement shows the directive:

```text
User-agent: *
Content-Signal: ai-train=no, search=yes, ai-input=yes
```

Pick your own values; the 2025 policy post also has comment lines that explain them to a human reader.

AI bot rules are ordinary `User-agent` groups in the same file, one per crawler you want to treat differently. Neither enforces anything.

Web Bot Auth is an IETF draft built on RFC 9421 HTTP Message Signatures: a bot signs its requests with an Ed25519 key and publishes the public key. The scan checks `/.well-known/http-message-signatures-directory`, where a bot operator publishes those keys, and Cloudflare says: "If your site just serves content, and doesn't make requests to other sites, you don't need it."

For a site that receives bot traffic, checking incoming signatures is the useful half. The mod_pagespeed module on nginx and the optimizer worker include an experimental, observe-only verifier, off by default, that checks each signature and labels the request so you can log the verdict ([how it works](/blog/verify-ai-crawlers-web-bot-auth/), 2026-06-22). It tells you which bots are real; it does not turn the scan's line green.

## The lines a content site will not pass

MCP Server Card, A2A Agent Card, Agent Skills, API Catalog, OAuth discovery, Auth.md and ARD are well-known files or JSON documents describing an API, a tool server or a sign-in flow; WebMCP is a browser-side API, and the commerce lines are payment and checkout flows, x402 reviving HTTP 402 Payment Required, as the announcement puts it. If you have those, publish them; a file advertising a missing API is worse than a fail. Radar found MCP Server Cards and API Catalogs together on fewer than 15 sites in the entire dataset.

## Check what an agent reads

Run the free [AI readability check](/ai-readability/) on a page. It fetches the page the way a non-rendering agent does, renders it in a real browser too, compares what each can read, and grades the result. It does not run Cloudflare's checks. Its agent-signals section, which does not count toward the grade, shows whether the page answers `Accept: text/markdown` with Markdown, whether robots.txt carries a Content-Signal line, and whether the response sends a Link header. To check one URL by hand, use curl:

```sh
curl -sI -H 'Accept: text/markdown' https://example.com/page/
```

`Content-Type: text/markdown` and `Vary: Accept` in the response are what that line asks for.

Our [agent-readiness audit](/pricing/#audit) costs €950 per site, excl. VAT, and comes with one written report and one review call. The audit checks technical readiness and verifies the fixes with a rescan; it does not measure how often AI answers mention you.

Start with robots.txt: add the `Sitemap:` line and a `Content-Signal:` line, then run Cloudflare's scan again.

---

**Sources** (all read on 2026-10-13)

- "Introducing the Agent Readiness score. Check to see if your site is agent-ready", Cloudflare blog, 2026-04-17: https://blog.cloudflare.com/agent-readiness/
- "Is Your Site Agent-Ready?", isitagentready.com, as read on 2026-10-13: https://isitagentready.com/
- "Giving users choice with Cloudflare's new Content Signals Policy", Cloudflare blog, 2025-09-24: https://blog.cloudflare.com/content-signals-policy/
- RFC 8288, Web Linking, IETF, October 2017: https://www.rfc-editor.org/rfc/rfc8288
- RFC 9421, HTTP Message Signatures, IETF, February 2024: https://www.rfc-editor.org/rfc/rfc9421

**Related:** [Accept: text/markdown on nginx and Apache](/blog/accept-text-markdown-nginx-apache/)
· [Can AI read your website?](/blog/can-ai-read-your-website/)
· [Web Bot Auth at your origin](/blog/verify-ai-crawlers-web-bot-auth/)
· [The agentic web at the origin](/blog/agentic-web-at-the-origin/)
