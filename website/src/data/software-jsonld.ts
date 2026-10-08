// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Shared SoftwareApplication facts for the mod_pagespeed 2.1 product nodes:
// the platforms it ships for (the Apache and nginx packages on Linux, the IIS
// module and the ASP.NET Core packages on Windows) and the current version,
// read from the 2.1 release manifest so it never needs a hand edit.

import { getRelease } from '../lib/release';

export const PRODUCT_OPERATING_SYSTEMS = ['Linux', 'Windows'];

export async function productSoftwareVersion(): Promise<string> {
  return (await getRelease('2.1')).release.semver;
}
