# Visual scripting: the four ways a node graph goes wrong quietly

Issue #634 (`Scripting/VisualScript/`). Read this before adding a node type,
widening `PinType`, or changing how the VM evaluates. The design rationale is
[ADR 0014](../adr/0014-visual-script-execution-model.md); this document is the
list of things that fail *without* failing a test you already have.

---

## 1. A loop node that does not call `BeginIteration()` hangs the frame

Every loop node (`Flow.ForLoop`, `Flow.WhileLoop`, and anything you add) **must**
call `NodeContext::BeginIteration()` once per iteration and stop when it returns
false. It does two things, and both are load-bearing:

- **Charges the iteration against the per-tick node budget.** Budget is otherwise
  only consumed by `ExecuteFrom`, i.e. by nodes *inside* the body. A `While(true)`
  with an **empty body** consumes nothing at all.
- **Bumps the pure-evaluation memo stamp.** Without it the loop's condition is
  memoized from the first iteration and never re-read, so a loop whose body sets
  the variable the condition reads never terminates.

The two failures compose into the worst case: an empty-bodied `While(true)` spins
forever with no budget consumed and a frozen condition. That exact graph is
`VisualScriptVMTest.GuardRunawayWhileLoopIsHaltedByTheNodeBudget` — if it ever
*hangs* instead of failing, this is why.

## 2. Memoization is per exec STEP, not per tick — and the difference is invisible in a small graph

`m_EvalStamp` is bumped on every exec-node execution. Change it to once-per-tick
and the suite still passes for any graph that reads a variable *before* writing
it; the bug only shows up as `Get Variable` returning a stale value after a `Set`
earlier in the same tick. Which is to say: it shows up in a designer's graph, not
in a test, unless the test deliberately sets-then-gets.

If you add a node that mutates state a **pure** node can observe (a new
blackboard-like store, a component write a pure getter reads), it must be an exec
node. A pure node with side effects breaks the memo's premise: the VM is free to
call `m_Evaluate` any number of times, in any order, or not at all.

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

Component **add/remove** is the one exception, applied inline — and it is safe
*only* because `VisualScriptSystem::Update` iterates a **snapshot of entity
UUIDs** rather than a live EnTT view. If that iteration is ever changed to a live
view, `Entity.AddComponent` / `Entity.RemoveComponent` must move to a deferred
command first. See
[script-structural-command-safe-point.md](script-structural-command-safe-point.md).

## 5. Smaller things that bit during implementation

- **Link endpoints are serialized by pin NAME, not index.** Renaming a pin on an
  existing node type breaks every saved graph that used it — with a *clean*
  "dangling link endpoint" compile error, which is the good outcome, but it is
  still a migration. Adding a pin anywhere is free; renaming one is not.
- **`GetInputEntity` substitutes the owning entity for an unwired pin.** That is
  right for `Target`-style pins (the common case is "me") and wrong for anything
  meaning "everyone" — `Utility.PublishEvent` therefore uses
  `IsInputConnected(pin)` and reads the raw value, so an unwired Target
  broadcasts instead of silently self-addressing.
- **A `Pure` node must not be `Latent`.** A latent node parks a resume record
  keyed by its node index; a pure one is pull-evaluated, possibly several times
  per step, and would park one per pull. `VisualScriptNodeLibraryTest` asserts
  the flags are exclusive.
- **A compile failure is remembered.** `VisualScriptSystem::m_FailedPlans` exists
  so a broken graph logs once, not once per entity per tick for the rest of the
  session. `NotifyGraphReloaded` clears it — a reload is exactly when a broken
  graph may have been fixed.
- **`Function.Call` is not recursive.** Each function graph has one set of value
  slots per instance; re-entry is refused with a reported error rather than
  silently sharing them.

## 6. The component-field registry is generated — do not hand-maintain a second one

`Component.GetField` / `Component.SetField` address a component field by two
strings, resolved through `Scripting/VisualScript/ComponentFieldRegistry.h`. Its
~1.1k entries are **generated** by OloHeaderTool from the same data-member scan
that drives the scene serializer, so a new component is graph-addressable as soon
as it compiles. Three rules follow.

**Do not add a curated table for a component you want reachable.** The older
`Entity.AddComponent` / `Entity.HasComponent` nodes still use one (16 rows, in
`EntityNodes.cpp`) because an EnTT *type* is not reachable from a runtime string;
*fields* are, so nothing here needs a list. To expose a field: make it a public
member of a supported type and rebuild `GenerateBindings`. To keep a runtime-only
field out, tag it `OLO_SERIALIZE(Skip)`; to keep a whole component out, add it to
`kComponentsNotFieldEditable` in `tools/OloHeaderTool/main.cpp` — the set the MCP
registry shares.

**The registry lives under `OloEngine/src`, not `OloEditor/src`, and must stay
there.** The editor's `McpFieldRegistry` is the same idea a layer up, but a graph
runs in `OloRuntime` and `OloServer`, neither of which links the editor.

**`PropType` is lossy in exactly one place that matters here.** `SceneSerType`
folds both `AssetHandle` and `UUID` onto `PropType::AssetHandle`, because scene
YAML round-trips them identically. `PinType` does **not**: an asset reference is
`PinType::Asset` and an entity reference is `PinType::Entity`. `SerField::cppType`
carries the written spelling so the emitter can tell them apart. Get this wrong
and `CheckLinkCompatibility` will happily accept a wire between an entity
reference and an asset slot that the runtime can never satisfy — a graph that
compiles clean and silently does nothing. Pinned by
`ComponentFieldRegistryTest.MappingEntityReferenceIsNotExposedAsAnAsset`.

A write goes through the field's serializer-declared range (the
`OLO_SERIALIZE(Clamp, …)` annotation, or `kHandWrittenFieldClamps`) and then
through a second clamp into the member's own integer width, in that order. Both
are load-bearing: the first keeps a graph from producing a component state a
scene load could not, the second keeps a `9999` written into a `u8` field from
wrapping to `15`. A non-finite float is **refused**, not stored.

That refusal has one trap in it, and it is the kind that tests pass over. Check
`PinValue::IsFinite()`, never `std::isfinite(value.AsFloat())`: the accessor
already maps a non-finite Float to `0.0f`, so the second form can never fail and
a NaN is stored as a plausible-looking `0` and reported as a successful change.
Only `IsFinite` looks at the value that was actually handed over. (Coming through
the VM the guard is belt-and-braces — `NodeContext::SetOutput` sanitizes on the
way into a value slot — but the registry is a public API the editor and tests
call directly, and it has to keep its own promise.)
