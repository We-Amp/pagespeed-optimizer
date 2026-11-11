// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Types for scripts/visual-diff.mjs, so the vitest suite in
// test/visual-diff.test.ts imports it with `astro check` none the wiser.
// Keep in sync with the .mjs exports.

export type Viewport = { name: string; width: number; height: number };
export type RawImage = { data: Buffer; width: number; height: number };
export type PixelComparison = {
  width: number;
  height: number;
  total: number;
  changed: number;
  maxDelta: number;
  top: number | null;
  bottom: number | null;
  mask: Uint8Array | null;
};
export type DistDiff = {
  empty: boolean;
  html: {
    compared: number;
    onlyBase: string[];
    onlyHead: string[];
    changed: { file: string; offset: number; base: string; head: string }[];
  };
  css: {
    baseRules: number;
    headRules: number;
    onlyBase: string[];
    onlyHead: string[];
    sheetsOnlyBase: string[];
    sheetsOnlyHead: string[];
  };
};

export const VIEWPORTS: Viewport[];
export const DEFAULT_PATHS: string[];
export const DEFAULT_MASKS: string[];
export function normalizeText(text: string): string;
export function splitCssRules(css: string, context?: string): string[];
export function multisetDiff(a: string[], b: string[]): { onlyA: string[]; onlyB: string[] };
export function distDiff(baseInput: string, headInput: string): DistDiff;
export function comparePixels(a: RawImage, b: RawImage, threshold?: number): PixelComparison;
export function formatPercent(changed: number, total: number): string;
export function slugForPath(path: string): string;
export function freezeSvg(svg: string): string;
export function main(argv: string[]): Promise<number>;
