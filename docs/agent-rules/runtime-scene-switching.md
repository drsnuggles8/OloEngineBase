# Runtime scene load/switch — host-serviced transitions (issue #642)

How a script-requested scene change gets from `SceneManager.LoadScene("Level2")` /
`Scene.LoadScene("Level2")` to a running scene, and the ordering rules that make it
safe. The same path supports `LoadSceneFromSave("Level2", "slot")`, which pairs an
optional restore request with the scene request. Read before touching
`Scene::SetPendingSceneLoad`, `SceneTransition`,
`RuntimeLayer::ActivateScene`, or `EditorLayer::SwitchPlayScene`.

The shape: **Scene records the request, the HOST applies it.** `Scene` deliberately owns
no loading code — it only records the requested scene and optional save slot. The two
hosts (OloRuntime's `RuntimeLayer`, the editor's Play branch) service them after the tick
returns, both through the shared `SceneTransition::{ResolveScenePath, LoadSceneFile}`.

---

## 1. The request cannot be honoured where it is made

`Scene::UpdateScripts` dispatches `OnUpdate` **while iterating the script component
pools**. A scene swap destroys that registry. So a switch applied inline is a
use-after-free that surfaces later, somewhere unrelated — the same hazard
`Scene::SetPendingReload` has always existed to avoid, and the same reason script
spawn/destroy is queued (see
[script-structural-command-safe-point.md](script-structural-command-safe-point.md)).

`SetPendingSceneLoad` generalizes `SetPendingReload` from "reload `m_ScenePath`" to "load
an arbitrary path" rather than inventing a second mechanism.

## 2. Reload and load must be mutually exclusive

They are one mechanism with two targets. If a tick can leave *both* set, the host switches
scenes and then immediately reloads the scene it just switched into — which looks like the
switch "didn't take" and is maddening to debug from the symptom. `Scene`'s two setters
clear each other, so **the last request of a tick is the one that happens**, and the host's
`if (HasPendingSceneLoad()) … else if (GetPendingReload())` is a check order, not a
precedence rule.

## 3. Load and validate BEFORE tearing the outgoing scene down

The pre-#642 reload did `OnRuntimeStop()` → `Ref<Scene>::Create()` → `Deserialize()` and,
on failure, `Application::Close()`. That is only survivable because a reload targets a path
already known to load. An arbitrary path from script is a *content* input: a typo must not
be able to kill the game.

`SceneTransition::LoadSceneFile` therefore deserializes and validates into a **fresh**
Scene and hands back a `LoadResult`; only once it succeeds does the host stop the current
scene. A failed switch logs why and leaves the game running exactly where it was. That
policy is pinned by `RuntimeSceneSwitchTest.AnUnknownSceneLeavesTheCurrentSceneRunning`.

The validation includes **"has a primary camera"**: the runtime has no editor camera to
fall back on, so accepting a camera-less scene means switching the player into a black
screen. Refusing is louder and recoverable.

## 4. Stop-before-start, because the script context is process-wide

`ScriptEngine` (C#) and `LuaScriptEngine` share **one** global scene-context pointer
(`s_LuaData.SceneContext`, `ScriptEngine::GetSceneContext()`). Starting the incoming scene
before stopping the outgoing one has the new scene claim the context and the old scene's
`OnRuntimeStop` then clear it — leaving the *live* scene with no script context, i.e. every
script in every level after the first silently dead.

So the order in both hosts is fixed: **deserialize new → `old->OnRuntimeStop()` → swap →
`new->OnRuntimeStart()`**. `LuaScriptEngine::OnRuntimeStop` also dereferences the scene it
is bound to (to release GOAP agents), so the outgoing scene must still be alive when it
runs — which is why the Functional test's `TearDown` calls `FunctionalTest::TearDown()`
*before* releasing its own scene ref.

Corollary for tests: a headless harness can't call `OnRuntimeStart` (it needs
`Application::Get()`), so a host-emulating test must redo its script sweep by hand —
`SetRunning(true)` plus `LuaScriptEngine::OnCreateEntity` for every `LuaScriptComponent`.
Forgetting that sweep makes the incoming scene's scripts look dead in the test for a reason
that has nothing to do with the code under test.

For a Continue transition, restore the save into the successfully deserialized incoming
scene **before** stopping the outgoing scene and before `OnRuntimeStart`. A save restore
replaces component state; applying it after startup would invalidate physics, script, and
audio handles that were created from the authored registry. If restore fails, discard the
incoming scene and leave the outgoing scene running, just like a deserialize failure. The
save slot is stored beside the scene request and cleared with it so it cannot leak into a
later ordinary transition.

## 5. `OnRuntimeStart` must clear the pending request

