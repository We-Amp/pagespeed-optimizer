---
title: 'Downstream caching with nginx proxy_cache and Varnish'
description: 'Cache mod_pagespeed HTML in nginx proxy_cache or Varnish: key it on PS-CapabilityList, let the module purge stale pages, and keep beacons flowing.'
order: 24.5
group: 'Configure'
lastUpdated: 2026-10-30
---

Put a cache such as nginx `proxy_cache` or Varnish in front of the server that runs mod_pagespeed and it can serve the optimized HTML without involving the module. Out of the box that does not work, for two reasons. The module marks rewritten HTML as uncacheable, and the HTML it produces depends on the client: the first request for a page is served before the images are optimized, and the markup differs for browsers that accept WebP or AVIF. This page sets up the cache so that it stores one correct copy per client class, and is told when a copy has been superseded.

It applies to the in-process module on Apache and nginx, whose directives are named below. The CDN-specific advice is on [Integrate with a CDN](/docs/cdn-integration/).

## What the module does when you enable the integration

Setting [`DownstreamCachePurgeLocationPrefix`](/docs/configuration/#downstreamcachepurgelocationprefix) turns the downstream-cache integration on. Its effects:

- **HTML keeps the origin's `Cache-Control`.** Without the integration the module replaces the caching headers of the HTML it serves, so a shared cache stores nothing. With it, the origin's `Cache-Control` passes through, and your cache can honor it. Your cache should then hide that header from browsers (the examples below do), so that visitors always ask the cache.
- **The module purges superseded pages.** When a page was served with less rewriting done than [`DownstreamCacheRewrittenPercentageThreshold`](/docs/configuration/#downstreamcacherewrittenpercentagethreshold) (95 percent by default), the module sends a purge request for that page to the address `DownstreamCachePurgeLocationPrefix` plus the page's path, using the method of [`DownstreamCachePurgeMethod`](/docs/configuration/#downstreamcachepurgemethod) (`PURGE` by default). The request carries the headers of the request that triggered it, so a cache that builds its key from request headers removes the right copy. It also carries an `X-PSA-Purge-Request: 1` header, which keeps a purge from triggering another one. The statistics `downstream_cache_purge_attempts` and `successful_downstream_cache_purges` count them, and the message `Purge url is …` appears in the log at info level.
- **Beacons need a key.** Filters that depend on measurements from visitors' browsers, such as `lazyload_images`, `inline_images` and `prioritize_critical_css`, insert beacon JavaScript into a page now and then. With the integration on, the module only does that for a request that carries a `PS-ShouldBeacon` header whose value equals [`DownstreamCacheRebeaconingKey`](/docs/configuration/#downstreamcacherebeaconingkey), and it logs a warning once per beacon interval while the key is missing. Your cache therefore has to send a small share of requests through to the module with that header, and must never accept the header from clients.

```nginx
# nginx with the module (the server behind the cache)
pagespeed DownstreamCachePurgeLocationPrefix http://127.0.0.1:80;
pagespeed DownstreamCacheRebeaconingKey "replace-with-a-long-random-string";
```

```apache
# Apache with the module
ModPagespeedDownstreamCachePurgeLocationPrefix http://127.0.0.1:80
ModPagespeedDownstreamCacheRebeaconingKey "replace-with-a-long-random-string"
```

Trailing slashes on the prefix are dropped. Point the prefix at the address where the cache listens for purge requests, which is not the module's own address.

## PS-CapabilityList

Some of what the module writes into a page depends on what the visitor's browser can do. A cache that ignores this stores the copy that the first visitor's browser could use and serves it to everyone. The `PS-CapabilityList` request header tells the module which of those optimizations the page may contain. You set it in the cache, per request, and the cache includes it in its cache key, so each value gets its own copy.

The value is a comma-separated list of filter ids, optionally followed by a colon and any text (the text after the colon is ignored, so you can use it to label the entry):

| Id | Filter | The page may contain |
| --- | --- | --- |
| `ii` | `inline_images` | Small images inlined as data URLs |
| `ll` | `lazyload_images` | Lazy-loaded images |
| `dj` | `defer_javascript` | Deferred JavaScript |
| `jw` | `convert_jpeg_to_webp` | WebP in place of JPEG |
| `ws` | `convert_to_webp_lossless` | Lossless WebP |
| `wa` | `convert_to_webp_animated` | Animated WebP |
| `ja` | `convert_jpeg_to_avif` | AVIF in place of JPEG |
| `al` | `convert_to_avif_lossless` | Lossless AVIF |
| `am` | `convert_to_avif_animated` | Animated AVIF |

Three rules decide the outcome, all in the module:

- **No header: no limit from the cache.** The module then decides from the request's own `User-Agent` and `Accept` headers, which is wrong for a shared copy. Always set the header.
- **A header can only narrow.** An optimization is used only if the list names it and the request itself supports it. The header never grants a capability the client lacks.
- **A value that names nothing allows nothing.** `fully general optimizations only` is the conventional value for "no client-dependent optimizations". Do not use an empty value: nginx does not send an empty header, and without the header the module decides from the client's own headers.

Choose a small number of classes. Every class is a separate copy of every page and a separate trip to the origin. The two examples below use three: AVIF, WebP and neither.

## nginx proxy_cache

```nginx
http {
    # One value per client class. This is also what keys the cache.
    map $http_accept $ps_capability_list {
        default         "fully general optimizations only";
        "~*image/avif"  "ja:";
        "~*image/webp"  "jw:";
    }

    # HTML leaves this cache as no-cache; everything else keeps its header.
    map $upstream_http_content_type $ps_cache_control {
        default        $upstream_http_cache_control;
        "~*text/html"  "no-cache, max-age=0";
    }

    # Send about 5 percent of requests through to the module with the key,
    # so that it can add beacons. An empty value sends no header at all.
    split_clients "${request_id}" $ps_should_beacon {
        5%  "replace-with-a-long-random-string";
        *   "";
    }

    proxy_cache_path /var/cache/nginx/pagespeed keys_zone=pagespeed:10m max_size=1g inactive=1h;

    server {
        listen 80;
        server_name www.example.com;

        location / {
            proxy_pass http://127.0.0.1:8080;
            proxy_set_header Host $host;

            proxy_cache pagespeed;
            proxy_cache_key "$scheme$host$request_uri$ps_capability_list";
            proxy_cache_bypass $ps_should_beacon;

            proxy_set_header PS-CapabilityList $ps_capability_list;
            proxy_set_header PS-ShouldBeacon $ps_should_beacon;

            proxy_hide_header Cache-Control;
            add_header Cache-Control $ps_cache_control;
        }
    }
}
```

The key `replace-with-a-long-random-string` must equal `DownstreamCacheRebeaconingKey` on the module. A client cannot inject the header, because `proxy_set_header` replaces whatever the client sent. Open-source nginx cannot purge a cache entry on request: the purge the module sends needs a purge module such as the third-party `ngx_cache_purge` (its `proxy_cache_purge` directive must derive the same cache key; read its documentation for how), or nginx Plus. Without one, an entry lives until the origin's `Cache-Control` expires it, and the module's purge requests fail: `downstream_cache_purge_attempts` rises and `successful_downstream_cache_purges` does not.

## Varnish

This is VCL 4.0 syntax; it is based on the sample that ships with the module in `install/debug_conf.v4.vcl`. Adapt it to your Varnish version and test it with `varnishd -C -f` before loading it.

```vcl
vcl 4.0;
import std;

backend default {
  .host = "127.0.0.1";
  .port = "8080";
}

# Where the module's purge requests come from.
acl purge {
  "127.0.0.1";
}

sub vcl_recv {
  # Clients never get to set this.
  unset req.http.PS-ShouldBeacon;

  # One value per client class. vcl_hash below puts it in the key.
  if (req.http.Accept ~ "image/avif") {
    set req.http.PS-CapabilityList = "ja:";
  } elsif (req.http.Accept ~ "image/webp") {
    set req.http.PS-CapabilityList = "jw:";
  } else {
    set req.http.PS-CapabilityList = "fully general optimizations only";
  }

  if (req.method == "PURGE") {
    if (!client.ip ~ purge) {
      return (synth(405, "Not allowed."));
    }
    return (purge);
  }

  # The module has its own cache for .pagespeed. files; keep them out of this one.
  if (req.url ~ "\.pagespeed\.") {
    return (pass);
  }
}

sub vcl_hash {
  hash_data(req.http.PS-CapabilityList);
}

# Send about 5 percent of hits and 25 percent of misses through, with the key.
sub vcl_hit {
  if (std.random(0, 100) < 5) {
    set req.http.PS-ShouldBeacon = "replace-with-a-long-random-string";
    return (pass);
  }
}

sub vcl_miss {
  if (std.random(0, 100) < 25) {
    set req.http.PS-ShouldBeacon = "replace-with-a-long-random-string";
    return (pass);
  }
}

# The ttl was taken from the origin's Cache-Control before this runs;
# browsers get no-cache so that they always ask this cache.
sub vcl_backend_response {
  if (beresp.http.Content-Type ~ "text/html") {
    unset beresp.http.Cache-Control;
    set beresp.http.Cache-Control = "no-cache, max-age=0";
  }
}
```

The `.pagespeed.` rule matters in practice: a cache that stores them duplicates the module's cache and has to be purged when a file changes, which nothing does. If you terminate TLS in front of Varnish, the page the module sees still has to be HTTPS; see [Behind a TLS terminator, load balancer or CDN](/docs/cdn-integration/#behind-a-tls-terminator-load-balancer-or-cdn).

## If the cache never gets a hit

- **Look at what the module sends.** `curl -s -D - -o /dev/null -H 'PS-CapabilityList: jw:' http://127.0.0.1:8080/` against the module itself, not the cache, prints the response headers of the HTML, including its `Cache-Control`. If it says `no-cache` or `max-age=0`, `DownstreamCachePurgeLocationPrefix` is not set where the page is served (it can be set per server or per location) or the origin itself sends that.
- **Check the key.** The cache key must include the capability class. If it varies on something unbounded, such as the whole `User-Agent` or `Accept` string, there is a copy per browser build and almost no hits.
- **Check cookies.** Varnish does not cache a request that carries cookies, and nginx does not cache a response that sets one, unless you tell them to.
- **The first request is served before optimization is done.** That is expected: with a cache, the first visitor gets the unoptimized page, the module sees that it went out with most of its rewriting pending and asks the cache to purge it, and the next request fetches the optimized one.

## See also

- [Cache modes](/docs/cache-modes/): the module's own caches, flushing and purging.
- [Configuration reference](/docs/configuration/): every directive named here.
- [Integrate with a CDN](/docs/cdn-integration/): `Vary`, `Accept` and cache keys at the edge.
