// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// capture-telemetry.mjs — capture the site's real response measurements and
// write src/data/telemetry-capture.json, the dated data file behind the
// telemetry strip's server-rendered fallback values.
//
// For each path one GET with `accept-encoding: gzip` is issued over
// node:https; the wire bytes are the raw (compressed) response body, the
// decoded bytes come from zlib.gunzipSync. No TTFB is recorded: one client's
// timing is not a property of the page. The strip upgrades to live values in
// the browser; this file is what no-JS readers and first paint see, always
// labelled with its capture date.
//
// Run before each site deploy and at least monthly:
//   npm run capture:telemetry                      # against production
//   node scripts/capture-telemetry.mjs <origin>    # against another origin

import { get } from 'node:https';
import { gunzipSync } from 'node:zlib';
import { writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ORIGIN = process.argv[2] ?? 'https://modpagespeed.com';
const PATHS = ['/', '/docs/', '/download/', '/pricing/', '/features/', '/demo/'];
const HEADER_KEYS = [
  'x-mod-pagespeed',
  'vary',
  'content-encoding',
  'content-type',
  'cache-control',
  'content-length',
];
const MAX_REDIRECTS = 3;
const TIMEOUT_MS = 15000;

const outFile = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../src/data/telemetry-capture.json',
);

function fetchPath(url, redirectsLeft) {
  return new Promise((resolvePromise, reject) => {
    const req = get(
      url,
      {
        headers: {
          'accept-encoding': 'gzip',
          accept: 'text/html,application/xhtml+xml',
          'user-agent': 'modpagespeed.com telemetry capture (build tooling)',
        },
      },
      (res) => {
        const status = res.statusCode ?? 0;
        if (status >= 300 && status < 400 && res.headers.location) {
          res.resume();
          if (redirectsLeft <= 0) {
            reject(new Error(`too many redirects for ${url}`));
            return;
          }
          resolvePromise(fetchPath(new URL(res.headers.location, url), redirectsLeft - 1));
          return;
        }
        const chunks = [];
        res.on('data', (chunk) => chunks.push(chunk));
        res.on('end', () => {
          const body = Buffer.concat(chunks);
          const headers = Object.fromEntries(
            HEADER_KEYS.map((key) => [key, res.headers[key] ?? null]),
          );
          let decodedBytes = body.length;
          if ((res.headers['content-encoding'] ?? '').includes('gzip')) {
            try {
              decodedBytes = gunzipSync(body).length;
            } catch (err) {
              reject(new Error(`gunzip failed for ${url}: ${err.message}`));
              return;
            }
          }
          resolvePromise({
            status,
            wireBytes: body.length,
            decodedBytes,
            headers,
          });
        });
        res.on('error', reject);
      },
    );
    req.setTimeout(TIMEOUT_MS, () => {
      req.destroy(new Error(`timeout after ${TIMEOUT_MS} ms for ${url}`));
    });
    req.on('error', reject);
  });
}

const origin = new URL(ORIGIN).origin;
const paths = {};
for (const path of PATHS) {
  const result = await fetchPath(new URL(path, origin), MAX_REDIRECTS);
  paths[path] = result;
  console.log(
    `${path}: ${result.status} wire=${result.wireBytes} decoded=${result.decodedBytes} ` +
      `x-mod-pagespeed=${result.headers['x-mod-pagespeed'] ?? 'not present'}`,
  );
}

const capture = {
  origin,
  capturedAt: new Date().toISOString(),
  paths,
};
writeFileSync(outFile, JSON.stringify(capture, null, 2) + '\n');
console.log(`wrote ${outFile}`);
