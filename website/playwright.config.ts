// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { defineConfig, devices } from '@playwright/test';
import { createHash } from 'node:crypto';

// The suite must test THIS checkout's code. A fixed port plus
// reuseExistingServer silently pointed every local run at whichever dev
// server already held :4321 -- usually another checkout or branch -- so
// results depended on what else was running (failures that vanished in
// isolation, a different test count between runs). Each checkout therefore
// gets its own port, derived from its path (override with PORT), and an
// already-running server is only reused on request (PW_REUSE_SERVER=1). If
// something foreign holds the port, Playwright now fails loudly instead of
// testing the wrong server. CI keeps 4321 (one checkout per runner).
// --ignore-lock keeps the server in the foreground: when astro detects an AI
// agent it otherwise daemonizes, Playwright cannot stop it, and the leaked
// server then collides with (or is silently reused by) the next run.
const PORT =
  process.env.PORT ??
  (process.env.CI
    ? '4321'
    : String(4500 + (createHash('sha1').update(process.cwd()).digest().readUInt16BE(0) % 500)));
const BASE_URL = `http://localhost:${PORT}`;

export default defineConfig({
  testDir: './tests',
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  workers: process.env.CI ? 1 : undefined,
  reporter: 'html',
  use: {
    baseURL: BASE_URL,
    screenshot: 'only-on-failure',
  },
  // The fixtures test is a vitest file that lives next to the data it checks;
  // the visual suite is opt-in (VISUAL=1) and has its own project.
  testIgnore: ['**/fixtures/**'],
  projects: [
    {
      name: 'desktop',
      use: { ...devices['Desktop Chrome'] },
      testIgnore: ['**/fixtures/**', '**/visual/**'],
    },
    {
      name: 'mobile',
      use: { ...devices['Pixel 5'] },
      testMatch: /responsive\.spec\.ts/,
    },
    ...(process.env.VISUAL === '1'
      ? [
          {
            name: 'visual',
            testMatch: /visual\/.*\.visual\.spec\.ts/,
            fullyParallel: false,
            use: {
              ...devices['Desktop Chrome'],
              viewport: { width: 1280, height: 800 },
              colorScheme: 'dark' as const,
              contextOptions: { reducedMotion: 'reduce' as const },
              screenshot: 'off' as const,
            },
          },
        ]
      : []),
  ],
  webServer: {
    command: `npm run dev -- --port ${PORT} --ignore-lock`,
    url: BASE_URL,
    reuseExistingServer: !!process.env.PW_REUSE_SERVER,
    timeout: 30000,
  },
});
