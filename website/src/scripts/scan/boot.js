// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Pre-paint scan interface chooser. ScanUiBoot.astro inlines this file as a
// classic script ahead of both interface variants; the test in
// test/scan-ui-boot.test.ts runs this same file. Plain ES5: it cannot import.
// The default and the storage key come from data-default and data-key on the
// <script> element itself (read through document.currentScript), so the file
// needs no build-time substitution. The rules mirror resolveScanUi() in
// src/lib/scan/flag.ts.
(function () {
  var cfg = (document.currentScript && document.currentScript.dataset) || {};
  var ui = cfg.default === 'v2' ? 'v2' : 'v1';
  var key = cfg.key || 'scan-ui';
  var store = null;
  try {
    store = window.localStorage;
  } catch (e) {
    store = null;
  }
  var asked = null;
  try {
    asked = new URLSearchParams(location.search).get('ui');
  } catch (e) {
    asked = null;
  }
  if (asked === 'v1' || asked === 'v2') {
    ui = asked;
    try {
      store.setItem(key, asked);
    } catch (e) {}
  } else {
    try {
      var saved = store.getItem(key);
      if (saved === 'v1' || saved === 'v2') ui = saved;
    } catch (e) {}
  }
  document.documentElement.dataset.scanUi = ui;
})();
