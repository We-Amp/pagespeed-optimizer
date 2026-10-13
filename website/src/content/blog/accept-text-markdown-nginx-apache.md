---
title: 'Accept: text/markdown on nginx and Apache'
description: 'Serve Markdown to AI agents that send Accept: text/markdown, with or without a CDN: tested nginx and Apache configs, the optimizer worker, and Cloudflare.'
date: 2026-10-21
author: 'Otto van der Schaaf'
tags: ['ai-readability', 'ai-agents', 'nginx', 'apache', 'markdown']
draft: true
---

Some AI agents send `Accept: text/markdown` when they fetch a page: as of February 2026, three of seven agents Cloudflare tested did. Answering with Markdown saves them the tokens they would spend on markup: up to 80% in some cases, Cloudflare measured (2026-04-17). Its Radar scan of the 200,000 most visited domains, after filtering out categories such as redirects and ad servers, found that Markdown negotiation passes on 3.9% of them.

There are three routes: static Markdown files, the mod_pagespeed 2.1 optimizer worker, or Cloudflare's toggle if you already sit behind it. Each costs a different kind of effort; all must get the same two headers right.

## What a correct answer looks like

RFC 9110 (June 2022) defines the `Accept` request header (section 12.5.1) and the `Vary` response header (section 12.5.5). A request that lists `text/markdown` gets a body with `Content-Type: text/markdown`. Every other request gets the HTML, at the same URL.

Both responses carry `Vary: Accept`. That header stops a shared cache, a CDN or a corporate proxy, from serving the stored Markdown copy to the next browser. Send it on the HTML response too, or the first cached copy wins for everyone.

## Route 1: static Markdown files, no CDN

Keep a `.md` file next to each `.html` file: `guide/index.html` and `guide/index.md`. A static site generator can write both in one build; many CMSs can export Markdown.

On nginx, in the `http` context (the stock `nginx.conf` already includes `mime.types`, and a `types` block inside a `server` would replace that list):

```nginx
types { text/markdown md; }

map $http_accept $page_ext {
    default            html;
    "~*text/markdown"  md;
}

server {
    listen 80;
    root /srv/www;
    charset utf-8;
    charset_types text/markdown;

    location / {
        try_files $uri/index.$page_ext $uri/index.html $uri =404;
        add_header Vary Accept always;
    }
}
```

A page without a `.md` file falls through to `index.html`, so a partial export is safe. An `add_header` inside a `location` drops the ones inherited from `server`, so repeat any security headers there.

On Apache, with mod_rewrite and mod_headers loaded:

```apache
AddType text/markdown .md
AddCharset utf-8 .md

<Directory "/srv/www">
    DirectoryIndex index.html

    RewriteEngine On
    RewriteCond %{HTTP:Accept} text/markdown [NC]
    RewriteCond %{REQUEST_FILENAME}index.md -f
    RewriteRule ^(.*/)?$ $1index.md [L]

    Header always merge Vary Accept
</Directory>
```

The rule fires on directory-style URLs ending in a slash, and the second `RewriteCond` checks that the Markdown file exists first. `Header always merge` adds `Accept` to `Vary` once, on error responses too.

We ran both configs in throwaway containers (`nginx:alpine`, nginx 1.31.4; `httpd:2.4-alpine`, Apache 2.4.69) on 2026-10-13. With the header, both answered `Content-Type: text/markdown; charset=utf-8` and `Vary: Accept`. Without it, both served the HTML with `Vary: Accept`. A directory with no `.md` served HTML either way, and Apache's 404 carried `Vary: Accept`.

The match is a substring test that ignores quality values; tested on nginx, a request with `Accept: text/markdown;q=0, text/html` got `Content-Type: text/markdown`.

Two costs. The Markdown is a second copy of every page, and copies drift unless the build writes both from one source. The server never runs JavaScript, so a page whose content appears only in the browser has nothing to export unless the build renders it. In [our scan of the top 1,000 sites](/blog/can-ai-read-your-website/) (2026-05-28), 403 of the 711 we could read, 57%, showed a non-rendering agent a nearly empty page.

## Route 2: the optimizer worker, no CDN

