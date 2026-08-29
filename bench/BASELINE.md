# Baseline: where wayvnc loses to Xvnc, and why

Recorded 2026-08-28 on a Gen5 (Ironlake) ThinkPad, loopback only. Both
servers on the same machine under an equivalent workload (a terminal
scrolling `date`), so load and drift affect both equally. **The ratio is the
measurement**; absolute numbers are not comparable across sessions or
workload revisions.

    bench/vncbench.py --ab --workload scroll --encoding <enc> --secs 6 --repeat 2

## What this harness can and cannot measure

It runs on the same machine as the servers and talks to them over loopback,
and its client is Python. Both matter:

  * **Loopback has no bandwidth limit worth finding.** The estimator
    correctly saturates here. Nothing this harness reports says anything
    about how the congestion controller behaves on a real link.
  * **The client is slower than the link.** A round trip is dominated by
    Python parsing the rectangles, not by bytes on the wire, so measured
    round-trip time is a property of the harness.

So congestion control has to be judged against a real viewer over the real
link. What the harness is good for is encoder bytes, and for catching
outright faults in the code path -- which it did: a zero estimate pinning
the server in permanent slow start, and a bandwidth figure overflowing the
int it was returned in. Both showed up here as an obvious 0.4 fps and would
have been invisible in casual use.

## Congestion control, loopback A/B

Single client, server restarted before each run.

| build | fps | drops | estimate |
|---|---|---|---|
| before any bandwidth work | 7.8, 6.6 | 11, 12 | median ~5 MB/s |
| after | 8.1, 8.1, 7.2 | 0, 0, 0 | saturating |

The throughput difference is small and partly noise, which is expected:
loopback was never bandwidth-limited, so a better-behaved limiter has
little room to help. The drops going to zero is the real result, along with
an estimate that no longer collapses to zero.

## What fences cost

Measured with `--encoding raw`, the only mode that parses messages exactly
and can therefore answer a fence request:

| harness | fps | KiB/update |
|---|---|---|
| `--no-fence` (congestion control inert) | 8.5 | 106.7 |
| fences answered | 7.1 | 134.4 |

So the congestion controller costs about 16% here, not the order of
magnitude a first, broken measurement suggested. That first run advertised
fences from the quiet-boundary path, which reads undifferentiated bytes and
cannot answer them; the in-flight byte count grew forever and the server
throttled itself to a standstill. The harness now refuses that combination
outright. Note also that this client answers a fence the instant it arrives,
where a real viewer answers on its own paint cadence -- 208ms for a 1KiB
frame was observed -- so 16% is a floor, not the figure a viewer sees.

> **The per-encoding table below predates fence support in the harness.** They were taken by
> a client that did not advertise the `fence` pseudo-encoding, so neatvnc's
> congestion control never engaged and never dropped a frame. That path is
> where the real cost turned out to be: against a fence-capable viewer the
> server was discarding most frames and pipelining four full screens at
> startup before any acknowledgement. Treat everything below as an encoder
> comparison only, and re-record it now that `vncbench.py` speaks fences.

## By encoding

| encoding | wayvnc | Xvnc | ratio |
|---|---|---|---|
| tight  | 4.78-5.93 KiB/upd | **0.09 KiB/upd** | **59x** |
| zrle   | 2.60-3.51 KiB/upd | 0.08 KiB/upd | 37x |
| raw    | 86-87 KiB/upd     | 2.3-3.0 KiB/upd | 33x |

Xvnc sends roughly **100 bytes** for a scrolled line under tight. The gap is
widest with the encoding real clients actually negotiate, so measuring only
with raw understates it.

## The gap decomposes into two independent effects

**1. More damaged pixels.** ~5x. A scroll in X11 is `XCopyArea`: the server
moves the pixels and only the newly exposed line is damaged. foot must
re-render the shifted text into a fresh buffer, so every tile containing
text changes. This is the one scroll detection addresses.

**2. Worse bytes per damaged pixel.** Xvnc ~0.13 B/px, wayvnc ~1.4 B/px --
about 10x. Two candidate causes were tested and **both were wrong**:

- *Antialiased vs bitmap glyphs*: no effect. Measured foot at its default
  font, Terminus 16 and Terminus 12 -- 5.06, 5.70 and 5.03 KiB/update
  respectively, and an identical 8192 px damaged in all three. The X11
  bitmap-font reasoning does not transfer.
- *A cheaper encoding available only to Xvnc*: no. Both servers were driven
  through tight, zrle, hextile and raw; the ordering never changed.

The identical 8192 px across three font sizes is the tell: damage is not
tracking the text, it is **quantised**. `src/damage-refinery.c` rounds every
damage rectangle to 32x32 tiles:

```c
int x1 = rects[i].x1 / 32;
int x2 = UDIV_UP(rects[i].x2, 32);
```

So the minimum damage unit is 1024 px whatever changed. That matches the
measured single-character cost exactly: 1024 px on wayvnc against 400 px on
Xvnc, which sends the real rectangle.

Note the refinery is still a large net win -- wayvnc's own counters report
~29.5% of the screen damaged per frame from the compositor, refined down to
around 8192 px. Tile quantisation is a floor on small updates, not the
dominant cost of scrolling.

## Success criteria

Both ratios approaching 1.0x, with `copyrect` appearing in wayvnc's rect
breakdown -- the scrolled region sent as a 12-byte rectangle instead of
pixels. Tile quantisation sets a floor below which this cannot go without
also making the refinery finer.

## Guardrails, and the wrong conclusion each came from

- workload verified by **heartbeat file**, not process liveness -- a dead
  generator made the server look like it stalled for seconds; and counting
  processes fails because `pgrep -f` matches both the terminal and its shell
- **stale generators killed before** a run -- leftovers inflated a target to
  154 KiB/update against a 5.7 KiB/update baseline
- next request sent **the instant an update is parsed**, never on a timer --
  an earlier harness measured its own 1s retry interval
- every rect **parsed to completion**; encodings that cannot be
  length-decoded fall back to an explicitly labelled `~approx`
  quiet-boundary mode rather than a guessed byte count
- runs **repeated**, reported individually, never a lone sample
- full-screen updates flagged **SUSPECT** and excluded from the ratio; if
  that leaves a target with no usable runs, no ratio is printed at all
