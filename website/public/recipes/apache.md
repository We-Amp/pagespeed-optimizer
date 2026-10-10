# Apache: install and verify mod_pagespeed 2.1 (native module)

Source: https://modpagespeed.com/docs/installation-module/#native-apache-module
and https://modpagespeed.com/docs/getting-started/. Header: `X-Mod-Pagespeed`.
Scope: the `mod-pagespeed` package, which also installs the `pagespeed-optimizer`
worker and the configuration that points the module at it. cPanel / WHM hosts
use the EasyApache 4 RPM instead: https://modpagespeed.com/docs/cpanel/

## 1. Prerequisites

Debian 12 or 13, Ubuntu 22.04 or 24.04, or AlmaLinux, RHEL or Rocky 9 or 10,
with Apache 2.4 or newer.

```bash
. /etc/os-release && echo "$ID $VERSION_ID"
apache2ctl -v 2>/dev/null || httpd -v
test -d /usr/local/cpanel && echo 'cPanel host: use https://modpagespeed.com/docs/cpanel/ instead'
```

## 2. Install

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt-get install -y mod-pagespeed && sudo systemctl restart apache2   # Debian / Ubuntu
sudo dnf install -y mod-pagespeed && sudo systemctl restart httpd         # AlmaLinux / RHEL / Rocky
```

## 3. Minimal configuration

None. The package enables the module and installs a default `pagespeed.conf`
(`/etc/apache2/mods-available/pagespeed.conf` on Debian and Ubuntu,
`/etc/httpd/conf.d/pagespeed.conf` on the Red Hat family) with `ModPagespeed on`
and a writable cache path. Confirm the module is loaded:

```bash
(apache2ctl -M 2>/dev/null || httpd -M) | grep pagespeed_module
```

Filters are turned on in that file: https://modpagespeed.com/docs/configuration/

## 4. Verify

```bash
curl -s -o /dev/null -D - '<site-url>/?mps-verify=agent' | grep -i '^x-mod-pagespeed:'
```

Pass: one `X-Mod-Pagespeed: <version>` line; the header is on every HTML
response the module handles. No header:
https://modpagespeed.com/docs/troubleshooting/#no-x-mod-pagespeed-or-x-page-speed-header

## 5. Rollback

```bash
# Off without removing: set `ModPagespeed off` in pagespeed.conf, then
sudo systemctl restart apache2   # httpd on Enterprise Linux

# Remove the module and the worker
sudo apt-get remove -y mod-pagespeed pagespeed-optimizer   # Debian / Ubuntu
sudo dnf remove -y mod-pagespeed pagespeed-optimizer       # AlmaLinux / RHEL / Rocky
sudo systemctl restart apache2   # httpd on Enterprise Linux
```

What stays behind and rolling back a release: https://modpagespeed.com/docs/uninstall/
