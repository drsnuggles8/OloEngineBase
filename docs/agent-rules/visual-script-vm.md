# Visual scripting: the four ways a node graph goes wrong quietly

Issue #634 (`Scripting/VisualScript/`). Read this before adding a node type,
widening `PinType`, or changing how the VM evaluates. The rationale is
[ADR 0014](../adr/0014-visual-script-execution-model.md), whose §2 is superseded by
[ADR 0023](../adr/0023-visual-script-exec-stack-owned-by-the-vm.md); this document
is the list of things that fail *without* failing a test you already have.

---

## 1. `Trigger` queues a branch, it does not run one — and a loop node pays for that

The exec stack lives on the `VisualScriptInstance` and is drained by a loop
(ADR 0023), which is what gives the debugger something to pause and resume. A node
body therefore **returns between exec steps**, and nothing may live on the C++
stack across a `Trigger`.

For a body that triggers *last* — 39 of the 42 sites — nothing changes: there is
no "after the branch" to lose. For one that needs control back, `Trigger` is the
wrong tool and the failure is silent, because `ctx.Trigger(body); doMoreWork();`
runs `doMoreWork` **before** the body. Use `ctx.TriggerAndReturn(pin)`, which
re-enters this body with `ctx.IsResume()` true. Never mix the two in one entry:
the resume would arrive before a later `Trigger`'s branch.

**A loop node** (`Flow.ForLoop`, `Flow.WhileLoop`, anything you add) has two
obligations on top of that, and both are load-bearing.

- **Call `NodeContext::BeginIteration()` once per iteration and stop when it
  returns false.** It charges the iteration against the per-tick node budget —
  otherwise only *running a node* consumes budget, so a `While(true)` with an
  empty body consumes nothing — and it bumps the pure-evaluation memo stamp,
  without which the condition is memoized from the first iteration and a loop
  whose body sets the variable the condition reads never terminates. The two
  compose into the worst case: an empty-bodied `While(true)` spinning forever on
  a frozen condition. That exact graph is
  `VisualScriptVMTest.GuardRunawayWhileLoopIsHaltedByTheNodeBudget` — if it ever
  *hangs* instead of failing, this is why.
- **Keep the iteration in `State()`, never in a C++ local.** Read the bounds once
  on the `!IsResume()` entry, and use `NodeState::m_Flag` — only that field — as
  the "still iterating" marker. The VM clears `m_Flag` when it discards a pending
  re-entry, which is what stops a loop the budget abandoned from refusing itself
  for the rest of play.

Two consequences elsewhere:

- **`kMaxExecDepth` no longer guards the C++ stack for exec.** A chain costs no
  C++ frames, so the bound rides on the exec frame as policy — a graph that was a
  reported error stays one. It is still real stack safety for the **pure-pull**
  evaluator, which is still recursion.
- **Whatever a body queues is flipped once when the body returns**, so it pops in
  written order. That one `std::reverse` in `RunFrame` is why `Sequence` needed no
  change; remove it and every multi-branch node runs its branches backwards.

## 2. Memoization is per exec STEP, not per tick — and the difference is invisible in a small graph

`m_EvalStamp` is bumped on every exec-node execution. Change it to once-per-tick
and the suite still passes for any graph that reads a variable *before* writing it;
the bug only shows up as `Get Variable` returning a stale value after a `Set`
earlier in the same tick — in a designer's graph, not in a test, unless the test
deliberately sets-then-gets.

A node that mutates state a **pure** node can observe (a new blackboard-like store,
a component write a pure getter reads) must be an exec node. A pure node with side
effects breaks the memo's premise: the VM may call `m_Evaluate` any number of
times, in any order, or not at all.

## 3. `PinType` is the contract; widening it has five consumers

Adding an enumerator means touching, in this order:

1. `PinTypeToString` / `PinTypeFromString` — a missing case silently round-trips
   as `Exec`, so the pin loses its type on save.
2. `PinValue::DefaultFor`, the `As*` accessors, `ConvertTo`, `ToStorageString`,
   `FromStorageString`, `IsFinite`, `SanitizeNonFinite`, `operator==`.
3. `CheckLinkCompatibility` — a type absent from every branch falls through to
   `Incompatible`, which is at least loud.
