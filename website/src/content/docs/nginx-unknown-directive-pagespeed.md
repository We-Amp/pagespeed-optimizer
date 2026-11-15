---
title: 'nginx: unknown directive pagespeed after Plesk 18.0.79'
seoTitle: 'nginx: unknown directive "pagespeed" after a Plesk update'
description: 'Plesk Obsidian 18.0.79 removed ngx_pagespeed from its nginx. How to get nginx starting again, and the options for PageSpeed afterwards.'
order: 13.5
group: 'Upgrade and migrate'
datePublished: 2026-11-15
lastUpdated: 2026-11-15
faq:
  - q: 'Why does nginx report unknown directive "pagespeed" after updating Plesk?'
    a: 'Plesk Obsidian 18.0.79 (23 June 2026) removed the ngx_pagespeed module from its nginx. nginx stops with an unknown directive error when a configuration still contains pagespeed lines and the module that defines them is gone. Remove or comment out those lines.'
  - q: 'Does mod_pagespeed support the nginx that Plesk ships?'
    a: 'No. We have not documented or tested our module for Plesk''s own nginx package, and we make no support claim for it.'
---

If nginx stopped starting or reloading after a Plesk update, with a message like
`unknown directive "pagespeed"`, this page explains what changed and how to get
nginx running again.

## What changed

Plesk's release notes record the change in two steps:

- Plesk Obsidian 18.0.77 (31 March 2026) announced that support for the
  `ngx_pagespeed.so` module would be deprecated and removed from the `sw-nginx`
  package.
- Plesk Obsidian 18.0.79 (23 June 2026) removed the module from nginx. Plesk's
  announcement gives the reason as a security vulnerability in the PSOL library
  (libwebp).

Sources: the [Plesk Obsidian change log](https://docs.plesk.com/release-notes/obsidian/change-log/)
and the [18.0.79 announcement](https://www.plesk.com/blog/plesk-news-announcements/plesk-obsidian-18-0-79/).

The update removes the module but not your configuration. Any `pagespeed`
directive left in an nginx configuration now refers to a module that is no
longer loaded, and nginx refuses a configuration that contains a directive it
does not know. That is why the whole server fails to start or reload, not only
the sites that used PageSpeed.

## Get nginx running again

1. Find every file that still has a `pagespeed` directive:

   ```bash
   sudo grep -rn "pagespeed" /etc/nginx /var/www/vhosts/system 2>/dev/null
   ```

   Directory layout differs between installations; widen the search if your
   configuration lives elsewhere.

2. Remove or comment out those lines. Typical places are:
   - the per-domain **additional nginx directives** field for each domain in
     the control panel, and
   - any included file, such as a snippet or custom configuration you added by
     hand.

   Every `pagespeed ...;` line has to go, not only `pagespeed on;`. Directives
   such as `pagespeed FileCachePath` fail the same way.

3. Test the configuration, then reload nginx:

   ```bash
   sudo nginx -t
   sudo systemctl reload nginx
   ```

   Run `nginx -t` through the same nginx binary the panel uses if your host has
   more than one.

Your sites serve normally again, without PageSpeed optimization. Nothing else
in the configuration depends on those lines.

## Options going forward

These are independent choices. None of them is required to get nginx running.

**Do without it.** Leave the lines out. Pages are served unoptimized.

**Use mod_pagespeed with Apache.** The maintained
[mod_pagespeed](/docs/installation-module/) has an Apache module. Our
documentation covers stock Apache on Debian, Ubuntu and the RHEL family, and
Apache under cPanel with EasyApache 4. It does not cover Apache as installed
and managed by Plesk, and we have not tested it there, so we cannot say whether
or how well it works in that setup. If you try it, treat it as unverified and
test on a staging domain first. We would like to hear how it goes through the
[contact page](/contact/).

**Run nginx with our module outside Plesk's nginx.** The
[nginx module](/docs/installation-module/#native-nginx-module) loads into the
nginx that your distribution ships, or into the nginx.org stable package, and
each build is pinned to that exact nginx version. It does not load into a
different nginx build, and we have not documented or tested it for the `sw-nginx`
package. We make no claim that it works there. A separate nginx, for example on
another host or in front of the panel's web server, is a workable layout; you
own that setup and its upgrades.

If you are replacing another ngx_pagespeed build, see
[Migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/): your
`pagespeed` directives carry over to the maintained module.

Plesk is a trademark of its owner. This page is not affiliated with or endorsed
by Plesk.
