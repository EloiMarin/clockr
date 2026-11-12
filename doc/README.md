# clockr's documentation

## Disclaimer on agentic prose

One of my personal goals to implement `clockr` was to learn how an FRP engine works under the hood,
so my intention was to write it manually from scratch.
But then came the idea to present it in the [Audio Developer Conference](https://conference.audio.dev/session/2026/functional-reactive-programming-for-audio-software/),
so priorities changed and I started using agents to finish the implementation on time.
Alongside, I also generated its documentation.

I want to progressively humanise the documentation, but be aware that it does read LLM-ish for now.

## Start where you are

**"I've never seen functional reactive programming."**
Run [`examples/first_network`](examples/first_network/main.cpp) first —
with the walkthrough [`learn/your-first-network.md`](learn/your-first-network.md)
beside it: one small program, built and stepped twice, quoted line by line.
Then [`learn/layering.md`](learn/layering.md) builds the mental
model — typed recipes composed in C++, lowered into an erased kernel, walked
by the engine — without assuming any FRP background. When you want the same
discipline but for meaning rather than machinery,
[`learn/semantics.md`](learn/semantics.md) takes you through denotational
semantics itself, and the reference it points to gives the full story.

**"Why is it designed this way?"**
[`explanation/frp-decisions.md`](explanation/frp-decisions.md) records the
decisions — the FRP flavour, the client API shape, the user value types —
with the options that were rejected and the reasons.
[`explanation/operator-sufficiency.md`](explanation/operator-sufficiency.md)
then asks the harder question: is the small operator set enough, and how far
is it *allowed* to fall short.
[`explanation/branchable-live-jam.md`](explanation/branchable-live-jam.md) is the
design for demonstrating scenario replay and branching through a shared musical
performance with agent-reviewed alternatives, followed by a second-priority
concurrent transport/event-interleaving workbench. Its
[`implementation plan`](explanation/branchable-live-jam-plan.md) records the
landed replay substrate (`clockr::replay`), the jam domain, and the remaining
gateway/MCP work. Its next-direction companion,
[`branchable-live-jam-ir.md`](explanation/branchable-live-jam-ir.md), designs a
portable program IR for per-user recipes and input journals.

**"I need the exact meaning."**
[`reference/denotational-semantics.md`](reference/denotational-semantics.md)
is the semantic reference: the domain, the meaning of every construct, and
the laws L1–L9 that any change to the engine must keep true. The glossary
holds the words; the reference holds the mathematics.

**"I want to go deeper."**
[`reference/further-reading.md`](reference/further-reading.md) is the linked
bibliography behind the design: the FRP origins and flavours, the
synchronous-language lineage, the semantics foundations, and prior audio
work.

**"I just want to build against it."**
You are probably leaving this directory. [`how-to/consuming.md`](how-to/consuming.md)
covers FetchContent and install + `find_package`; `CONTRIBUTING.md` (repo
root) covers building and testing this repository; [`how-to/ci.md`](how-to/ci.md)
covers CI and how to move it to another forge. The running examples are in
`examples/`; the debugging tools live in `tools/` (`web-debugger` for the
server and page, `debug-client`, `debug-mcp` and `debug-dsh-plugin` for the
independent TypeScript packages).

**"How do I watch what it is doing?"**
The debugging face of the engine is the observer: a per-tick `.trace` that
`tools/web-debugger` (repo root) turns into a live graph. The semantic model
behind it — snapshots, time travel, why state is data — comes from the
reference; the tool itself has its own `README` next to it.

**"How do I debug it together with an agent?"**
[`how-to/agent-live-debug.md`](how-to/agent-live-debug.md) is the live
feedback loop for a human and a software agent: build and run a host that
serves the debugger, wire an agent to it over MCP
(`tools/debug-mcp`, package `clockr-debug-mcp`), let it watch the trace,
write a boundary input at a tick boundary, and verify the traced effect —
then fix and re-verify against the same running program. The host contract
behind it is in [`../tools/web-debugger/README.md`](../tools/web-debugger/README.md),
and the agent-facing surface in [`../tools/debug-mcp/README.md`](../tools/debug-mcp/README.md).

**"I saw the ADC talk — or missed it."**
[`archive/adc-2026/`](archive/adc-2026/index.md) has the talk page: the
abstract, the presented slides as a PDF (through Q&A), and links to the live demo.

## What lives where

| Path | What it is |
|---|---|
| `learn/` | walkthroughs and mental models for people new to the project |
| `explanation/` | the reasoning behind design decisions |
| `reference/` | precise, load-bearing descriptions (the semantics) and the linked bibliography |
| `how-to/` | operating instructions for building, consuming, CI, and agent-driven live debugging |
| `examples/` | small host programs that consume the library like an external project |
| `archive/` | delivered talks and historical artifacts, read for the journey |

Each module also carries its own `README.md` under `src/<module>/` for the
source-tree view of the same ideas.
