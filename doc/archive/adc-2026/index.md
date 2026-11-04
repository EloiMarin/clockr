# Functional Reactive Programming for Audio Software — ADC 2026

*What a real-time concurrent FRP engine can do for us.*

Talk at the [Audio Developer
Conference 2026](https://conference.audio.dev/session/2026/functional-reactive-programming-for-audio-software/),
Tuesday 10 November 2026, 15:00–15:50 UTC, Conservatory.

**Slides:** [slides.pdf](slides.pdf) — the slides presented in the talk,
through the Q&A page, as a PDF.

## Abstract

> "Functional Reactive Programming (FRP) is a declarative way to create
> reactive systems." Most audio applications are reactive systems, and FRP's
> semantics map especially well to their temporal demands. The complex web of
> temporal interactions, synchronisation, data flow and state updates that
> happen between the audio thread, the UI thread, background workers, and so
> on could be expressed in FRP purely and declaratively instead of as an
> implicit flow of sparse callbacks and ad-hoc concurrent primitives.
>
> FRP has already been successfully applied to music and audio (see
> [Euterpea](https://www.euterpea.com/) or
> [YampaSynth](https://hackage.haskell.org/package/YampaSynth)), but mainly as
> closed systems for academic research and without general guarantees of
> real-time thread-safe operation — a notoriously difficult problem for FRP
> engines, but a non-negotiable for many audio applications.
>
> This talk proposes the architecture of an FRP engine capable of sitting at
> the core of audio applications, orchestrating and sharing data across
> threads while keeping real-time guarantees for the critical paths. The key
> move is to make the graph topology thread-aware: the engine knows which
> parts of the graph belong to the UI, audio, or worker threads, and can
> allocate and manage them accordingly. The talk presents a wormhole
> primitive for bounded-time snapshot exchange at explicit synchronisation
> points, and starts with a live demo of a toy sequencer with variable tempo
> and loops.

## What the talk covers

1. **The demo.** DAWmulator: a playhead driven by the audio thread, with
   seeking, tempo changes, and looping wired entirely in the functional
   graph. The live page is at
   [clockr.emgware.com/demo](https://clockr.emgware.com/demo/).
2. **The semantics.** Why the engine is clocked synchronous rather than
   classic FRP: discrete ticks, bounded state, and explicit clocks are what
   make real-time bounds and thread-awareness expressible in the model.
3. **The engine.** The typed shell and erased kernel, regions and crossings,
   the `latest` and `queued` link policies, allocation-free ticks, and the
   `debug_engine` trace that feeds the live graph view.
4. **The tools.** The web debugger and the MCP path that lets an agent
   observe and inject at tick boundaries.

## Going further

- [Further reading](../../reference/further-reading.md) collects the papers,
  books, and talks behind the design.
- The [documentation index](../../README.md) maps the rest of the design
  record.
- The source is at
  [github.com/EloiMarin/clockr](https://github.com/EloiMarin/clockr).

## Credits

The colour QR code on the closing Q&A slide, which scans to
[clockr.emgware.com](https://clockr.emgware.com), was generated with
[1mentat/qr-code-shenanigans](https://github.com/1mentat/qr-code-shenanigans),
a generator for full-colour photographic QR codes that remain scannable.
