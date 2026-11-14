# Security policy

## Reporting a vulnerability

If you discover a security vulnerability in mod_pagespeed, please report it
privately in one of these ways:

- **GitHub private vulnerability reporting:** open the
  [Security tab](https://github.com/We-Amp/pagespeed-optimizer/security) of this
  repository and choose **Report a vulnerability**, or go straight to the
  [report form](https://github.com/We-Amp/pagespeed-optimizer/security/advisories/new).
- **Email:** **security@modpagespeed.com**

Please do not open a public issue or pull request for a vulnerability. Both
are visible to everyone before a fix is available.

We will acknowledge your report within 72 hours and work with you to understand
and address the issue.

## Scope

The following components are in scope for security reports:

- pagespeed-optimizer daemon and the nginx serving module (image
  optimization, HTML rewriting, cache subsystem, management HTTP API)
- ASP.NET Core middleware and NuGet packages
- Web console (the `/console/` UI)

Reports for the Apache module (the
[We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed) repository) are
covered by this same policy and address.

Third-party dependencies are out of scope. If you find a vulnerability in a
dependency, please report it to the upstream maintainer and notify us so we can
assess impact.

## Fixes and advisories

Security fixes ship in a release. Each release that fixes a vulnerability comes
with a published
[security advisory](https://github.com/We-Amp/pagespeed-optimizer/security/advisories)
that lists the impact and the affected and fixed versions, and credits the
reporter if they want to be credited.

## Supported versions

Only the latest release of mod_pagespeed receives security
updates. Users on older versions should upgrade to receive fixes.

## Disclosure policy

We follow a 90-day coordinated disclosure timeline. Once a vulnerability is
reported:

1. We confirm receipt within 72 hours.
2. We investigate and develop a fix.
3. We coordinate a release within 90 days of the initial report.
4. We publish a security advisory when the fix is released.
5. After the fix is released, the reporter may publish details.

If you would like to be credited for your report, let us know and we will
include your name (or alias) in the security advisory and the release notes.

## Contact

- **Private vulnerability reporting:**
  https://github.com/We-Amp/pagespeed-optimizer/security/advisories/new
- **Email:** security@modpagespeed.com
- **Company:** We-Amp B.V., Castricum, The Netherlands
