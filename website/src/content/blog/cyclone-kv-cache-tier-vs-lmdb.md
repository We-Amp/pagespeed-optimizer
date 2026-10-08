---
title: 'Cyclone as an LLM KV-cache tier: eight rounds to match LMDB'
description: 'How Cyclone became an LLM KV-cache offload tier: eight rounds against LMDB and file-per-block, from 0.06 GB/s cold reads to ahead of LMDB.'
date: 2026-10-08
author: 'Otto van der Schaaf'
tags: ['performance', 'benchmarks', 'cache', 'ai']
product: '2.0'
faq:
  - q: 'Is Cyclone faster than LMDB for KV-cache offload?'
    a: 'On cold reads, yes: at every measured size from 64 KiB to 32 MiB, Cyclone reads 1.05-1.4x a same-day LMDB with checksums on. Under churn with the KV preset it meets the pre-registered bar on both access patterns: hit p99 0.19x the LMDB figure while serving 1.55-1.66x as much. Warm reads are the same class, and the Cyclone hit ratio under churn stays 3.4-4.1 points below an LRU.'
  - q: 'What does CacheConfig::for_kv_tier() change?'
    a: 'Exactly two fields: fill_large_document_tail and write_behind become true. Everything else keeps the library default. The measured costs are a 6-22% slower median hit and 18-31% lower bulk-load throughput; write_behind is Linux-only and can block, so the preset is for writers on worker threads, not event loops.'
  - q: 'Does this make Cyclone a replacement for LMDB?'
    a: 'No. The README positions it as a node-local tier behind a KV connector for large blocks: it is not a distributed store, has no GPU-direct or RDMA path, and ships no Python bindings. LMDB keeps an edge in warm copy reads (9.45 against 9.18 GB/s in round 7) and in hit ratio under churn, and file-per-block writes faster at 8 and 32 MiB.'
  - q: 'What hardware did the benchmark run on?'
    a: 'Two laptops: an Apple M5 with 16 GiB on macOS, where only warm page-cache behavior can be measured, and an i7-8750H with 23 GiB and a Samsung 970 PRO NVMe on Linux, where the page cache is dropped before every cold phase. Consumer hardware, one SSD per platform; treat differences under about 20% as noise.'
---

Cyclone is We-Amp's open-source cache library (Apache-2.0), the memory-mapped storage engine both parts of mod_pagespeed share. This post is about a different job we pointed it at: the node-local storage tier that an LLM inference server offloads prompt-prefix KV tensors to. When GPU and CPU memory fill up, engines such as vLLM, SGLang and TensorRT-LLM spill those tensors through a KV connector to storage on the node. The tier's job is narrow: put a multi-megabyte blob under a hash, get it back fast, survive restarts, and let several worker processes share it.

The headline, after eight rounds of measurement against LMDB, RocksDB and one file per block: on Linux/NVMe, Cyclone now reads cold blocks faster than a same-day LMDB at every measured size from 64 KiB to 32 MiB, with checksums verified on every read. Under bounded-capacity churn, its KV preset meets the benchmark's pre-registered bar against LMDB on both access patterns: a hit-latency tail at 0.19x LMDB's while serving 1.55-1.66x as much. Getting there took real work. The first cold measurement was 0.06 GB/s against LMDB's 2.15, and the first churn verdict was an explicit "not significantly better". This is the story of what was changed, round by round, and what each change bought.

