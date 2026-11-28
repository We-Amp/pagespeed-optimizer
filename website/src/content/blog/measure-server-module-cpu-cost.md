---
title: "How to benchmark a web server module's CPU cost: A/A first"
description: 'Before you judge what an nginx, Apache or IIS module, filter or upgrade costs in CPU, find the smallest change your benchmark can resolve: run A/A first.'
date: 2026-11-28
author: 'Otto van der Schaaf'
tags: ['performance', 'benchmarks', 'nginx']
product: '1.1'
draft: false
---

mod_pagespeed runs inside your web server, as a module in nginx, Apache and IIS. Every request it handles runs code in the server's own processes, so before you switch it on, enable another filter or take an upgrade, you may want to know what that costs in CPU per request.

If the cost is a few percent or less, it sits where benchmarks mislead. We-Amp has published [Proxy benchmarking](https://we-amp.com/writing/proxy-benchmarking/), a series on measuring CPU per request when the noise is bigger than the effect. Its measurements are of the Envoy proxy. None of them prices mod_pagespeed, but the method carries over to any module you want to price.

## How much benchmark noise is there?

On the series' test server, a dedicated machine with nothing else running, one A/B run of an Envoy build against another (the Envoy findings themselves are in [what Envoy itself costs in CPU](/blog/envoy-cpu-series/)) cannot tell a 2% CPU change from no change or from a 4% one. Twelve paired rounds resolve about 2% (Modelled, from the measured spreads). Shared or virtualized hardware is usually noisier. If the setting you are judging costs a percent or two, one before-and-after run cannot tell it from no change.

## Run an A/A test first

The first tool is an A/A run: [the same binary and configuration compared with a copy of themselves](https://we-amp.com/writing/proxy-benchmark-method/#what-does-an-aa-run-tell-you), in the same session, with the same pairing and analysis as the real comparison. Any difference it shows comes from the rig. On the series' server that floor was about 1.2% for HTTP/1 proxied responses in one session (0.7% to 1.7% in another, by shape and load) and about 1.9% for HTTP/2. An effect inside the floor is not a finding, whatever its own interval says.

For a module, the method translates like this:

- **Run the A/A first, on the hardware you will decide on.** Its floor is the smallest module cost you can report from that session; the series also never reports an effect under about 1% from one session, because sessions differ from each other. The series' floors will not transfer.
- **Restart every round and pair rounds in balanced order** (A-B-B-A), with the module or [filter](/docs/filter-selection/) on in one arm and off in the other.
- **Write the plan down before the data:** the pages or traffic shape, the number of rounds, and the result that would make you turn the filter off. Anything chosen after seeing the numbers is Exploratory.
- **Price CPU per response.** The series uses CPU cycles of the server process, user plus kernel, divided by the responses it served.
- **Check that the load reached every worker.** In the series, the load generator's default connection reuse put the load on 2 to 4 of Envoy's 4 workers, and spreading it cut the spread of paired differences from 6.9% to 1.3% on one traffic shape. nginx, Apache and IIS spread requests over several workers or processes too, so check where yours landed.
- **Check what you built.** Three early verdicts in the series, gains of 37% to 89%, were void: they rested on builds without optimization. If you build the server or the module from source, copy the binary out, check its build flags and hash it.

## Fast screens: steer, never decide

[Envoy benchmarking with an extended Nighthawk](https://we-amp.com/writing/envoy-measurement-loop/), the series' second post, describes a faster screen: an agent drives the load generator and Envoy, flips one setting inside 12 short-lived processes, and returns a reading in about half an hour. It was checked against slow restart-paired runs of two known patches: the screen tracked the per-request one (ratio 0.988), and the per-connection check was Not resolved as run. At publication it had screened nothing else. The screen depends on Envoy's runtime guards, which switch a setting without a restart, so it does not carry over to nginx, Apache or IIS, which need a restart to change most settings. Its rule does: a fast reading chooses what goes into the next planned session, and never becomes the number you decide on.

## What the series does not claim

Every number in the series comes from one dedicated server from 2016, running Envoy with 4 workers over loopback. Each number carries a basis label (Measured, No verdict, Not resolved, Exploratory or Modelled), and only Measured numbers become prices. The series has no figure for mod_pagespeed's own CPU cost, and neither does this post.

The harness and the raw data are public at [github.com/We-Amp/envoy-price-list](https://github.com/We-Amp/envoy-price-list). If you run nginx, Apache or IIS, read [how to benchmark a proxy's CPU when the noise is bigger than the effect](https://we-amp.com/writing/proxy-benchmark-method/): its A/A floor, its plan before the data and its labels apply beyond Envoy.
