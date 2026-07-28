# Script-driven structural registry changes need a deferred command queue

Issue #643 (`Scene::ScriptCreateEntity` / `ScriptInstantiatePrefab` /
`ScriptDestroyEntity`). Read this before exposing **any** new script binding
that creates or destroys an entity, adds or removes a component, or reparents a
hierarchy — i.e. anything that mutates the EnTT registry *structurally* rather
than writing a field inside an existing component.

---

## The rule

**A script may never apply a structural registry change inline. It queues a
command; the engine applies the queue at a drain point that is guaranteed to sit
outside every EnTT view/group iteration.**

The queue lives on `Scene` (`m_PendingEntityCommands` + the two pending-UUID
sets, all mutex-guarded). The drain is `Scene::FlushPendingEntityCommands()`,
called at the **top and bottom of `Scene::UpdateScripts`**.

## Why inline is wrong even though scripts run on the game thread

It is tempting to reason "the `Scripts` scheduler node is unmarked, so it's a
join-all barrier on the game thread, so there's no race — just do it inline."
That reasoning is about *threads*, and the hazard here is about *iterators*.

`Scene::UpdateScripts` dispatches `OnUpdate` while walking
`m_Registry.view<ScriptComponent>()` and `view<LuaScriptComponent>()`. During
that walk:

- **Spawning a prefab whose root carries a `ScriptComponent` pushes into the
  very pool being iterated.** entt may reallocate the pool; the live iterator is
  then dangling.
- **Destroying an entity that has a script component swap-and-pops out of that
  pool**, so the iteration silently skips an entity or visits one twice.
- `LuaScriptEngine::OnUpdateEntity` and `ScriptEngine::OnUpdateEntity` still
  touch the entity *after* the callback returns, so a script destroying the
  entity it is running on is a plain use-after-free.

None of these fail at the call site. They fail later — in an unrelated system,
often in an unrelated test — which is exactly the failure mode that costs a day
to bisect.

## Why "always defer" and not "defer only inside UpdateScripts"

An earlier design considered an RAII deferral scope entered by `UpdateScripts`,
with the binding applying inline when outside it. Reject that: script callbacks
are invoked from *several* engine call sites, most of which are themselves
mid-iteration — dialogue node actions, UI button handlers, `GameplayEventBus`
subscribers, GOAP action callbacks. A scope-based rule is only correct if every
one of those sites remembers to open the scope, and the failure when one forgets
is silent corruption. One uniform contract cannot be broken by adding a new
script-invoking call site.

The cost of always deferring is a queue-empty bool check per drain.

## Why the drains bracket the script loops (two calls, not one)

- **Trailing drain** — applies what *this* tick's `OnUpdate` callbacks queued.
  This is what makes a script spawn visible to every downstream system in the
  same tick: physics builds its body, `PropagateTransforms` composes its world
  matrix, the frame renders it. A single drain placed anywhere later in the
  schedule would work too, but it would have to be a new scheduler node.
- **Leading drain** — applies anything queued *after* last tick's trailing drain,
  i.e. from those other call sites above. Without it such a request would sit
  until something else happened to flush, which is unbounded latency; with it,
  the worst case is one tick.

## The API shape this forces (and why it is the right shape anyway)

A deferred spawn cannot return a live entity. It returns a **pre-allocated
UUID**, and the entity materialises at the drain. Consequences, all deliberate:

- **The spawn transform is an argument**, not something the caller assigns
  afterwards. `Instantiate(handle, pos, rot, scale)` — because
  `var e = Instantiate(h); e.Translation = p;` would silently write to nothing.
- **`Entity.IsValid` reports the *logical* answer, not the raw registry one**
  (`Scene::IsEntityLiveForScripts`): `true` for a handle whose spawn is still
  pending, `false` from the instant a destroy is requested. Without this, a
  script cannot tell its own fresh spawn from a dead entity, and sees its own
  `Destroy()` not take effect for the rest of the callback.
- **Destroy is idempotent.** Two `Destroy` calls on the same target queue one
  command (`m_PendingDestroyIDs.insert(...).second` is the gate). Queuing two
  would let the first erase the pending marker and briefly report the entity
  live again.

### Known sharp edge: component access on an unresolvable handle

`ScriptGlue.cpp`'s shared `GetEntity(UUID)` helper — which every one of the ~740
generated `OLO_PROPERTY` bindings calls — resolves through
`Scene::GetEntityByUUID`, which **asserts** in Debug and reads through a
`FindChecked` in Release when the UUID is unknown. So `e.Translation` on a handle
whose spawn has not drained yet (or one another script already destroyed) is not
a clean no-op. This is pre-existing — any stale UUID has always behaved this way
— but deferred spawning makes it easier to reach, which is why both languages'
API docs state plainly that a spawned entity's components are not readable until
the next tick.

