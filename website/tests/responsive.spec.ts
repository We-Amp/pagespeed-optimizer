// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

test.describe('Responsive layout', () => {
  test('desktop: header nav links visible, hamburger hidden', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.goto('/');
    // Desktop nav should be visible
    const desktopNav = page.locator('header .hidden.md\\:flex');
    await expect(desktopNav).toBeVisible();
    // Hamburger should be hidden
    await expect(page.locator('#mobile-menu-btn')).toBeHidden();
  });

  test('mobile: header nav links hidden, hamburger visible', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    // Desktop nav should be hidden
    const desktopNav = page.locator('header .hidden.md\\:flex');
    await expect(desktopNav).toBeHidden();
    // Hamburger should be visible
    await expect(page.locator('#mobile-menu-btn')).toBeVisible();
  });

  test('mobile: hamburger opens nav menu', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    const menu = page.locator('#mobile-menu');
    await expect(menu).toBeHidden();

    await page.locator('#mobile-menu-btn').click();
    await expect(menu).toBeVisible();

    // Menu should contain nav links
    await expect(menu.locator('a[href="/features/"]')).toBeVisible();
    await expect(menu.locator('a[href="/pricing/"]').first()).toBeVisible();
  });

  test('pricing cards are visible on mobile', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/pricing/');
    // Two-card layout: mod_pagespeed 1.1 (.card) and ModPageSpeed 2.0 (.card-featured).
    await expect(page.locator('.card, .card-featured').first()).toBeVisible();
  });

  test('footer is visible on mobile', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    await expect(page.locator('footer')).toBeVisible();
  });

  test('calculator inputs are visible on mobile', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/calculator/');
    await expect(page.locator('#pageviews')).toBeVisible();
    // per-site licensing renamed the "servers" input to "sites".
    await expect(page.locator('#sites')).toBeVisible();
  });

  test('demo tabs are visible on mobile', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/demo/');
    await expect(page.locator('#tab-ecommerce')).toBeVisible();
  });
});
