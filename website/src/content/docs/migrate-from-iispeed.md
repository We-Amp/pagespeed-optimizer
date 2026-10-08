---
title: 'Migrate from IISpeed'
description: 'Move an IISpeed install to the mod_pagespeed IIS module: save the configuration, uninstall IISpeed, run the MSI, keep the cache, verify, roll back.'
order: 14
group: 'Upgrade and migrate'
lastUpdated: 2026-10-08
datePublished: 2026-10-08
faq:
  - q: 'What happens to my IISpeed subscription?'
    a: 'Nothing needs to carry over: mod_pagespeed 2.1 is free to install and run, in development and in production, so there is no entitlement to move. Questions about an existing IISpeed subscription go to license@we-amp.com.'
  - q: 'Do I have to rewrite my IISpeed configuration?'
    a: 'No. The module reads your `iiswebspeed.config` files where no `pagespeed.config` exists, accepts the `iispeed` directive prefix, and reads the server-level file under `%ProgramData%\We-Amp\IISWebSpeed\`. Rename the files to `pagespeed.config` when convenient; the content stays the same.'
---

IISpeed is deprecated; its successor is the native IIS module that
mod_pagespeed 2.1 ships, from the same code lineage. The move is an uninstall,
an install and a check: your configuration files and your cache directory stay
in place and keep working. The IIS package ships from the 1.15 packaging
channel.

## Before you start

1. **Save your configuration.** Copy the `iiswebspeed.config` files (the
   server-level one under `%ProgramData%\We-Amp\IISWebSpeed\` and any
   per-site file in a site's physical root) somewhere safe. You will not need
   to edit them, but you want the copy if you roll back.
2. **Keep the IISpeed installer** you installed from. It is your rollback.
3. **Leave the cache.** The module reads the cache directory IISpeed used;
   there is nothing to drain.

## Steps

1. **Uninstall IISpeed.** _Apps & features_ (or _Programs and Features_) →
   remove "IISpeed", then run `iisreset` so no worker process still holds the
   old module. The two modules register the same handler in IIS and cannot
   coexist; this step is not optional.
2. **Install the module.** Download the signed MSI from the
   [download page](/download/) and run it, then `iisreset`. The installer
   registers the module as a native HTTP module and creates the default cache
   directory at `%ProgramData%\We-Amp\IISWebSpeed\Cache`.
   [Install on IIS](/docs/install-iis/) has the requirements.
3. **Check that the configuration is read.** The module looks for
   `pagespeed.config` first and `iiswebspeed.config` second, in the site root
   and then at the server level, so your files are picked up as they are. The
   `iispeed` directive prefix is accepted alongside `pagespeed` and
   `ModPagespeed`. A cache path inherited from IISpeed under the `IISWebSpeed`
   tree keeps working, and each site's cache subdirectory is created on first
   request. [IIS configuration](/docs/iis-configuration/) has the lookup
   order.
4. **Verify.** On IIS the module emits `X-Page-Speed`:

   ```powershell
   (Invoke-WebRequest http://localhost/ -Method Head).Headers["X-Page-Speed"]
   ```

   Then open `/pagespeed_global_admin` on the server itself: the Configuration
   page lists the active filters, so you can confirm your directives loaded.
   [Is it working?](/docs/is-it-working/) has the other checks.
5. **Rename when convenient.** `pagespeed.config` takes priority when both
   files exist in a directory; rename `iiswebspeed.config` to
   `pagespeed.config` at your own pace. No content changes are needed.

## Common stumbles

- **"Module DLL could not be registered"** is almost always an IISpeed
  remnant: the old native module is still registered in
  `applicationHost.config` although its files are gone. Remove the
  `IISpeedModule` entry, run `iisreset`, and run the MSI again.
- **Pages load but nothing is optimized.** Check the rewrite level in your
  configuration (`PassThrough` optimizes nothing), a `Disallow` that excludes
  the site root, or an application pool identity that cannot write to the
  cache directory. The admin console's Statistics page shows which: no image
  rewrites means the rewrite never ran; `resource_url_domain_rejections`
  points at a `Disallow` or domain mismatch.
- **Combined resources return 404.11.** `combine_css` and
  `combine_javascript` produce URLs with a `+`; allow double escaping in the
  site's `web.config`. See [IIS tuning](/docs/iis-configuration/#iis-tuning).

## Roll back

Uninstall the module from _Apps & features_, run `iisreset`, reinstall the
IISpeed installer you kept, and restore your saved configuration files if you
renamed them. Nothing on our side needs undoing.
