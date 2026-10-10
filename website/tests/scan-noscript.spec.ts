// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { test, expect } from '@playwright/test';

// With JavaScript off, a visitor can still send an inquiry from the scan pages
// and the contact page: a native form post to the contact endpoint, answered
// with a redirect to the thanks page. The endpoint is mocked; it is never called.

for (const [route, topic] of [
  ['/analyze/', 'consulting'],
  ['/ai-readability/', 'agents'],
  ['/contact/', 'other'],
] as const) {
  test(`${route} posts an inquiry without JavaScript and lands on the thanks page`, async ({
    browser,
  }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    const posts: Array<{ body: string; type: string }> = [];
    await page.route('**/ai-readability/api/contact', (r) => {
      const req = r.request();
      posts.push({ body: req.postData() ?? '', type: req.headers()['content-type'] ?? '' });
      return r.fulfill({ status: 303, headers: { location: '/contact/thanks/' } });
    });
    await page.goto(route);
    const form =
      route === '/contact/'
        ? page.locator('#contact-form')
        : page.locator('form[data-ui="noscript-lead"]');
    await expect(form).toBeVisible();
    await form.getByLabel('Email').fill('visitor@example.com');
    await form.getByLabel('Name').fill('A visitor');
    await form.getByLabel('Message').fill('Hello from a browser without scripts.');
    if (route === '/contact/') {
      await form.locator('select[name="topic"]').selectOption(topic);
    } else {
      await form.getByLabel('Site address').fill('https://example.com/');
    }
    // Submit with Enter in a field: with scripts disabled Playwright's click
    // never sees the button settle, but Enter is the same native form submit.
    await expect(form.getByRole('button', { name: 'Send message' })).toBeVisible();
    await form.getByLabel('Email').press('Enter');
    await expect(page).toHaveURL(/\/contact\/thanks\/$/);
    expect(posts).toHaveLength(1);
    expect(posts[0].type).toContain('application/x-www-form-urlencoded');
    const body = new URLSearchParams(posts[0].body);
    expect(body.get('email')).toBe('visitor@example.com');
    expect(body.get('name')).toBe('A visitor');
    expect(body.get('message')).toBe('Hello from a browser without scripts.');
    expect(body.get('topic')).toBe(topic);
    expect(body.get('redirect')).toBe('/contact/thanks/');
    expect(body.get('company')).toBe('');
    if (route !== '/contact/') expect(body.get('url')).toBe('https://example.com/');
    await context.close();
  });
}

test('with JavaScript on, the noscript form is not rendered', async ({ page }) => {
  await page.goto('/analyze/');
  await expect(page.locator('form[data-ui="noscript-lead"]')).toHaveCount(0);
});