Two entry points were made tolerant because a script would reasonably use them to
guard: `Entity_HasComponent` (hence C#'s `GetComponent<T>()`, which returns null)
and Lua's `entity_utils.get_component` / `has_component`, all of which resolve via
`TryGetEntityWithUUID`. Hardening `GetEntity` itself would need the generated
bindings to null-check, which is a separate change.

## Other invariants that bit during implementation

- **Re-entrancy.** A spawned entity's `OnCreate` may queue more commands. The
  drain runs *rounds* (swap the queue, apply the batch, repeat) with a
  re-entrancy flag so a nested `Flush` is a no-op, and a round cap
  (`kMaxEntityCommandDrainRounds`) so a script that unconditionally spawns from
  `OnCreate` cannot hang the frame. The cap is **per `Flush` invocation, not per
  tick** — `UpdateScripts` flushes twice (before and after the script loops), so
  a runaway spawner gets up to two round-budgets per tick. Still bounded, still
  making progress, leftovers deferred to the next flush. The re-entrancy flag is
  read and written under the same mutex as the queue, so the two cannot disagree
  about whether a drain is in progress.
- **Order within a batch is request order.** A spawn-then-destroy of the same
  entity in one tick therefore runs `OnCreate` on a live entity and then
  `OnDestroy` on it — never `OnCreate` on something already gone.
- **`Scene::DestroyEntity` does not recurse into children** (correct for the
  editor's delete-one-node semantics, wrong for gameplay). The script destroy
  routes through `Scene::DestroyEntityAndChildren`, which destroys the subtree
  children-first and unlinks the root from its parent's child list. Without
  that, every script-driven spawn/despawn loop leaks one entity per prefab child
  per cycle — invisible until a churn test counts live entities.
- **`OnCreate` must be fired by the drain.** The `OnRuntimeStart` sweep only
  covers entities that existed when the session started, so a runtime-spawned
  scripted entity would otherwise receive `OnUpdate` having never received
  `OnCreate` (and for Lua, never at all — the engine only registers a script
  instance on a successful `OnCreate`). Fire it for the whole spawned subtree:
  prefab children carry scripts too.
- **`Project::GetAssetFileSystemPath` asserts without an active project.** The
  Lua `OnCreate` path must fall back to using `LuaScriptComponent::ScriptFile`
  verbatim when no project is mounted, or every headless harness that spawns a
  Lua-scripted entity asserts.
- **Clear the queue at `OnRuntimeStart` and `OnRuntimeStop`.** Commands queued in
  the last tick of a session name entities in a scene that no longer exists.

## If you ever mark the `Scripts` node `.Parallelizable()`

The drains **must move out of the system body** to a barrier node first. The
queue's *request* side is mutex-guarded and already worker-safe; the *apply*
side performs EnTT structural changes and is game-thread-only. Applying it from
a worker is precisely the corruption the queue exists to prevent — see the
"EnTT + worker threads: first-touch is a WRITE" note in `CLAUDE.md`, which
covers the adjacent hazard of lazy pool creation.

## The Lua u64 trap this uncovered (`SOL_ALL_INTEGER_VALUES_FIT`)

Entity UUIDs and AssetHandles are full-range `u64`. **Lua 5.4's `lua_Integer` is
SIGNED 64-bit**, so roughly half of all randomly generated ids sit above
`LUA_MAXINTEGER`. That breaks the sol2 boundary in two different ways, and both
bit during #643:

1. **Pushing** a `u64 > INT64_MAX` under `SOL_ALL_SAFETIES_ON` throws
   *"integer value will be misrepresented in lua"*. Every Lua binding that
   returns a real runtime UUID — `entity_utils.find_by_name`, the spawn calls,
   any `sol::property` exposing an entity reference — threw about half the time.
   This was **pre-existing and latent**: the sandbox scenes use small
   hand-authored UUIDs (`1000000000000000001`), so it only shows up against
   `UUID()`-generated ids. Fixed centrally by
   `target_compile_definitions(OloEngine PUBLIC SOL_ALL_INTEGER_VALUES_FIT=1)`,
   which lets sol2 bit-cast between `u64` and `lua_Integer` instead of
   range-checking. The round trip is lossless — sol2's *get*-side integer check
   only verifies `lua_isinteger` and does no signedness check, so the bits come
   back intact. PUBLIC and set in CMake rather than per-TU for the same ODR
   reason as `ENTT_USE_ATOMIC`: it changes inline push/check helpers.

2. **A large id written as a decimal literal in Lua source** is parsed by Lua's
   *lexer* as a FLOAT, and then fails sol2's "fits exactly an integer" check on
   the way into a `u64` parameter. No compile flag can fix this — it happens
   before sol2 is involved. Real scripts are unaffected (they receive handles as
   `lua_Integer` from a binding or component field); it is specifically
   hard-coding a big id in Lua text, which is exactly what a C++ test that
   splices a handle into a script string does. Emit such ids as **signed**
   decimals (`std::to_string(static_cast<i64>(handle))`) — same bit pattern,
   always lexes as an integer. See `LuaIdLiteral` in the Functional test.

The visible cost of (1) is that a Lua script printing an id with the top bit set
sees a negative number. Identity, equality and table-key use are unaffected.

## Where the contract is pinned

- `OloEngine/tests/Functional/Scripting/LuaSpawnsAndDestroysEntitiesTest.cpp` —
  deferred-within-OnUpdate / applied-within-tick, UUID stability, prefab
  hierarchy + transform override, `OnCreate` firing for a spawned scripted
  prefab, self-destroy, hierarchical destroy, and 30 ticks of spawn/destroy
  churn asserting an exact live-entity count under **both** the parallel and the
  sequential (`OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL=1`) executor.
- `OloEngine/tests/Lua/LuaBindingTest.cpp` — binding existence and the
  queue-level semantics (deferral, idempotent destroy, non-finite transform
  rejection, null prefab handle).
