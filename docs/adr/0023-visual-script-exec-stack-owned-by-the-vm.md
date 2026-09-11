# ADR 0023 — Visual scripting: the exec stack moves onto the instance

- **Status:** Accepted
- **Date:** 2026-09-10
- **Issue:** [#1069](https://github.com/drsnuggles8/OloEngineBase/issues/1069)
- **Supersedes:** [ADR 0014](0014-visual-script-execution-model.md) §2 (exec token push by
  *synchronous descent*). Every other section of ADR 0014 stands unchanged.

## Decision

Control flow is an exec token pushed onto a **stack the `VisualScriptInstance` owns**, drained by a
loop, instead of a token pushed by C++ recursion. `NodeContext::Trigger` stops recursing and starts
pushing; `ExecuteFrom` becomes `Drain`.

This is issue #1069's option 1, and it is taken because the measurement below says it costs four
call sites rather than the seventy the issue estimated.

It buys the feature #1069 asked for — pausing is "stop draining", stepping is "pop exactly one" —
and it closes a hole the old model could not: **today's Resume does not resume.**

## The measurement that decided it

#1069 sized option 1 as "a rewrite of `VisualScriptVM.cpp`'s core that 70+ node bodies sit on top
of". That is the count of node bodies, not the count of bodies that *care*. The number that matters
is how many call `Trigger` and then keep working, because only those need their local state to
survive a suspension.

Counted over `Scripting/VisualScript/Nodes/*.cpp` at `2dacd7ad2` — **42 `Trigger` call sites, 39 of
them tail calls**: the trigger is the last statement on its path, followed by nothing or by `return`.
A tail trigger needs no continuation at all. "Recurse into the branch and come back" and "push the
branch and return" are the same thing when there is no *back* to come to.

The three that are not tail calls, plus one inside the VM:

| Site | What follows the trigger |
|---|---|
| `Flow.Sequence` | the next output pin |
| `Flow.ForLoop` | the next index, then `Completed` |
| `Flow.WhileLoop` | the condition re-read, then `Completed` |
| `NodeContext::CallFunction` | marshalling the callee's results back onto the call node |

Four sites. That is the whole of the rewrite's blast radius on behaviour, and it is why option 1 is
affordable here in a way the issue did not expect.

## What ADR 0014 §2 bought, and what happens to it

§2 chose descent for two named reasons. Both survive, and one improves.

**"`ForLoop` and `Sequence` stay five lines each."**

- `Sequence` keeps it *unchanged*, for free. The stack is LIFO, so triggering the output pins in
  reverse makes them pop in order, and each branch's own successors land on top of the pins still
  waiting. Depth-first drain — branch 0 completing before branch 1 begins — is a property of the
  stack, not of the node.
- `WhileLoop` gets **shorter**: the C++ `while` disappears, because re-entry is the iteration.
- `ForLoop` goes from 5 lines to 7: the index moves from a C++ local into the `NodeState` the VM
  already allocates for every node, and the body asks `ctx.IsResume()` whether this is iteration
  zero.

One new `NodeContext` primitive carries all three: `TriggerAndReturn(pin)`, meaning *run this
branch, then re-enter me*. It pushes a resume frame under the branch. The ADR 0014 argument against
a work queue — "each loop node needs its own continuation record and the VM grows a scheduler" —
was right about the record and wrong about the scheduler: the record is `NodeState`, which already
exists, and the drain loop is not a scheduler because the stack fixes the order.

**"`kMaxExecDepth = 128`, exceeded ⇒ a reported error, never a stack overflow."**

Kept, and it stops being load-bearing for the reason it existed. Exec chains no longer consume C++
stack at all, so a deep graph cannot overflow one. The bound is carried per stack frame and still
reports the same error, now as a **policy** limit — a graph that was an error before must not
silently become legal — rather than as stack safety.

It remains genuine stack safety for the **pure-pull evaluator**, which is still real C++ recursion
(`EvaluateOutput` → `EvaluateInput` → `EvaluateOutput`) and still bounded by the same constant.
`GuardDeepExecChainIsBoundedNotAStackOverflow` keeps passing, for a slightly different reason than
its comment says; the comment is updated with it.

## The hole this closes

Breaking at a node sets `m_Paused`, and `ExecuteFrom` re-checks that flag on entry so the rest of
the run unwinds instead of executing behind the paused canvas. That is right, and
`BreakpointPauseAbandonsEveryLaterBranchInTheRun` pins it.

But the work that unwound is **gone**. `DebugResume()` clears the flag and the graph starts again
from the *next tick's* entry points. A `Sequence` broken on in branch 0 never runs branches 1..N for
that tick; a `ForLoop` broken on in iteration 3 drops iterations 3..N. The button says **Resume**
and the tooltip does not mention it.

So the current debugger cannot break anywhere without changing what the graph does. With the stack
on the instance, the abandoned work is still *on* the stack: resume drains it, and the tick
completes exactly as it would have. Stepping is the feature that was asked for; this is the one that
was wrong.

## Why not the other two

**Option 2 — coroutines.** Dominated by option 1 once the tail-call ratio is known. Making each exec
body awaitable pays a frame allocation on all ~70 node types, on a path that runs per entity per
tick under a 10 000-node budget, to make 3 of them easier to write — and those 3 are already easy
with a resume frame. Keeping `m_Execute` for the plain bodies and adding `m_ExecuteAsync` for the
three avoids the allocation and buys a permanently forked execution path in its place, which is
worse than what it fixes. Pooling the frames is possible (a promise-type `operator new` over a
per-instance arena) and is real work with a nesting-shaped failure mode, spent on a cost option 1
never incurs.

**Option 3 — decline, and build a trace scrubber instead.** Costed honestly and rejected on two
counts.

The first is that it is not as cheap as #1069 assumed. "The execution order is already recorded" is
half true: `DebugState::m_ExecutionOrder` is `node → last position`, one entry per node, so a node
that ran five times in a loop leaves one. It records the *set* of nodes that ran with where each was
last seen, not the sequence. A scrubber needs an actual append-only history, which is a new
structure and a new bound on it.

The second is that a scrubber cannot answer the questions stepping is for. `m_ExecutionOrder` and
the pin-value watch read live slots, which have moved on by the time you scrub; there is no value
history without recording one, and recording one for every pin of every node is a much larger
structure than the exec stack this ADR adds. And a replay cannot stop *before* a side effect, which
is the thing a debugger is for.

Declining would also have made the Resume hole permanent, because there is nothing to resume from.
That is not a trade #1069 knew it was making.

## Consequences

**Good.** Node-granular stepping, honestly: one pop, one node, side effects exactly once. Resume
finishes the tick it interrupted. Exec chains cannot overflow the C++ stack. `Sequence` is untouched
and `WhileLoop` shrinks.

**Bad, and specifically.**

- `VisualScriptInstance` grows a frame vector. Empty while idle, and it is one allocation amortised
  over the instance's life once it has run a tick, but it is per entity and there are thousands.
- **An exec cycle that re-enters a loop node still mid-iteration now clobbers that iteration**
  instead of nesting. Under descent, a `ForLoop` whose body wires back to its own `Enter` got a
  fresh C++ frame with its own index and recursed until the depth cap; the index now lives in
  `NodeState`, so the re-entry resets it. Exec cycles are legal (ADR 0014 §4) and useful, so this is
  a real behaviour change on a pathological graph. It is refused with a reported error rather than
  silently miscounting, and it is a test.
- A resume frame must **not** charge the node budget — `BeginIteration()` already charges the
  iteration, and charging both would halve every loop's effective budget.
- The debugger now has semantics to define where it had none: what a step does at a latent suspend
  (nothing — the stack is empty until the wait resumes), at a function boundary (steps into it), and
  at the end of a tick.

**Unchanged.** Latent suspend/resume semantics are preserved exactly, and for free: a node that
suspends pushes nothing, so the drain moves on to whatever was already stacked — which is what
returning without triggering did under descent. A `Delay` in branch 0 of a `Sequence` still does not
hold up branch 1. Per-exec-step memoization (§3), pure-cycle rejection (§4), the latent record (§5),
the four queued escapes (§6), per-scene subscriptions (§7) and the scheduler registration (§8) are
untouched.
