---
title: "Pass Cloudflare's agent-readiness checks on your own server"
description: 'What each isitagentready.com check looks for, how rare a pass is, and what nginx, Apache or IIS can do about each line without a CDN.'
date: 2026-10-21
author: 'Otto van der Schaaf'
tags: ['ai-readability', 'ai-agents', 'nginx', 'apache', 'robots-txt']
draft: true
---

Markdown content negotiation passes on 3.9% of the 200,000 most visited domains. That is Cloudflare's Radar number, published on 2026-04-17 with its Agent Readiness score. If you ran [isitagentready.com](https://isitagentready.com/) and saw a column of fails, you are in the majority.

The test is free and needs no account. Most fixes are a file or a header, and none needs a CDN. Below, line by line: what each check looks for, and what nginx, Apache or IIS can do about it today.

## What the score is, and what it is not

The announcement (2026-04-17) scores four dimensions:

- **Discoverability:** robots.txt, sitemap.xml, Link headers (RFC 8288).
- **Content:** Markdown for Agents, meaning a Markdown response to `Accept: text/markdown`.
- **Bot access control:** Content Signals, AI bot rules in robots.txt, Web Bot Auth.
- **Capabilities:** Agent Skills, API Catalog, OAuth server discovery, MCP Server Card, WebMCP.

It also checks agentic commerce standards, "but these do not currently count towards the score." And it leaves out a file many guides lead with: "By default, we only check whether the site correctly handles Markdown content negotiation, and do not check for llms.txt."

The test page, as read on 2026-10-13, has grown since. It lists five categories: Discoverability (adding DNS for AI Discovery), Content Accessibility, Bot Access Control, Protocol Discovery (adding OAuth Protected Resource, Auth.md and ARD) and Commerce (x402, MPP, UCP, ACP). Its scan options let you pick a site type, "Content Site" or "API / Application".

Cloudflare's Radar baseline from the same announcement, for the top 200,000 domains:

| Check | Sites that pass |
| --- | ---: |
| robots.txt present | 78% |
| Content Signals declared | 4% |
| Markdown content negotiation | 3.9% |
| MCP Server Cards and API Catalogs | fewer than 15 sites, together |

## Discoverability: robots.txt, sitemap, Link headers

These are the cheap ones. Serve a `robots.txt` at the root, list your sitemap in it, and serve the sitemap. Any static file handler does this. If your CMS generates a sitemap, the only work is the `Sitemap:` line.

Link headers are newer for most operators. RFC 8288 (October 2017) puts links in the HTTP response, so an agent finds a resource without parsing your HTML. The announcement's one example points at an API catalog:

```http
Link: </.well-known/api-catalog>; rel="api-catalog"
```

`api-catalog` is the only rel value the announcement names; it does not say which others count. Point a Link header only at something you publish. On nginx:

```nginx
add_header Link '</.well-known/api-catalog>; rel="api-catalog"' always;
```

On Apache, with mod_headers:

```apache
Header add Link "</.well-known/api-catalog>; rel=\"api-catalog\""
```

On IIS, the same value goes in a custom header under `<httpProtocol>` in `web.config`. DNS for AI Discovery is a DNS record, not a server setting.

## Content: Markdown on `Accept: text/markdown`

This is the line most sites fail. An agent asks for `text/markdown`; the server answers with a Markdown copy of the same page at the same URL, plus `Vary: Accept` so a shared cache does not hand that copy to a browser. The announcement reports "up to 80% token reduction in some cases."

There are three routes: a static `.md` next to each `.html` plus a few lines of server config, the mod_pagespeed 2.1 optimizer worker's experimental Markdown copy rendered from the full DOM, or Cloudflare's toggle if the site already sits behind it. Tested nginx and Apache configs are in [Accept: text/markdown on nginx and Apache](/blog/accept-text-markdown-nginx-apache/).

