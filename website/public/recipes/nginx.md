# nginx: install and verify mod_pagespeed 2.1 (native module)

Source: https://modpagespeed.com/docs/installation-module/ and
https://modpagespeed.com/docs/getting-started/. Header: `X-Page-Speed`.
Scope: the `nginx-module-pagespeed` package on the distribution's stock nginx.
For nginx as a reverse proxy in front of another origin use
https://modpagespeed.com/recipes/docker.md.

## 1. Prerequisites

Debian 12 or 13, Ubuntu 22.04 or 24.04, or AlmaLinux, RHEL or Rocky 9 or 10,
running that distribution's nginx package. Each module build is pinned to the
stock nginx version and nginx refuses a module built for another version.

```bash
. /etc/os-release && echo "$ID $VERSION_ID"
nginx -v
dpkg -S "$(command -v nginx)" 2>/dev/null || rpm -qf "$(command -v nginx)"   # must name the distribution's nginx package
```

Stop if nginx came from nginx.org or was built from source; see the
compatibility table in the source document.

## 2. Install

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install -y nginx-module-pagespeed   # Debian / Ubuntu
sudo dnf install -y nginx-module-pagespeed       # AlmaLinux / RHEL / Rocky
```

The package drops a `load_module` snippet into the modules directory nginx
includes. Confirm it, and add the line at the top of `/etc/nginx/nginx.conf`
only if it is missing:

```bash
sudo nginx -T 2>/dev/null | grep -q 'ngx_pagespeed_module.so' && echo 'module loaded' || echo 'ADD to nginx.conf: load_module modules/ngx_pagespeed_module.so;'
```

## 3. Minimal configuration

In each `server` block to optimize:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;

# Let the module serve its own resources.
location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
    add_header "" "";
}
location ~ "^/pagespeed_static/" { }
location ~ "^/ngx_pagespeed_beacon$" { }
```

```bash
sudo nginx -t && sudo systemctl restart nginx
```

To hand in-place optimization to the optimizer worker (`pagespeed-optimizer`,
same release) follow
https://modpagespeed.com/docs/installation-module/#using-the-optimizer-daemon-with-nginx.

## 4. Verify

```bash
curl -s -o /dev/null -D - '<site-url>/?mps-verify=agent' | grep -i '^x-page-speed:'
```

Pass: one `X-Page-Speed: <version>` line; the header is on every HTML response
the module handles. No header:
https://modpagespeed.com/docs/troubleshooting/#no-x-mod-pagespeed-or-x-page-speed-header

## 5. Rollback

```bash
# Off without removing: replace `pagespeed on;` with `pagespeed off;`, then
sudo nginx -t && sudo systemctl restart nginx

# Remove: delete the pagespeed directives, the three location blocks and any
# load_module line added by hand (nginx does not start with directives of a
# missing module), then
sudo apt-get remove -y nginx-module-pagespeed   # Debian / Ubuntu; add pagespeed-optimizer if installed
sudo dnf remove -y nginx-module-pagespeed       # AlmaLinux / RHEL / Rocky
sudo systemctl restart nginx
```

What stays behind and rolling back a release: https://modpagespeed.com/docs/uninstall/