Everything below is measured on two laptops: an Apple M5 (macOS, warm page cache only) and an i7-8750H with a Samsung 970 PRO NVMe (Linux, ext4, page cache dropped before cold phases). Block sizes are 512 KiB to 32 MiB, because Llama-3-8B fp16 KV works out to about 131 KB per token, so a 16-token block is about 2 MiB and a 256-token chunk about 32 MiB. Full method, raw data and per-run caveats are in [the benchmark document](https://github.com/We-Amp/cyclone-cache/blob/main/doc/kv-cache-benchmark.md) in the [cyclone-cache repository](https://github.com/We-Amp/cyclone-cache).

## Rounds 1 and 2: the baseline and the cold-read gap

Round 1 ran the stock tree on macOS with a warm page cache. Warm reads immediately tied LMDB (70.9 against 64.9 GB/s copying 2 MiB blocks): both stores serve a hit as a span into a mapping that already exists, with no syscall per get, and that design similarity is why LMDB is the peer that matters. Everything else was behind: writes at 0.40-0.48 GB/s against 1.1-1.5 for file-per-block, and a restart that re-read the dataset at a flat 0.56 GB/s, 25-60x behind, because the first read of each document verifies a byte-wise CRC32 running at about 0.55 GB/s.

Round 2 repeated the sweep on Linux with the page cache dropped, and that exposed the real problem. A cold 2 MiB read ran at 0.06 GB/s against LMDB's 2.15. The cause was the volume-wide `MADV_RANDOM` advice, right for 4 KB HTTP objects and wrong here: each 4 KiB page of a block became its own fault and its own NVMe round trip, about 512 serial faults per block. Turning verification off only recovered to 0.18 GB/s, so the checksum was a third of the gap and the faulting most of the rest.

## Round 3: readahead and a fast checksum

Two fixes followed from that diagnosis. The read path now issues a readahead hint over exactly the document's byte range for documents of at least 256 KiB, before the checksum pass first touches the content. The call is platform-specific because `MADV_WILLNEED` turned out to be wrong on Darwin: it populates the range under a lock, costing up to 86% of the multi-process read phase there, so macOS uses `fcntl(F_RDADVISE)` and Windows `PrefetchVirtualMemory`. The byte-wise CRC32 was replaced with slice-by-16 tables plus a hardware ARMv8 path.

Together these took the Linux cold 2 MiB first-touch read from 0.06 to 1.62 GB/s (27x), and the macOS restart from 0.56 to 8.45 GB/s. Multi-process readers, which had scaled worse than threads because every forked child re-verified and re-faulted from scratch, reached 621k gets/s on Linux, about 2.9x stock. Round 3b then switched the checksum to CRC-32C at on-disk format v8, which has a hardware path on x86-64 too: three interleaved SSE4.2 CRC registers run at 26.5 GB/s on the i7 against 2.8 for slice-by-16. The verified cold read reached 2.17 GB/s, level with LMDB at 2 MiB and ahead at 8 and 32 MiB, with the checksum verified in every case.

## Round 4: a bounded tier under churn, and a partial verdict

Rounds 1-3 measured stores that never fill. A real KV tier is bounded, larger than RAM, and full. Round 4 fixed a decision bar in a spec before any run: 16 GiB of payload, a key universe three times that, get-or-insert traffic in a 4 GiB cgroup, 120 seconds measured, against LMDB with `MDB_NOSYNC` and the in-memory LRU a user would have to write, and file-per-block with the same LRU.

The verdict was "partial", and the diagnosis was worth more than the pass. Cyclone's hit ratio sat 8-9 points below the LRU stores. A policy replay with no I/O traced the loss: about 3-4 points are plain FIFO against LRU, and the rest came from the stripe wrap flushing the directory phase, which made every entry of a stripe's previous lap stop resolving at once, although most of those blocks were still intact on disk. Each stripe restarted empty on every wrap and held roughly half its capacity on average. The tail was the other side: at four threads Cyclone's hit p99 was 3.5-4x lower than LMDB's, partly because LMDB serialises all writes in one transaction.

## Rounds 5 and 6: wrap retention, and the write path rebuilt

Round 5 implemented the design that diagnosis pointed at: wrap retention, keeping the previous lap resolvable until the write cursor actually needs its bytes. The hit ratio rose from 0.758 to 0.815 on plain Zipf and from 0.64 to 0.686 with scans, each within 0.002 of the replay prediction, and the churn verdict became a win through the latency clause. Retention is now the default.

Round 6 re-ran the whole matrix at main and profiled the two remaining losses. The write path had been copying each value three times, and the profiler found the real cost was not memcpy: glibc hands 2 MiB buffers back to the kernel on free, so every put faulted about 992 fresh pages back in. The fix writes the content straight from the handle's buffer instead of assembling a contiguous document, and adds `WriteHandle::reserve()` so a producer generates the block where the commit will write from. One-thread miss+insert p50 fell from 4.41 to 1.49 ms, and 2 MiB puts reached 1.52 GB/s against 1.46 for file-per-block, same day. A trace also explained the remaining insert tail: ext4 reads a 4 KiB page back before letting a write change part of it, and Cyclone packs documents at 8-byte offsets, so each insert's last page shares a block with the previous lap.

## Round 7: small cold reads, write-behind, and the KV preset

Two more fix studies landed before round 7. Small cold reads extended the readahead work below the 256 KiB threshold, gating every hint on the read that is about to run the checksum pass (a validated warm re-read never reaches it) and adding a per-stripe 1 MiB sequential window. A cold 64 KiB read went from 0.04 to 2.15-2.63 GB/s, and cold reads are now ahead of a same-day LMDB at every size from 64 KiB to 32 MiB. The insert tail got `fill_large_document_tail`, which fills each large document's last page so ext4 has nothing to read, and `write_behind`, which starts each large document's write-back from the writer with `sync_file_range`, outside every lock. The fill alone was a measured loss: the read it removed had been accidental back-pressure on a saturated device, so removing it lengthened the read tail. Write-behind keeps both tails down.

Round 7 repeated the matrix with those two options as configurations, on a shared machine with only clean runs counted. The pair together is the first configuration in any round to meet the throughput clause on both patterns: served 1.55x and 1.66x LMDB at four threads, hit p99 8.6 and 9.6 ms against LMDB's 46.6 and 49.4, and a one-thread insert p99 of 4.6 ms. That is what `CacheConfig::for_kv_tier()` now returns: the library defaults with exactly those two fields on. The library defaults are unchanged, and mod_pagespeed keeps both off, because `write_behind` can block and nginx writes inline on the event loop.

## Where it stands

| 2 MiB unless stated, Linux, same-day peers (round 7)        |                               Cyclone | Peers                                      |
| ----------------------------------------------------------- | ------------------------------------: | ------------------------------------------ |
| Cold first touch, 64 KiB / 512 KiB / 2 MiB / 8 MiB / 32 MiB | 2.48 / 2.81 / 2.76 / 3.25 / 3.41 GB/s | LMDB 1.80 / 2.10 / 2.63 / 2.54 / 2.58      |
| Warm GET copy, 1 thread                                     |                             9.18 GB/s | LMDB 9.45, filedir 5.72, RocksDB 2.41      |
| 4 reader processes, view                                    |                           551k gets/s | LMDB 442k                                  |
| PUT, 2 MiB                                                  |                             1.41 GB/s | filedir 1.49 (one clean run), RocksDB 0.39 |
| Churn, zipf, 4 threads, defaults: served / hit p99          |                   2.40 GB/s / 26.0 ms | LMDB 2.24 / 46.6 ms                        |
| Churn, zipf, 4 threads, KV preset: served / hit p99         |                    3.48 GB/s / 8.6 ms | LMDB 2.24 / 46.6 ms                        |
| Host to GPU, mapping registered once (CUDA)                 |                            12.79 GB/s | 12.82 GB/s pinned ceiling                  |

For a KV connector the device-transfer row matters as much as the disk ones: registering Cyclone's whole volume mapping once and copying straight out of it reaches 99.8% of this machine's pinned-buffer PCIe ceiling, 2.3x staging through a buffer. Pinning each returned span instead is slower than staging, on both CUDA and Metal.

## What we did not measure

One laptop per platform and one consumer SSD each; a server NVMe array would change absolute numbers. The churn bar was evaluated against LMDB on the same day because T=4 throughput moved by up to a third between days. CacheLib is the obvious missing peer. No second device, no XFS, no GPUDirect Storage, no more than one writer process, and no end-to-end engine integration: this benchmarks the storage role, not any LLM product. The peer harness itself is unpublished, but the two workload specs are in the repository, a reimplementation must print the same reference vectors, and every raw JSON line is committed. Where Cyclone trails, the numbers above say so: warm copy reads slightly behind LMDB, a churn hit ratio 3.4-4.1 points below an LRU, and 8 and 32 MiB puts behind file-per-block.

## Trying it

The repository is [github.com/We-Amp/cyclone-cache](https://github.com/We-Amp/cyclone-cache); the KV section of its README and [doc/kv-cache-benchmark.md](https://github.com/We-Amp/cyclone-cache/blob/main/doc/kv-cache-benchmark.md) carry the details, including the exact harness invocations. The recommended starting point is the preset:

```cpp
CacheConfig config = CacheConfig::for_kv_tier();
config.set_ram_cache_size(0);   // the OS page cache is the RAM tier
config.max_object_size = 0;     // or your largest block
auto cache = std::move(*Cache::create(config));
```

A read hands back a `std::span` aliasing the mapped pages while the handle is open, and `volume_files()` with `content_file_offset()` is enough to build the one-time GPU registration. The benchmark itself builds from the repository and runs with `./build/kv_bench --print-vectors` to check the harness against a peer implementation.

Cyclone's first job was the HTTP object cache behind mod_pagespeed, where it replaced a file-per-entry design; that comparison, against the same file-per-block pattern this post keeps meeting in the KV role, is covered in [Cyclone vs the file cache](/blog/cyclone-cache-vs-file-cache-benchmark/).

## Frequently asked questions

**Is Cyclone faster than LMDB for KV-cache offload?** On cold reads, yes: at every measured size from 64 KiB to 32 MiB, Cyclone reads 1.05-1.4x a same-day LMDB with checksums on. Under churn with the KV preset it meets the pre-registered bar on both access patterns: hit p99 0.19x the LMDB figure while serving 1.55-1.66x as much. Warm reads are the same class, and the Cyclone hit ratio under churn stays 3.4-4.1 points below an LRU.

**What does CacheConfig::for_kv_tier() change?** Exactly two fields: fill_large_document_tail and write_behind become true. Everything else keeps the library default. The measured costs are a 6-22% slower median hit and 18-31% lower bulk-load throughput; write_behind is Linux-only and can block, so the preset is for writers on worker threads, not event loops.

**Does this make Cyclone a replacement for LMDB?** No. The README positions it as a node-local tier behind a KV connector for large blocks: it is not a distributed store, has no GPU-direct or RDMA path, and ships no Python bindings. LMDB keeps an edge in warm copy reads (9.45 against 9.18 GB/s in round 7) and in hit ratio under churn, and file-per-block writes faster at 8 and 32 MiB.

**What hardware did the benchmark run on?** Two laptops: an Apple M5 with 16 GiB on macOS, where only warm page-cache behavior can be measured, and an i7-8750H with 23 GiB and a Samsung 970 PRO NVMe on Linux, where the page cache is dropped before every cold phase. Consumer hardware, one SSD per platform; treat differences under about 20% as noise.

---

_mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently._
