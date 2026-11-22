---
title: 'What AI crawlers cost your server, from your access log'
description: 'A free page that reads your nginx, Apache or IIS access log in your browser: requests, bytes and estimated cost per AI crawler, and visits sent back.'
date: 2026-12-01
author: 'Otto van der Schaaf'
tags: ['ai-agents', 'nginx', 'apache', 'iis']
draft: false
tool: crawler-cost
---

Your access log records every request an AI crawler makes to your site. What it does not tell you at a glance is what those requests cost, and whether the crawlers send anyone back.

The [AI crawler cost meter](/ai-crawler-cost/) answers that from the log itself. Drop an access log on the page and it reports, per crawler:

- requests and response bytes, and their share of all traffic;
- an estimated cost at the rate you enter (per GB, and per million requests if you pay for those);
- visits that AI assistants sent back to your pages, and the ratio of crawler page requests to those visits.

It reads nginx and Apache `combined`, Apache `common` and IIS W3C extended logs, plain or gzip. Rotated files can be dropped together.

## The log stays on your machine

The page parses the file in your browser. Nothing from the log is uploaded, and the page stores nothing from it. You can check this yourself: load the page, switch your network off, then drop the file. The analysis still runs.

The site's analytics counts page views, link clicks and one event when an analysis finishes. None of these carries anything from your log.

## What the numbers mean, and what they do not

- **Crawlers are identified by their claimed user agent.** A scraper can claim to be GPTBot, and a crawler can pretend to be a browser. Each per-crawler number is an estimate in both directions.
- **"Visits sent back" are landing requests**, not sessions. The page never reads client addresses, so it cannot group requests into visits.
- **Cost is your rate times your bytes.** A self-hosted server usually pays for traffic, not requests, so the per-request rate starts at zero.

## Two things to do about it

The meter tells you how big the problem is. If it is big enough to act on, there are two recipes. The first needs only stock nginx. The second needs the mod_pagespeed nginx module in front of Anubis.

1. [Rate-limit traffic that claims to be a crawler but is not](/ai-crawler-cost/rate-limit-impostor-crawlers/). nginx can compare the claimed user agent with the address ranges the crawler operators publish, and refuse (429) only the requests above the limit that do not match.
2. [Let signed AI agents past Anubis](/ai-crawler-cost/anubis-signed-agents/). If you run a proof-of-work gate, you can keep it for unknown traffic and still let through agents whose request signatures your nginx has verified.

If you would rather serve crawlers a smaller copy than block them, see [serving Markdown to agents](/blog/serve-markdown-to-ai-agents/). If you want to know which crawler is really knocking, see [verifying AI crawlers with Web Bot Auth](/blog/verify-ai-crawlers-web-bot-auth/).

## Tell me what you find

I want to know whether AI crawlers are a real cost for self-hosted sites, or mostly noise. If the meter shows something worth talking about, the page's "Copy summary" button gives you a plain-text summary without client addresses or paths from your log. Paste it into the [contact form](/contact/?topic=agents&wedge=crawler-cost) with a line about your setup. I read every message.

[Open the AI crawler cost meter](/ai-crawler-cost/)