The mod_pagespeed 2.1 optimizer worker produces the copy itself. Start it with `--agent-optimize`, which requires `--enable-browser-analysis`. On a request with `Accept: text/markdown`, the worker serves Markdown produced from the fully rendered DOM, JavaScript executed, at the same URL. The response carries `Vary: Accept`, `X-Robots-Tag: noindex` and `Cache-Control: private`. Browsers and search engines get the normal HTML. Rendering, caching and serving happen on your own server; nothing leaves it.

That solves both costs of route 1: there is no second copy to maintain, and a page that only exists after JavaScript runs gets a Markdown copy made from the rendered page. The cost is a headless browser next to your web server.

The feature is experimental and off by default, and its flags may change; evaluate it on staging first. A separate flag, `--agent-optimize-llms-txt`, synthesizes an `/llms.txt` from your sitemap, also off by default. The mod_pagespeed module alone, on Apache or nginx, adds `Vary: Accept` and serves the normal HTML; it does not render. Details are in the [agent optimization docs](/docs/agent-optimize/) (updated 2026-09-06) and [the design write-up](/blog/serve-markdown-to-ai-agents/) (2026-06-22).

## Route 3: behind Cloudflare

Cloudflare announced Markdown for Agents on 2026-02-12 as a beta at no cost on Pro, Business and Enterprise plans, and for SSL for SaaS customers. Its docs (last updated 2026-10-09) describe enabling it per zone in the dashboard, under AI Crawl Control, and name a Pro or Business plan. Converted responses carry `Content-Type: text/markdown; charset=utf-8` and a `Vary` that includes `Accept`, and keep your origin's other headers; `ETag` and `Last-Modified` are dropped. The announcement adds an `x-markdown-tokens` header with an estimated token count, and gives its own example: 16,180 tokens as HTML, 3,150 as Markdown.

It converts whatever the origin HTML contains, so a page whose content appears only after JavaScript runs has little to convert. And when your origin sets no `content-signal` header, the docs say it adds `Content-Signal: ai-train=yes, search=yes, ai-input=yes`; set your own if that is not your policy. A site that is not behind Cloudflare on those plans has no equivalent toggle.

## Check your server

Ask for Markdown, then ask without the header:

```sh
curl -sI -H 'Accept: text/markdown' https://example.com/page
curl -sI https://example.com/page
```

The first should show `Content-Type: text/markdown` and `Vary: Accept`. The second should show `text/html` and `Vary: Accept`. If the second shows Markdown, purge the cache and check `Vary`.

## About llms.txt

Many guides start with `/llms.txt`. Cloudflare's default agent-readiness score leaves it out and checks Markdown negotiation instead (2026-04-17). Serve one if you like; negotiation comes first.

## Next step

Run the free [AI readability check](/ai-readability/) on a page. It fetches the page the way a non-rendering agent does, renders it in a real browser too, and grades what each can read. It does not test Markdown negotiation; the curl pair above does. A large gap means route 1 has little to export for that page.

For a written review, our [agent-readiness audit](/pricing/#audit) costs €950 per site, excl. VAT, and comes with one report and one review call. The audit checks technical readiness and verifies the fixes with a rescan; it does not measure how often AI answers mention you.

Pick one page that has a `.md` export, add the config for your server, and run the curl pair against it.

---

> **Status:** routes 1 and 3 use stock nginx, Apache and Cloudflare features as documented on the dates above. Route 2, the optimizer worker's Markdown copy and synthesized `/llms.txt`, is experimental and off by default; its flags may change.

**Sources** (all read on 2026-10-13)

- RFC 9110, HTTP Semantics, IETF, June 2022, sections 12.5.1 (Accept) and 12.5.5 (Vary): https://www.rfc-editor.org/rfc/rfc9110
- "Introducing Markdown for Agents", Cloudflare blog, 2026-02-12: https://blog.cloudflare.com/markdown-for-agents/
- "Markdown for Agents", Cloudflare docs, last updated 2026-10-09: https://developers.cloudflare.com/fundamentals/reference/markdown-for-agents/
- "Introducing the Agent Readiness score. Check to see if your site is agent-ready", Cloudflare blog, 2026-04-17: https://blog.cloudflare.com/agent-readiness/

**Related:** [Pass Cloudflare's agent-readiness checks on your own server](/blog/pass-cloudflare-agent-readiness-checks-self-hosted/)
· [Agent optimization (docs)](/docs/agent-optimize/)
· [Serve Markdown to AI agents](/blog/serve-markdown-to-ai-agents/)
· [Can AI read your website?](/blog/can-ai-read-your-website/)