Negotiation fixes the format, not a page that is empty without JavaScript. In [our scan of the top 1,000 sites](/blog/can-ai-read-your-website/) (2026-05-28), 403 of the 711 we could read, 57%, showed a non-rendering agent a nearly empty page. Markdown built from that HTML is empty too.

## Bot access control: Content Signals, AI bot rules, Web Bot Auth

Cloudflare's Content Signals Policy (2025-09-24) defines three signals, `search`, `ai-input` and `ai-train`, set in robots.txt. The agent-readiness announcement shows the directive:

```text
User-agent: *
Content-Signal: ai-train=no, search=yes, ai-input=yes
```

Pick your own values. The 2025 policy also proposes comment lines that explain the signals to a human reader; copy them from the policy post.

AI bot rules are ordinary `User-agent` groups in the same file, one per crawler you want to treat differently. Both are a text file edit. Neither enforces anything: a crawler that ignores robots.txt ignores these too.

Web Bot Auth is an IETF draft built on RFC 9421 HTTP Message Signatures: a bot signs its requests with an Ed25519 key and publishes the public key. The scan checks `/.well-known/http-message-signatures-directory`, where a bot operator publishes those keys, and the announcement adds: "If your site just serves content, and doesn't make requests to other sites, you don't need it." For a content site, that line can stay red.

Checking incoming signatures is the other half, and the useful one for a site that receives bot traffic. The mod_pagespeed module and the optimizer worker include an experimental, observe-only verifier, off by default, that checks each signature and labels the request so you can log the verdict ([how it works](/blog/verify-ai-crawlers-web-bot-auth/), 2026-06-22). That tells you which bots are real. It does not turn the scan's line green.

## The lines a content site will not pass

MCP Server Card, Agent Skills, WebMCP, API Catalog, the OAuth lines, Auth.md and the commerce protocols each look for a well-known file or JSON document describing something an agent can call: an API, a tool server, a sign-in flow, a checkout. If you have those, publish the documents; any server can serve a static file. If you do not, a file that advertises a missing API is worse than a fail. Radar found MCP Server Cards and API Catalogs together on fewer than 15 of 200,000 sites.

## Check what an agent reads

Run the free [AI readability check](/ai-readability/) on the page you care about. It fetches the page the way a non-rendering agent does, renders it in a real browser too, compares what each can read, and grades the result. It does not run Cloudflare's checks or test Markdown negotiation. For negotiation, use curl:

```sh
curl -sI -H 'Accept: text/markdown' https://example.com/page
```

`Content-Type: text/markdown` and `Vary: Accept` in the response are what that line asks for.

If you want a second pair of eyes, the [agent-readiness audit](/pricing/#audit) costs €950 per site, excl. VAT: one written report and one review call. It checks technical readiness and verifies the fixes with a rescan; it does not measure how often AI answers mention you.

Start with robots.txt today: add the `Sitemap:` line and a `Content-Signal:` line, then rescan.

---

**Sources** (all read on 2026-10-13)

- "Introducing the Agent Readiness score", Cloudflare blog, 2026-04-17: https://blog.cloudflare.com/agent-readiness/
- "Is Your Site Agent-Ready?", isitagentready.com, as read on 2026-10-13: https://isitagentready.com/
- "Giving users choice with Cloudflare's new Content Signals Policy", Cloudflare blog, 2025-09-24: https://blog.cloudflare.com/content-signals-policy/
- RFC 8288, Web Linking, IETF, October 2017: https://www.rfc-editor.org/rfc/rfc8288
- RFC 9421, HTTP Message Signatures, IETF, February 2024: https://www.rfc-editor.org/rfc/rfc9421

**Related:** [Accept: text/markdown on nginx and Apache](/blog/accept-text-markdown-nginx-apache/)
· [Can AI read your website?](/blog/can-ai-read-your-website/)
· [Web Bot Auth at your origin](/blog/verify-ai-crawlers-web-bot-auth/)
· [the agentic web at the origin](/blog/agentic-web-at-the-origin/)
