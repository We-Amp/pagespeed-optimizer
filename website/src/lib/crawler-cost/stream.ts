// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Bytes in, lines out, in bounded memory. Gzip is recognised by its magic
// bytes and inflated with fflate's streaming Gunzip, which reads every member
// of a concatenated archive (logrotate output); the browser's
// DecompressionStream stops after the first member and is not used.
import { Gunzip } from 'fflate';

export interface LineChunk {
  text: string;
  cut: boolean;
}
export interface ReadOptions {
  onBytes?: (n: number) => void;
  onGzip?: () => void;
  maxLineBytes?: number;
  maxDecompressedBytes?: number;
}
export class StreamError extends Error {}
const GZIP_DAMAGED =
  'the gzip data ended early or is damaged; the lines before that point are counted';

export function isGzip(bytes: Uint8Array): boolean {
  return bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b;
}

export async function* readLines(
  stream: ReadableStream<Uint8Array>,
  opts: ReadOptions = {},
): AsyncGenerator<LineChunk> {
  const maxLine = opts.maxLineBytes ?? 65_536;
  const maxOut = opts.maxDecompressedBytes ?? 20_000_000_000;
  const decoder = new TextDecoder('utf-8');
  const reader = stream.getReader();
  let pending = '';
  let cutting = false;
  let produced = 0;
  let gunzip: Gunzip | null = null;
  let first = true;
  const inflated: Uint8Array[] = [];

  function* feed(text: string): Generator<LineChunk> {
    let start = 0;
    for (;;) {
      const nl = text.indexOf('\n', start);
      if (nl < 0) break;
      const piece = text.slice(start, nl);
      start = nl + 1;
      if (cutting) {
        cutting = false;
        yield { text: pending.slice(0, 200), cut: true };
        pending = '';
        continue;
      }
      let line = pending + piece;
      pending = '';
      if (line.length > maxLine) {
        yield { text: line.slice(0, 200), cut: true };
        continue;
      }
      if (line.endsWith('\r')) line = line.slice(0, -1);
      yield { text: line, cut: false };
    }
    const rest = text.slice(start);
    if (cutting) return;
    pending += rest;
    if (pending.length > maxLine) cutting = true;
  }

  function* handle(bytes: Uint8Array): Generator<LineChunk> {
    produced += bytes.length;
    if (produced > maxOut)
      throw new StreamError('more than 20 GB after decompression; stopped here');
    yield* feed(decoder.decode(bytes, { stream: true }));
  }

  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      if (!value || value.length === 0) continue;
      opts.onBytes?.(value.length);
      if (first) {
        first = false;
        if (isGzip(value)) {
          opts.onGzip?.();
          gunzip = new Gunzip((chunk) => {
            inflated.push(chunk);
          });
        }
      }
      if (gunzip) {
        try {
          gunzip.push(value);
        } catch {
          throw new StreamError(GZIP_DAMAGED);
        }
        while (inflated.length) yield* handle(inflated.shift() as Uint8Array);
      } else {
        yield* handle(value);
      }
    }
    if (gunzip) {
      try {
        gunzip.push(new Uint8Array(0), true);
      } catch {
        throw new StreamError(GZIP_DAMAGED);
      }
      while (inflated.length) yield* handle(inflated.shift() as Uint8Array);
      // fflate does not throw on a truncated member. Its internal state keeps the
      // final-block flag set when the deflate data stops short, and clears it
      // after a complete member, so a set flag here means the file was cut.
      if ((gunzip as unknown as { s?: { f?: unknown } }).s?.f) throw new StreamError(GZIP_DAMAGED);
    }
    const tail = decoder.decode();
    if (tail) yield* feed(tail);
    if (cutting) yield { text: pending.slice(0, 200), cut: true };
    else if (pending !== '')
      yield { text: pending.endsWith('\r') ? pending.slice(0, -1) : pending, cut: false };
  } finally {
    reader.releaseLock();
  }
}
