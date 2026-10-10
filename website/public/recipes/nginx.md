# nginx: install and verify mod_pagespeed 2.1 (native module)

Source: https://modpagespeed.com/docs/installation-module/ and
https://modpagespeed.com/docs/getting-started/. Header: `X-Page-Speed`.
Scope: the `nginx-module-pagespeed` package on the distribution's stock nginx.
For nginx as a reverse proxy in front of another origin use
https://modpagespeed.com/recipes/docker.md.

## 1. Prerequisites

Debian 12 or 13 or Ubuntu 22.04 or 24.04 (amd64 or arm64), or AlmaLinux, RHEL
or Rocky 9 (x86_64 or aarch64) or 10 (x86_64 only), running that
distribution's nginx package. Each module build is pinned to the stock nginx
version and nginx refuses a module built for another version.

```bash
. /etc/os-release && echo "$ID $VERSION_ID $(uname -m)"
nginx -v                         # must equal the version the compatibility table lists for this distribution
dpkg -s nginx-module-pagespeed   # Debian / Ubuntu: must report "is not installed"
rpm -q nginx-module-pagespeed    # AlmaLinux / RHEL / Rocky: must report "is not installed"
```

Compatibility table:
https://modpagespeed.com/docs/installation-module/#nginx-compatibility. Stop if
`nginx -v` does not match it (nginx from nginx.org or built from source). Stop
and ask if the module package is already installed.

## 2. Install

The apt and dnf repositories that `install.sh` configures are GPG-signed; apt
and dnf check every package against the repository key.

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install -s nginx-module-pagespeed | grep '^Inst'   # Debian / Ubuntu: what would change
sudo apt-get install -y nginx-module-pagespeed                  # Debian / Ubuntu
sudo dnf install -y nginx-module-pagespeed                      # AlmaLinux / RHEL / Rocky
```

The module requires the exact nginx version it was built for. If the
simulation (or dnf's transaction summary) would upgrade or replace nginx
itself, stop and ask.

The package drops a `load_module` snippet into the modules directory nginx
includes. Confirm it:

```bash
sudo nginx -T 2>/dev/null | grep -q 'ngx_pagespeed_module.so' && echo 'module loaded' || echo 'missing: add the load_module line'
```

Only if it is missing, add the line for the distribution at the top of
`/etc/nginx/nginx.conf`:

```nginx
load_module modules/ngx_pagespeed_module.so;                    # Debian / Ubuntu
load_module /usr/lib64/nginx/modules/ngx_pagespeed_module.so;   # AlmaLinux / RHEL / Rocky
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
sudo apt-get remove -y nginx-module-pagespeed   # Debian / Ubuntu; add pagespeed-optimizer if this run installed it
sudo dnf remove -y nginx-module-pagespeed       # AlmaLinux / RHEL / Rocky
sudo nginx -t && sudo systemctl restart nginx
```

What stays behind and rolling back a release: https://modpagespeed.com/docs/uninstall/
