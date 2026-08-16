# ADR 0014 — Visual scripting: exec-token push with synchronous branch descent

- **Status:** Accepted
- **Date:** 2026-08-14
- **Issue:** [#634](https://github.com/drsnuggles8/OloEngineBase/issues/634)

## Context

OloEngine could author audio graphs (SoundGraph), material graphs (ShaderGraph) and
animation-blend graphs (AnimationGraph), but gameplay logic was text-only (C# and Lua).
Issue #634 adds a node-graph gameplay layer.

The obvious move is to reuse the SoundGraph VM. **It does not fit**, and the mismatch is
structural rather than a matter of effort:

| SoundGraph VM | What gameplay logic needs |
|---|---|
| Cycle-free by topological sort | Loops, gates, latches — cycles are the point |
| No control flow: every node runs, every block | Branch: most nodes must NOT run this tick |
| Block-rate `Process(numFrames)` on the audio thread | Event-driven, on the game thread |
| One graph instance per playing sound | One instance per entity, thousands of them |
| No suspend/resume | `Delay`, `WaitForEvent` must survive across frames |

What *does* transfer is the **layering**, and only that: `Asset → Prototype → runtime
instance`, with a type-name-keyed factory (`SoundGraphFactory.cpp`). That layering is what
makes per-entity instancing cheap, and this design copies it.

## Decision

### 1. Two pin kinds, decided at compile time

`PinType::Exec` is a member of the same enum as the data types rather than a separate flag
on the pin. One `CheckLinkCompatibility(source, target)` therefore answers both "may these
connect?" and "does this need a coercion?", and there is exactly one place where control
flow and dataflow could be allowed to cross — so they cannot drift apart between the
compiler and (later) the editor's link-drag feedback.

### 2. Control flow is an exec token pushed by **synchronous descent**, not a work queue

When a node triggers an exec output, the VM runs that whole branch to completion **right
there**, recursively, before returning to the caller.

The alternative — a work queue — was rejected because the loop nodes become hard:
`ForLoop` must run its body N times *and then* continue to `Completed`, and `Sequence` must
finish branch 0 before starting branch 1. With descent both are five lines
(`for (…) { ctx.Trigger(BodyPin); } ctx.Trigger(CompletedPin);`). With a queue, each needs
its own continuation record and the VM grows a scheduler.

The cost is real C++ stack depth, so it is bounded: `kMaxExecDepth = 128`, exceeded ⇒ a
reported error, never a stack overflow. `VisualScriptVMTest.GuardDeepExecChainIsBoundedNotAStackOverflow`
pins it with a 400-node chain.

### 3. Data flows by **lazy pull**, memoized per exec step

`GetInput(pin)` walks the wire to the source node's output. For a **pure** node (no exec
pins) that means *running* it; for an exec node it reads the value slot that node last
wrote.

Memoization is keyed on a stamp bumped **once per exec-node execution**, not once per tick.
Per-tick would be wrong: `Get Variable` is pure, so a `Set` earlier in the same tick would
be invisible to a later `Get`. Per-edge (no memo) would re-evaluate a diamond-shaped pure
sub-graph once per consumer. Per-exec-step is the level at which "nothing changed" is
actually true.

The loop nodes bump the stamp themselves through `NodeContext::BeginIteration()` — which
is also what charges the iteration against the node budget. Both matter: a `While` with an
**empty body** would otherwise never refresh its condition *and* never consume budget, and
would hang the frame. That exact graph is a test.

### 4. Cycles: rejected among pure nodes, allowed among exec nodes

Only pure nodes are pull-evaluated recursively, so only a pure cycle can spin forever. An
exec-level feedback loop reads stored value slots and terminates by construction — and is
useful (a FlipFlop feeding a DoOnce's Reset). The compiler's DFS therefore follows data
edges **into pure nodes only**. Both directions are tested.

### 5. Latent nodes suspend by parking a resume record

`Delay` and `WaitForEvent` return without triggering, leaving
`{graph, node, resumePin, remaining | eventKey}` on the instance. Timers are advanced at
the top of `Tick` — *before* `OnUpdate` fires, so a delay expiring this frame runs its
continuation in authored order rather than one tick late. Event waits are resumed by
`DispatchEvent`.

**Not supported: recursion.** Each function graph gets one set of value slots per instance,
and a re-entrant `Function.Call` is refused with a reported error rather than silently
sharing them. Loops cover the cases recursion would; supporting it needs a per-call frame
stack, which is a follow-up if anyone actually hits it.

### 6. Everything that could escape the tick is queued

- **Entity create/destroy** goes through Scene's existing deferred command queue, and
  `VisualScriptSystem::Update` brackets itself with `FlushPendingEntityCommands()` exactly
  as `Scene::UpdateScripts` does. See
  [script-structural-command-safe-point.md](../agent-rules/script-structural-command-safe-point.md).
- **Event publishing** goes to an outbox drained between iterations, never inline —
  `GameplayEventBus::Publish` is synchronous and a graph publishing mid-iteration is the
  hazard that document is about.
- **Component add/remove** is applied inline, which is safe *only because* the system
  iterates a **snapshot of entity UUIDs** rather than a live EnTT view. If that ever
  changes, those two nodes must move to the queue first.

### 7. Bus subscriptions are per **scene**, not per entity

`GameplayEventBus::Subscribe` returns no token and there is no unsubscribe — handlers live
until `Clear()`. A per-instance subscription would therefore dangle the instant that entity
died. One handler per scene, fanning out to whichever instances are alive at delivery time,
has no such window, and needed **no API change** to a header Quest/Inventory/Progression
all include.

Graph-authored events travel as a single `VisualScriptCustomEvent` payload (name + string)
because a graph cannot name an arbitrary C++ type and the bus is keyed by `std::type_index`.
Engine events flow the other way through a **curated** bridge table
(`VisualScriptSystem::SubscribeToGameplayBus`) — curated because each entry decides what the
graph-visible payload *is*, which a generic forwarder cannot.

### 8. `VisualScriptSystem` is an unmarked scheduler node, and must stay one

Registered `After("Scripts")` with `ReadsWrites(kLocalTransforms)`. Unmarked (join-all
barrier) for the same two reasons `Scripts` is: it drains the entity-command queue (EnTT
structural changes) and it publishes to the synchronous bus. Marking it `.Parallelizable()`
requires moving both out to a barrier node first.

## Consequences

**Good.** Loop and sequence nodes are trivial. A diamond-shaped pure sub-graph is computed
once per exec step. Per-entity instancing is a `std::vector` resize over a shared plan.
Every unsafe interaction has exactly one queue in front of it.

**Bad.** Exec descent consumes C++ stack, so a legal-but-deep graph hits a depth error
rather than running — bounded, reported, but a real limit. Recursion is unsupported.
A per-exec-step memo is more evaluations than a per-tick one for a graph whose pure inputs
genuinely never change.

**Deliberately not built** — carried to [#793](https://github.com/drsnuggles8/OloEngineBase/issues/793):

- **Node-granular single-step.** The debugger breaks *before* a node and can resume or run
  one more whole tick, but it cannot advance one node at a time. Exec descent has no
  resumable continuation, so "step one node" would mean re-running the tick from its entry
  and repeating every side effect that preceded the breakpoint — a debugger that lies about
  what it just did is worse than one that admits its granularity. Doing it properly means a
  continuation-based VM.
- **A `Get/Set arbitrary component field` node.** Needs an *engine-side* field registry; the
  generated `McpFieldRegistry` lives under `OloEditor/`.
- **Migrating the seven existing canvases onto `GraphCanvas`** (below).

### The GraphCanvas question — answered by building the widget, not by extracting it

There are seven hand-rolled ImGui graph canvases in this repo (~8.6k lines) with zero shared
code, and `VisualScriptEditorPanel` is the eighth graph editor. Three options were real:

1. **Write an eighth private copy.** Cheapest now, and permanently entrenches the
   duplication at its worst point — this is the largest and most feature-rich canvas.
2. **Extract `GraphCanvas` out of `ShaderGraphEditorPanel` first, as its own PR.** The
   cleanest end state, but it edits a working 1,507-line panel whose only regression net is
   `ShaderGraphCommandTest.cpp`, and it blocks this work behind an unrelated refactor.
3. **Write `GraphCanvas` as new code, consume it here, migrate the others later.**

**Taken: (3).** `OloEditor/src/Panels/Graph/GraphCanvas.h` holds the viewport half — pan,
zoom, grid, the two coordinate transforms, bezier wire drawing, wire hit-testing. The panel
holds node layout, hit-testing, selection and link semantics, which is where graph editors
genuinely differ. No existing panel was touched, so nothing could regress; the widget starts
with a real consumer rather than being designed against seven at once; and the migration
option (2) proposed becomes a smaller incremental change per panel, against an interface
that already works, instead of a big-bang refactor.

The cost is honest: until that migration happens there are eight canvases and only one of
them is shared. That is strictly better than nine, and it is tracked.
