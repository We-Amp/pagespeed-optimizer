---
title: "Measuring a server module's CPU cost: start with A/A"
description: 'Before you judge what a module, a filter or an upgrade costs your server in CPU, find the smallest change your test can resolve. A new series shows how.'
date: 2026-11-28
author: 'Otto van der Schaaf'
tags: ['performance', 'benchmarks', 'envoy']
product: '1.1'
draft: false
---

mod_pagespeed runs inside your web server, as a module in nginx, Apache and IIS. Every request it handles runs code in the server's own processes, so before you switch it on, enable another filter or take an upgrade, it is fair to ask what that costs in CPU per request.

The answer is usually a few percent or less, and small numbers are where benchmarks mislead. We-Amp has published [Proxy benchmarking](https://we-amp.com/writing/proxy-benchmarking/), a series on measuring CPU per request when the noise is bigger than the effect. Its measurements are of the Envoy proxy. None of them prices mod_pagespeed, but the method carries over to any module you want to price.

## How big is the noise?

On the series' test server, a dedicated machine with nothing else running, one A/B run of an Envoy build against another cannot tell a 2% CPU change from no change or from a 4% one. Twelve paired rounds resolve about 2% (Modelled, from the measured spreads). Shared or virtualized hardware is usually noisier. If the setting you are judging costs a percent or two, one before-and-after run cannot show it, and even the sign of the result can be chance.

## Start with the same build against itself

The first tool is an A/A run: [the same binary and configuration compared with a copy of themselves](https://we-amp.com/writing/proxy-benchmark-method/#what-does-an-aa-run-tell-you), in the same session, with the same pairing and analysis as the real comparison. Any difference it shows comes from the rig. On the series' server that floor was about 1.2% for HTTP/1 proxied responses in one session and 1.9% for HTTP/2. An effect inside the floor is not a finding, whatever its own interval says.

For a module, the method translates like this:

- **Run the A/A first, on the hardware you will decide on.** Its floor is the smallest module cost you can report; the series' floors will not transfer.
- **Restart every round and pair rounds in balanced order** (A-B-B-A), with the module or filter on in one arm and off in the other.
- **Write the plan down before the data:** the pages or traffic shape, the number of rounds, and the result that would make you turn the filter off. Anything chosen after seeing the numbers is exploratory.
- **Price CPU per response.** The series uses CPU cycles of the server process, user plus kernel, divided by the responses it served.
- **Check that the load reached every worker.** In the series, the load generator's default connection reuse put the load on 2 to 4 of Envoy's 4 workers, and spreading it cut the spread of paired differences from 6.9% to 1.3% on one traffic shape. nginx and Apache spread requests over several workers too, so check where yours landed.
- **Check what you built.** Three early verdicts in the series, gains of 37% to 89%, rested on builds without optimization. If you build the server or the module from source, copy the binary out, check its build flags and hash it.

## A fast screen that only steers

[Envoy benchmarking with an extended Nighthawk](https://we-amp.com/writing/envoy-measurement-loop/), the second part, describes a faster screen: an agent drives the load generator and Envoy, flips one setting inside 12 short-lived processes, and returns a reading in about half an hour. It was checked against slow restart-paired runs before we used it. The screen depends on Envoy's runtime guards, which switch a setting without a restart, so it does not carry over to nginx or Apache as built. Its rule does: a fast reading chooses what goes into the next planned session, and never becomes a published result.

## What the series does not claim

Every number in the series comes from one dedicated server from 2016, running Envoy with 4 workers over loopback. Each number carries a basis label (Measured, No verdict, Not resolved, Exploratory or Modelled), and only Measured numbers become prices. The series has no figure for mod_pagespeed's own CPU cost, and neither does this post.

The harness and the raw data are public at [github.com/We-Amp/envoy-price-list](https://github.com/We-Amp/envoy-price-list). If you run nginx, Apache or IIS, read [part 1](https://we-amp.com/writing/proxy-benchmark-method/): the A/A floor, the plan before the data and the labels are the parts that apply beyond Envoy.