4. The Lua (`LuaScriptGlue.cpp`'s `visual_script` table) and C#
   (`ScriptGlue.cpp`'s `VisualScript_*`) marshalling.
5. The hand-written `VisualScriptComponent` block in `SceneSerializer.cpp` and the
   `SaveGameComponentSerializer` overload — both persist a `PinType` as its
   integer/name, so a *reordered* enum silently reinterprets saved overrides.

**Never reorder or renumber the existing enumerators.** The save-game path writes
`static_cast<u8>(value.GetType())`, so the numbering is on disk.

## 4. The four things that must never happen inline

The VM sits in the middle of an ECS iteration, so each of these is queued:

| Operation | Why not inline | Where it goes |
|---|---|---|
| Entity create / destroy | Invalidates the EnTT pool being iterated | `Scene::Script*` → `FlushPendingEntityCommands` |
| `GameplayEventBus::Publish` | Synchronous dispatch reaching arbitrary subscribers | `RuntimeContext::m_EventOutbox`, drained between iterations |
| A graph event reaching another graph | Same, plus re-entrancy | `VisualScriptSystem::m_Inbox` |
| A physics contact reaching a graph | Fires inside the contact drain, inside the physics fence | `VisualScriptSystem::QueueContact` |

Component **add/remove** is the one exception, applied inline — safe *only*
because `VisualScriptSystem::Update` iterates a **snapshot of entity UUIDs**, not a
live EnTT view. Change that iteration to a live view and those two nodes must move
to a deferred command first. See
[script-structural-command-safe-point.md](script-structural-command-safe-point.md).

## 5. Smaller rules that are easy to miss

- **Link endpoints are serialized by pin NAME, not index.** Renaming a pin on an
  existing node type breaks every saved graph that used it — with a *clean*
  "dangling link endpoint" compile error, which is the good outcome, but it is
  still a migration. Adding a pin anywhere is free; renaming one is not.
- **`GetInputEntity` substitutes the owning entity for an unwired pin.** Right
  for a `Target`-style pin ("me"), wrong for one meaning "everyone" — a node like
  `Utility.PublishEvent` must use `IsInputConnected(pin)` and read the raw value,
  or an unwired Target self-addresses instead of broadcasting.
- **A `Pure` node must not be `Latent`.** A latent node parks a resume record
  keyed by its node index; a pure one is pull-evaluated, possibly several times
  per step, and would park one per pull. `VisualScriptNodeLibraryTest` asserts
  the flags are exclusive.
- **A compile failure is remembered.** `VisualScriptSystem::m_FailedPlans` makes
  a broken graph log once, not once per entity per tick forever. Clear it from
  `NotifyGraphReloaded` — a reload is when a broken graph may have been fixed.
- **`Function.Call` is not recursive.** Each function graph has one set of value
  slots per instance; re-entry is refused with a reported error rather than
  silently sharing them. The guard is released by the call's bookkeeping frame,
  not by the node body — so a code path that drops queued work must run those
  frames (`UnwindAbandonedWork` does) or the function stays locked for the rest
  of play.
- **A loop re-entered through an exec cycle is refused, not nested.** Exec cycles
  are legal (ADR 0014 §4), so a body can wire back into its own loop's `Enter`.
  The iteration lives in `NodeState` since ADR 0023, so a second entry would reset
  the first's index; it reports an error instead. Descent nested it.

## 6. The component-field registry is generated — do not hand-maintain a second one

`Component.GetField` / `Component.SetField` address a field by two strings through
`Scripting/VisualScript/ComponentFieldRegistry.h`. Its ~1.1k entries are
**generated** by OloHeaderTool from the same data-member scan that drives the scene
serializer, so a new component is graph-addressable as soon as it compiles. Four
rules follow.

**Do not add a curated table for a component you want reachable.** The older
`Entity.AddComponent` / `Entity.HasComponent` nodes still use one (16 rows, in
`EntityNodes.cpp`) because an EnTT *type* is not reachable from a runtime string;
*fields* are. To expose a field, make it a public member of a supported type and
rebuild `GenerateBindings`. To keep a runtime-only field out, tag it
`OLO_SERIALIZE(Skip)`; to keep a whole component out, add it to
`kComponentsNotFieldEditable` in `tools/OloHeaderTool/main.cpp` — the set the MCP
registry shares.

**The registry lives under `OloEngine/src`, not `OloEditor/src`, and must stay
there.** The editor's `McpFieldRegistry` is the same idea a layer up, but a graph
runs in `OloRuntime` and `OloServer`, neither of which links the editor.

**Keep `AssetHandle` and `UUID` apart when emitting.** `SceneSerType` folds both
onto `PropType::AssetHandle` because scene YAML round-trips them identically;
`PinType` does **not**, so read the written spelling from `SerField::cppType`. Get
it wrong and `CheckLinkCompatibility` accepts a wire between an entity reference
and an asset slot the runtime can never satisfy — a graph that compiles clean and
silently does nothing. Pinned by
`ComponentFieldRegistryTest.MappingEntityReferenceIsNotExposedAsAnAsset`.

**Check `PinValue::IsFinite()`, never `std::isfinite(value.AsFloat())`.** The
accessor already maps a non-finite Float to `0.0f`, so the second form can never
fail: a NaN is stored as a plausible-looking `0` and reported as a successful
change. Only `IsFinite` sees the value that was handed over. A write then passes
the field's declared range (`OLO_SERIALIZE(Clamp, …)` or `kHandWrittenFieldClamps`)
and then the member's own integer width, in that order — the first stops a graph
producing a state a scene load could not, the second stops `9999` into a `u8`
wrapping to `15`. A non-finite float is refused, not stored.