A request that survives into the scene it caused is an infinite bounce: tick 0 of the new
scene re-fires the switch, forever, at frame rate. `Scene::OnRuntimeStart` clears both
`m_PendingReload`, `m_PendingSceneLoad`, and the paired save slot next to its existing
`ClearPendingEntityCommands()` for exactly the same reason that call is there. Pinned by
`RuntimeSceneSwitchTest.AServicedRequestDoesNotRefireOnTheNewScene`.

---

## Editor-specific: a Play-mode switch must not touch the editor scene

Play mode is a sandbox over a `Scene::Copy` of `m_EditorScene`. A script switching scenes
inside Play must therefore replace **only `m_ActiveScene`** — never `m_EditorScene`, and
never route through `OpenScene()`, which would discard the user's unsaved edits as a side
effect of a *script* call. `Stop` then returns the user to what they were working on, and
the title bar keeps showing the authored scene throughout. Verified live: switch A→B in
Play, then Stop, and the hierarchy is back to A's entities.

Every editor panel holds a raw `Scene*`/entity handles, so **every** site that swaps the
active scene has to rebind all of them or a panel is left pointing into a destroyed
registry. There are now four such sites (`OnScenePlay`, `OnSceneSimulate`, `OnSceneStop`,
`SwitchPlayScene`), which is why they funnel through `EditorLayer::BindPanelsToScene`
instead of repeating the list.

## Path resolution is part of the contract

A designer types `LoadScene("Level2")`, not a path. `SceneTransition::ResolveScenePath`
probes `<root>/<name>`, `<root>/Scenes/<name>`, the request as given, and finally a sorted
recursive search of `Scenes/` — each with and without an appended `.olo`. Two consequences
worth knowing:

- The **root differs per host**: the game's working directory for OloRuntime,
  `Project::GetAssetDirectory()` for the editor. `GameBuildPipeline` copies scenes to
  `<game>/Scenes/<authored-relative-path>`, so the nested-`Scenes/Scenes/…` layout a build
  produces is why candidate 2 exists.
- `..` components are **rejected**. Scene names come from script source and there is no
  legitimate reason for one to reach outside the game's data directory.

`SceneSerializer::Deserialize` names the loaded scene after its **file name including the
extension** (`"Level2.olo"`, not `"Level2"`) — long-standing behaviour, and a trap when
asserting on `Scene::GetName()` after a load.

---

## A shipped game needs an active `Project`, and two halves must agree on the layout

Found while verifying the Lua half of #642, fixed in the same change: `OloRuntime` used to
run with **no active `Project`** — it only called `Project::SetAssetManager`. But
`Scene::OnRuntimeStart` resolves every `LuaScriptComponent::ScriptFile` through
`Project::GetAssetFileSystemPath`, which asserts on the null `s_ActiveProject`. A shipped
game therefore died the instant it loaded a scene carrying a Lua script — Lua scripting was
effectively **editor-only**, which quietly made the Lua `Scene.LoadScene` binding useless in
exactly the builds it exists for. (C# was never affected: `ScriptEngine` touches `Project`
nowhere.)

A built game has no `.oloproj` — `GameBuildPipeline` flattens the project into
`game.manifest` plus an asset pack — so the fix is `Project::NewInMemory(directory, config)`,
called from `RuntimeLayer::MountGameProject` **before anything loads**, rooted at the game's
working directory with `AssetDirectory = "Assets"`.

That only works if **two independent halves agree on one layout**, and a mismatch between
them is silent — no crash, no error, the script just never loads:

- `GameBuildPipeline::CopyScriptFiles` writes each `.lua` to
  `<game>/Assets/<asset-relative path>` (note: under `Assets/`, *not* a top-level sibling
  like the scene copy's `Scenes/` — because the runtime resolves scripts through the asset
  root while it resolves scenes against the working directory).
- `MountGameProject` mounts `AssetDirectory = "Assets"` so
  `GetAssetFileSystemPath("Scripts/LuaScripts/Foo.lua")` lands exactly there.

`RuntimeProjectMountTest` pins that round trip against a staged directory laid out the way
the pipeline lays one out. If you change either half, change both and check that test.

Generalisable rule: **any engine path resolved through `Project::` is a build-pipeline
obligation.** `LuaScriptEngine::OnCreateEntity` uses `lua_State::load_file` — a plain
filesystem read, not an asset-pack lookup — so its files must be shipped loose. Before
assuming a subsystem works in a shipped game, check whether it reads through
`AssetManager` (packed, shipped) or through `Project::GetAssetFileSystemPath` + the
filesystem (loose, needs an explicit copy step). Adjacent still-unverified case:
`Scene::InitAudioRuntime` reads `GetAssetDirectory()/audio/AudioEvents.yaml`, which no build
step copies.

`Scene::OnRuntimeStart`'s Lua sweep now also guards with
`Project::GetActive() ? GetAssetFileSystemPath(...) : raw`, matching what
`FireSpawnScriptLifecycle` already did, so a headless harness that sets an absolute
`ScriptFile` keeps working without a project.
