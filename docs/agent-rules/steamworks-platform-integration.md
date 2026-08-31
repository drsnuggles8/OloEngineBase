# Steamworks platform integration

Read before touching anything under `OloEngine/src/Platform/Steam/`, the `OLO_WITH_STEAM` CMake
plumbing, or the Steam Lua bindings.

The governing fact, from which almost everything else follows:

> **The Steamworks SDK cannot exist in this repository, and cannot reach CI.** It downloads from
> `partner.steamgames.com` behind a Valve login, its licence forbids redistribution, and
> `drsnuggles8/OloEngineBase` is **public**.

That single constraint is why the code is shaped the way it is. Every rule below is a consequence
of it, or a trap found while building it (#644).

---

## 1. It is a developer-installed SDK, not a vendored one — Vulkan is the precedent, not Mono

The obvious move is to copy the **Mono** block in `OloEngine/CMakeLists.txt` — the `if(MSVC)`
branch that links `libmono-static-sgen` and defines `OLO_ENABLE_CSHARP_SCRIPTING`: a prebuilt
proprietary-ish library, committed in-tree, platform-gated. **That is wrong here.** Mono may live
in-tree *because Mono is redistributable*. Steamworks is not, and the repo is public.

The right precedent is the **Vulkan SDK**: resolved from an environment variable, installed by the
developer, never in the tree.

```
STEAMWORKS_SDK_ROOT  →  the inner `sdk` directory, the one DIRECTLY containing
                        public/ and redistributable_bin/
```

Mono remains the precedent for exactly one line — defining the macro **PUBLIC in both branches**
of the `if`, so every TU agrees on its value:

```cmake
if(OLO_WITH_STEAM)
    target_compile_definitions(OloEngine PUBLIC OLO_WITH_STEAM=1)
else()
    target_compile_definitions(OloEngine PUBLIC OLO_WITH_STEAM=0)   # ← the half people forget
endif()
```

`vcpkg.json` does **not** change, and an overlay port in `cmake/overlay-ports/` is not an option
either: it would need an authenticated download URL and has no upstream port to stay a minimal
diff against, which is the explicit rule in
[vcpkg-dependency-management.md](vcpkg-dependency-management.md).

---

## 2. Auto-detecting the SDK has a silent-failure mode, and it is the likeliest user error

`OLO_WITH_STEAM` defaults ON when `STEAMWORKS_SDK_ROOT` resolves to a real SDK. The naive probe:

```cmake
if(DEFINED ENV{STEAMWORKS_SDK_ROOT} AND IS_DIRECTORY "$ENV{STEAMWORKS_SDK_ROOT}/public/steam")
    set(OLO_STEAM_SDK_DEFAULT ON)
endif()
```

...is **wrong in the exact case that matters**. The SDK zip extracts to an outer folder containing
an inner `sdk/`, so a developer naturally points the variable at the outer one. The probe then
finds no `public/steam`, defaults to OFF, and **the build succeeds with Steam silently disabled**.
Worse, the carefully-worded "must point at the directory directly containing public/" error never
fires, because it only runs when the option is *explicitly* forced ON — so the one person who made
the mistake is the one person guaranteed not to see the explanation.

The fix, in the root `CMakeLists.txt`: a variable that is *set but invalid* must **warn loudly**,
and the one-level-too-high case is detected specifically and answered with the corrected path in
native separators, ready to paste:

```
STEAMWORKS_SDK_ROOT is set one level too high, so Steam support is OFF.
  current: D:\SDKs\steamworks_sdk_165
  correct: D:\SDKs\steamworks_sdk_165\sdk
```

An **unset** variable stays silent — that is CI and every contributor who does not care about
Steam, and they must not be warned about a feature they never asked for.

**Generalisable:** whenever an option auto-detects from the environment, "configured but wrong"
and "not configured" need *different* outcomes. Collapsing them into one silent default is how a
feature ends up mysteriously absent.

---

## 3. Every executable needs `steam_api64.dll` — including the ones that never call Steam

`steam_api64.lib` is an **import library** for a DLL, not a static library. Two consequences, and
the second one costs a debugging session:

**The good one:** an import stub carries no CRT of its own, so it links cleanly against this
project's `x64-windows-static-md` triplet. The CRT-triplet mismatch that
[vcpkg-dependency-management.md](vcpkg-dependency-management.md) calls out as silent heap
corruption **does not apply here**. Don't go hunting for it.

**The trap:** because `OloEngine` links that import library, *every* executable linking the engine
carries a static import on `steam_api64.dll`, and **Windows resolves static imports at process
load, before a single line of our code runs.** So `OloServer` — which explicitly skips Steam, being
headless — and `OloEngine-Tests` both fail to start without the DLL beside them. The symptom is a
bare "the code execution cannot proceed because steam_api64.dll was not found" dialog with **no log
line at all**, because the process never reached `main`. It looks nothing like its cause.

Hence `olo_copy_steam_runtime()` (`cmake/CommonProperties.cmake`) is called from **all four**
executable scopes: `OloEditor`, `OloRuntime`, `OloServer`, `OloEngine-Tests`. Same directory-scope
rule as `olo_copy_ffmpeg_runtime` — `add_custom_command(TARGET ...)` must run where the target was
created.

---

## 4. Exactly one TU may include a Valve header

`Platform/Steam/SteamworksBackend.cpp` is the only file in the engine that may
`#include "steam/..."`. Everything else talks to `ISteamBackend` (`Platform/Steam/SteamBackend.h`).

This is not tidiness. Three things depend on it:

- **The logic stays fake-testable.** The dedup, the validation, the cloud short-circuit are driven
  through `FakeSteamBackend` with no Steam client, no App ID and no Valve account — on any machine,
  including a CI runner that has never had Steam installed.
- **The stub SDK stays a sufficient stand-in.** The hand-written stubs only have to satisfy *one*
  file. If Valve includes spread, the stub surface grows without bound and the CI job dies.
- **`OLO_WITH_STEAM=0` changes nothing about the link surface**, because no other header ever saw
  a Valve type.

Enforced by a grep in `.github/workflows/steam-stub.yml` (`single-valve-tu` job), because a comment
would not have held.

**The obvious grep for that guard is wrong**, and it is wrong because of the GNS confusion #644
warns about. **GameNetworkingSockets also ships its headers under a `steam/` prefix** — the netcode
includes `<steam/steamnetworkingsockets.h>` and `<steam/isteamnetworkingutils.h>`. So a plain
`#include ["<]steam/` search flags every networking file and the guard is red on its first run,
which is worse than no guard: someone will "fix" it by deleting it.

Because the Steamworks SDK is included as `public/steam/…` (see the hijack section below), the
guard keys on **that** prefix instead — which sidesteps the GNS clash entirely — and then adds a
second check that the Steam TUs never regress to a bare `steam/…`:

```bash
# 1. nobody outside the one TU includes the SDK
grep -rlE '^[[:space:]]*#include[[:space:]]*[<"]public/steam/' … | grep -v <the two allowed files>
# 2. the allowed files never use a bare steam/ form
grep -qE '^[[:space:]]*#include[[:space:]]*[<"]steam/' SteamworksBackend.cpp SteamStubSDK.cpp
```

Two details that cost a round each:

- **Anchor to `^\s*#include`.** An unanchored match also fires on *prose*: these files explain the
  rule in comments that quote the very includes being banned, so the first version of the guard
  failed on its own documentation.
- **Test the mixed case.** A file containing *both* a GNS include and a Steamworks one must fail;
  checking only "clean tree / planted Steamworks / planted GNS" lets it through.

Remember GNS is a *standalone BSD-licensed transport library*: no Steam client, no App ID, no Valve
account, and no **link-time** symbol overlap. `SteamNetworkingSockets()` is not `SteamAPI_Init`, and
GNS's own `RunCallbacks()` on its interface is not `SteamAPI_RunCallbacks`. **Do not touch anything
under `Networking/` while working on Steamworks.**

### …but the SDK will HIJACK GNS's headers engine-wide if you put it on the include path

This is the single worst trap in the whole feature, and the include line that avoids it looks
like a typo. **"Zero symbol overlap" is true of the linker and false of the preprocessor.**

The two SDKs share **file names**. Valve's Steamworks SDK ships its own:

```
isteamnetworkingutils.h   isteamnetworkingsockets.h   steamnetworkingtypes.h
steam_api_common.h        steamtypes.h
```

The engine's netcode includes exactly two headers:

```c
#include <steam/steamnetworkingsockets.h>   // Valve does NOT ship this → resolves to GNS
#include <steam/isteamnetworkingutils.h>    // Valve DOES ship this   → HIJACKED
```

So the moment a directory containing a `steam/` child lands on the engine include path — which is
exactly what `target_include_directories(OloEngine … "${SDK}/public")` does — every Networking TU
gets **half of each SDK**, and the build dies with:

```
steam_api_internal.h(255,8): error C2365: 'k_iSteamNetworkingSocketsCallbacks':
    redefinition; previous definition was 'enumerator'
```

Twelve of them, reported *inside a Steamworks header*, while compiling *Networking* files that
never mention Steam. Nothing in the message names the include path, the Steam change, or GNS.

**The fix is to root the include directory one level higher and prefix the include:**

```cmake
set(OLO_STEAM_INCLUDE_DIR "${OLO_STEAM_SDK_ROOT}")   # …/sdk   — NOT …/sdk/public
```
```c
#include "public/steam/steam_api.h"                 // NOT "steam/steam_api.h"
```

Now nothing on the include path has a `steam/` child, so GNS's headers cannot be shadowed by
construction. The stub SDK mirrors the same `public/steam/` layout for the same reason. Both are
enforced by the `single-valve-tu` CI job, which checks *both* that no other file includes
`public/steam/…` **and** that the Steam TUs never use a bare `steam/…` — because reverting to the
bare form is what would force the include dir back to `public/` and bring the hijack straight back.

**A wrong diagnosis to skip:** the first instinct is unity (jumbo) builds concatenating
`SteamworksBackend.cpp` with a Networking TU. Plausible — the errors are interleaved with
Networking output — but wrong here: `OLO_ENABLE_UNITY_BUILD` is **OFF** in this build, and MSBuild
simply interleaves the output of parallel compiles. Check `OLO_ENABLE_UNITY_BUILD` in
`CMakeCache.txt` before adding a `SKIP_UNITY_BUILD_INCLUSION` entry that does nothing.

**Nothing automated catches the underlying breakage**, because CI can never build the Steam path
at all — which is why the guard checks the *include form* rather than waiting for a compile error.

---

## 5. Two degradation variants, and they are not interchangeable

Both are required; they cover genuinely different failures.

| | Trigger | Mechanism |
|---|---|---|
| **Variant B — compile time** | `OLO_WITH_STEAM=0`, no SDK present | `CreateSteamBackend()` returns a `NullSteamBackend`. Header and link surface identical either way. |
| **Variant A — run time** | `OLO_WITH_STEAM=1`, `SteamAPI_InitEx()` fails | Warn with an actionable sentence, set the availability flag false, **return**. |

The precedent for both is `Scripting/C#/ScriptEngine.cpp`: `ScriptEngine::Init` for the runtime
form (it warns with a remediation sentence and returns when `LoadAssembly` fails), and the
`#else !OLO_ENABLE_CSHARP_SCRIPTING` half of the same file for the complete no-op stub set.

**The counter-example to avoid is in the same file you are editing.** In the `Application`
constructor, the `AudioEngine::Init()` and `NetworkManager::Init()` failure branches both
`throw std::runtime_error` when they fail
to initialise. **Steam must never join that club.** "No Steam client" is an ordinary state — a
developer machine, a CI runner, a player who launched the exe directly rather than through Steam.
A missing Steam client stopping the engine from starting would be a catastrophic regression, and
it is the single behaviour most worth protecting with a test.

There is no second stub class: `NullSteamBackend` *is* the complete no-op set, and duplicating it
would give the OFF path a second thing to drift from.

---

## 6. Two fakes at two levels, and they are not redundant

This confuses people, so name it in review:

- **`FakeSteamBackend`** (`tests/Platform/Steam/`) stands in for the *backend*, so the **engine's
  logic** can be driven directly. Never sees a Valve type. Runs everywhere, always.
- **The stub SDK** (`src/Platform/Steam/StubSDK/`) stands in for *Valve's headers*, so the **one
  Valve-calling TU** compiles, links and runs in CI. Never sees our logic.

They test different things. Removing either leaves a real hole.

The stub SDK deliberately **links** rather than merely compiling. That is what upgrades the ON path
from "compiles in CI" to "tested in CI", and it is what lets
`SteamStubOnPathTest.FailedSteamInitDoesNotThrowAndLeavesSteamUnavailable` run on a machine that
has never had Steam — verifying §5's contract on every push rather than once, by hand, by one
person.

**If the stub job fails because a Steamworks call is missing**, add the declaration to
`StubSDK/public/steam/steam_api.h` and the canned implementation to `StubSDK/SteamStubSDK.cpp`. That is
the mechanism working. Do not delete the job.

### Reproducing the stub build locally on Windows — do NOT copy the CI flags

The CI job configures with `-DOLO_WITH_USD=OFF -DOLO_WITH_ALEMBIC=OFF -DOLO_WITH_MATERIALX=OFF`
to keep the runner cheap. That flag set is verified on **`x64-linux` only**, because that is the
only place the job runs.

Copying it into a local Windows configure produces a build that compiles fine and then dies at
link with **46 unresolved `google::protobuf` externals referenced from
`GameNetworkingSockets_s.lib`** — `__declspec(dllimport)` symbols, i.e. GNS expecting a shared
protobuf against a static one. Turning those subsystems off changes the vcpkg feature set, and on
`x64-windows-static-md` the protobuf that GNS ends up linking no longer matches.

Nothing to do with Steam: **zero** of the unresolved symbols are Steamworks API. Do not go hunting
in the stub for them.

**Reproducing it on Windows is UNSOLVED — do not assume the recipe below works.** Turning the
subsystems back on was tried and the link failed identically (87 unresolved instead of 46), so the
feature set is not the whole story. The same protobuf/GNS mismatch is present in the normal
`build/` tree and is simply masked there; nothing in the diagnosis pointed at Steam. Since CI
builds, links and runs this path on Linux, chasing the Windows link further was judged not worth
it — but do not read the snippet below as verified.

If you try again, start from a **fresh build directory** (CMake caches `OLO_WITH_*`, so re-running
with different flags in an existing tree mixes states and the vcpkg feature set does not fully
re-resolve), set the subsystem options **explicitly** rather than trusting defaults, and unset
`STEAMWORKS_SDK_ROOT` **in the environment** — removing the CMake cache entry is not enough,
because the option auto-detects from `$ENV{...}` on every configure:

```powershell
Remove-Item Env:\STEAMWORKS_SDK_ROOT -ErrorAction SilentlyContinue   # environment, not cache
Remove-Item -Recurse -Force build-steamstub -ErrorAction SilentlyContinue
cmake -S . -B build-steamstub `
  -DOLO_WITH_STEAM_STUB_SDK=ON `
  -DOLO_WITH_USD=ON -DOLO_WITH_ALEMBIC=ON -DOLO_WITH_MATERIALX=ON `
  -DOLO_FFMPEG_PREFIX="<repo>/OloEngine/vendor/ffmpeg-install"
```

Unsetting the environment variable is not incidental: it is what proves the stub path needs no SDK
at all, which is the property the whole stub exists to demonstrate.

**The cheap alternative that does work**: compile-check the stub TUs directly, which catches
everything except link and runtime:

```powershell
cl /Zs /std:c++latest /EHsc /permissive- /utf-8 /W4 `
   /DOLO_WITH_STEAM=1 /DOLO_WITH_STEAM_STUB_SDK=1 <engine include dirs> <StubSDK dir> `
   OloEngine\src\Platform\Steam\StubSDK\SteamStubSDK.cpp `
   OloEngine\src\Platform\Steam\SteamworksBackend.cpp
```

---

## 7. Steamworks API details that are easy to get wrong

- **`SteamAPI_InitEx`, not `SteamAPI_Init`.** The Ex form fills a `SteamErrMsg` with a
  human-readable reason and returns a discriminated `ESteamAPIInitResult`, so "Steam isn't running"
  and "your client is out of date" get different remediation sentences. `SteamAPI_Init()` is just
  the Ex call with the message thrown away.
- **The overlay toast fires on `StoreStats()`, not `SetAchievement()`.** Set without store records
  the achievement but shows the player nothing, which reads as "achievements are broken".
- **...but store is a network round-trip, so dedup first.** A game unlocking from an `OnUpdate`
  would otherwise issue one store per frame forever. `UnlockAchievement` queries first and returns
  `AlreadySet` without storing. `AlreadySet` is distinct from `Success` (so an unlock sting can
  tell them apart) but both satisfy `SteamSucceeded()`.
- **`GetAchievement()` returning false means "Steam has never heard of this id"** — a typo, or an
  achievement not defined on the partner site. It is silent in-game, so the engine logs it loudly.
- **`IsOverlayEnabled()` is not "is the overlay showing".** It answers whether the overlay is
  *available*; using it for display state reports the overlay active for the entire session. The
  displayed state comes from the `GameOverlayActivated_t` callback.
- **Callback registration must happen after a successful init.** A `CCallback` registering in a
  constructor runs before `SteamAPI_Init` and is invalid — hence `CCallbackManual` plus an explicit
  `Register()` in `Initialize()` and `Unregister()` in `Shutdown()`.
- **Cloud has TWO switches**: `IsCloudEnabledForAccount()` *and* `IsCloudEnabledForApp()`. A game
  checking only the app-level one silently loses saves for every player who turned Cloud off
  account-wide.
- **A zero-length cloud file is legal.** Returning it as an empty buffer hands the caller something
  that looks like a valid save; report it as `NotFound` instead.

---

## 8. Init/Shutdown pairs across TWO shutdown sites

`Core/Application.cpp` tears down in two different places:

1. the **exception path** in the constructor's `catch (...)`, and
2. the **normal destructor path**.

Missing either leaks the Steam session. `SteamManager::Shutdown()` is called **unconditionally**
from both — even though `Initialize()` is guarded on `!IsHeadless` — precisely so the two guards
cannot drift apart into a leak. Shutdown is a documented no-op when init never ran.

The per-frame `SteamAPI_RunCallbacks` pump belongs in `Run()`, **after** the input/platform block
and **before** `ProcessTasks`, so a Steam callback that enqueues a game-thread task drains the same
frame. It must **not** go in `RenderFrameLayers` — that function has a re-entrancy latch and the
nested-swap path would pump callbacks twice per frame.

---

## 9. It is a process-level service, not an ECS component

`SteamManager` is a static manager, matching `NetworkManager` / `SaveGameManager` /
`InputActionManager`. Achievements belong to the process and the signed-in user, not to an entity.

That choice is load-bearing for cost: it keeps the feature entirely clear of the hand-maintained
cross-binding touch-point set in [CLAUDE.md](../../CLAUDE.md) — no `AllComponents` tuple entry, no
`SceneSerializer` block, no SaveGame capture/restore, no `OnComponentAdded` specialization. Do not
"promote" any of this to a component without reading that section first.

---

## 10. What can and cannot be verified, and how to say so

Verification here is **three-tiered**, which is unusual for this repo. State the tiers explicitly in
any PR touching Steam:

| Tier | Covers | Where |
|---|---|---|
| **CI, every push** | The OFF path, plus the ON path via the stub SDK — compiles, links, tests run | `steam-stub.yml` + the normal suite |
| **One machine only** | The real SDK ON build, and live behaviour against App ID 480 | A developer holding the SDK |
| **Not verified at all** | Vulkan overlay behaviour | — |

For tier 2: put `480` (Valve's public *Spacewar* test app, owned by every Steam account) in
`OloEditor/steam_appid.txt`, run the Steam client, and drive the real thing. Its achievement ids are
public on SteamDB. That file is git-ignored and **must be removed for a release build** — shipped
games take their App ID from the client, and leaving it behind is how a build reports itself as
Spacewar.

Do **not** report Steam features as working on the strength of the OFF-path or stub tests. The stub
proves the code compiles, links and its branches behave; it proves nothing about Valve's servers.

This repo's own rule — *"whenever the dev box happens to match the value CI varies, local green
proves nothing"* — bites unusually hard here, because with the real SDK the dev box is the **only**
box that can be green. The stub job exists so that is not the whole story.

---

## 11. Steam Input on `ISteamBackend`, and the decision on `InputActionManager`

Added for #893 (Deck Verified needs controller remapping through Steam's own configurator, and
Steam Input is the layer through which most Deck and many desktop players will actually bind
their controls).

### The surface

`ISteamBackend` gained action sets, digital/analog action state and glyph/origin lookup —
`SteamTypes.h` (`SteamInputHandle`, `SteamInputActionSetHandle`, `SteamInputDigitalActionHandle`,
`SteamInputAnalogActionHandle`, `SteamInputDigitalActionState`, `SteamInputAnalogActionState`),
implemented for real against `ISteamInput` in `SteamworksBackend.cpp`, mirrored in the stub SDK
(`StubSDK/public/steam/steam_api.h` + `SteamStubSDK.cpp`), and wrapped on `SteamManager`. Unlike
the rest of the interface, Steam Input needs its own init/shutdown/pump — `SteamManager::
Initialize()` calls `InputInit()` right after the base `Initialize()` succeeds (logging and
continuing, never failing, if it doesn't), `RunCallbacks()` calls `InputRunFrame()` alongside
`SteamAPI_RunCallbacks()`, and `Shutdown()` tears input down before the base backend. So callers
never call an `Input*` lifecycle method directly — only `SteamManager::IsInputAvailable()` and the
query methods.

The stub SDK does not model an action manifest (there is no manifest in a CI checkout — a game's
manifest is authored content, uploaded to the Steamworks partner site, not engine code). Any
action-set or action **name** the caller passes gets a stable handle assigned on first sight,
matching how the real SDK behaves for a name that genuinely exists in the manifest. This means the
stub tests prove the *plumbing* (handles round-trip, action-set activation reaches the SDK,
digital/analog state and glyph lookup pass through), not that a specific action name is correctly
declared in a manifest — that part is only checkable against the real SDK with a real manifest
file, which is tier 2 in §10.

### The decision: Steam Input wins when available, per gamepad-origin binding

**`InputActionManager` decides per-frame, not per-action-map.** When `SteamManager::
IsInputAvailable()` is true and at least one controller is connected (`GetConnectedControllers()`
non-empty), `InputActionManager::Update()`:

1. Activates the Steam Input action set whose **name equals `InputContextTypeToString` of the
   active context** (`"Gameplay"`, `"Menu"`, `"Vehicle"`, `"Custom"`) on every connected
   controller. This is the load-bearing naming convention: a game's Steam Input manifest must
   define an action set per `InputContextType` the game uses, named exactly that, or Steam Input
   silently has nothing bound for that context (see the non-goal risk below).
2. For every action in the active map, **skips `GamepadButton`/`GamepadAxis` bindings entirely**
   and instead queries the Steam digital/analog action **whose name equals the `InputAction::Name`**
   (`GetDigitalActionState`/`GetAnalogActionState`, handles cached for the process after first
   lookup — see `ApplySteamInputForAction` in `InputActionManager.cpp`). `Active` on the returned
   state means "Steam has an origin bound to this action in the current set"; when false, the
   Steam contribution is skipped entirely for that action this frame, not treated as "not
   pressed" — so an action Steam doesn't recognise falls all the way back to whatever the engine
   bindings would have said, which is nothing, since gamepad bindings are already skipped. In
   practice that means: an action absent from the manifest is simply unreachable via a connected
   Steam Input controller, by design — add it to the manifest, don't add a special case here.
3. `Keyboard`/`Mouse` bindings are **never affected** — they run exactly as before, both with and
   without a Steam Input controller connected, so keyboard-and-mouse play is identical either way
   and a menu/UI flow that expects Enter/Escape to always work keeps working.

The result: **Steam Input governs the physical controller whenever it's present; the engine's own
gamepad bindings are the fallback for a controller Steam Input isn't driving** (no Steam client,
Steam Input failed to init, or nothing connected through it). Keyboard and mouse are orthogonal to
both and always live. This was chosen over the alternative (OR-ing engine gamepad polling
together with Steam Input state) because the same physical pad can appear to both APIs
simultaneously under Steam's controller emulation — ORing them risks double-firing an action from
what is, to the player, a single button press, and it defeats the entire point of Steam Input
remapping (a player who rebound "Jump" away from the physical A button in Steam's configurator
would still trigger it here through the untouched GLFW polling).

**Known limitations, deliberate for #893's scope:**

- Only the *first* connected controller (`GetConnectedControllers()[0]`) drives action state —
  the same simplification the pre-existing `GetActionAxisValue`/`IsGamepadButtonPressed` default
  (`gamepadIndex = 0`) already made for raw engine bindings. Local multiplayer through Steam
  Input (per-player controller → per-player action state) is out of scope here; revisit together
  with any local co-op feature, not before.
- `ApplySteamInputForAction` only ever reads `SteamInputAnalogActionState::X`. A Steam Input
  manifest analog action can be 2-axis (a joystick-move action, where `Y` also carries signal);
  an `InputAction` that wants Steam Input to drive a *vertical* engine axis would currently read
  0 from Steam while `Active` is true. There's no ambiguity to resolve on the engine's side of
  `InputAction`/`InputActionMap` — an action is a single named f32, not an X/Y pair — so this
  isn't a bug to fix here so much as a real limit of the current one-axis-per-action design; a
  future 2D "move" action would need a second engine-side action name (or a new binding shape)
  to carry `Y`, not a change to this routing code.
- **Trigger-mode range normalization is handled, but ONLY for that one mode.** Valve's own
  analog-action range conventions differ by `InputAnalogActionData_t::eMode`
  (`EInputSourceMode`): a Trigger-mode action reports `0..1` (`0` = released), a JoystickMove
  action already reports `-1..1`. `SteamworksBackend::GetAnalogActionState` checks `eMode` and
  remaps Trigger-mode `x`/`y` to this engine's own `-1..1` / `-1` = released convention
  (`GamepadAxis::RightTrigger` etc. — see `GamepadCodes.h`) before it ever reaches
  `ISteamBackend`; every other mode passes through unchanged. `SteamInputAnalogActionState`
  itself does NOT carry `eMode` — the normalization happens once, at the SDK boundary, so
  `InputActionManager` and every other caller of `GetAnalogActionState` never need to know Steam
  Input's source-mode taxonomy exists. Covered by `SteamStubOnPathTest`'s
  `TriggerModeAnalogActionAt{Rest,FullPress,HalfPress}Normalizes...` and
  `JoystickModeAnalogActionIsNotRenormalized`.

### Non-goals (deliberate, not forgotten)

- **Leaderboards, DLC entitlements, UGC/Workshop** — `ISteamBackend` still exposes none of these.
  Verified absent by the same `grep -r "ISteamLeaderboard\|ISteamDLC\|ISteamUGC" OloEngine/src/`
  that #893 itself opened with. Revisit only if a specific Drift (#878) feature needs one of
  them — a Steam Deck-relevant example would be a leaderboard for a time-trial mode, which does
  not exist today.

### Manual real-SDK verification checklist (tier 2 from §10)

There is no automated way to prove Steam Input against the real client — the stub proves the
plumbing, nothing about Valve's servers or a real controller. Run this by hand, once per Steam
Input change, on a machine with `STEAMWORKS_SDK_ROOT` set and `480` in
`OloEditor/steam_appid.txt` (§10). It needs a Steam Input action manifest for the test app — a
minimal one action-set-per-`InputContextType`, one digital action per test binding — uploaded via
the Steamworks partner site for App ID 480, or configured locally through Steam's own
"Manage Steam Input" screen for a connected controller if partner-site access isn't available
(the origins Steam reports differ, but the plumbing check is the same).

| # | Step | Expected | Result recorded 2026-08-31 |
|---|---|---|---|
| 1 | Launch with a Steam Input-capable controller connected (Xbox/DualSense/Deck), Steam client running | `SteamManager::IsInputAvailable()` true, `GetConnectedControllers()` non-empty | **PASS.** Against the real client, App ID 480, a Steam Controller (new model): `IsInputAvailable=true ConnectedControllers=1`, live-traced from a running editor. |
| 2 | Switch `InputContextType` (e.g. open a menu) | The corresponding named action set activates in Steam's overlay controller HUD | **PARTIAL.** `ActivateActionSet` reaches the real `ISteamInput::ActivateActionSet` with the correct context-derived name (`ctx=Gameplay`), confirmed live. The returned handle was `0` (invalid) — Spacewar (App 480, a shared app this project does not own) has no Steam Input action manifest defining a `"Gameplay"` action set, so Steam legitimately has nothing to activate. The overlay HUD step itself was not reachable without a bound action set. |
| 3 | Press a button bound to an engine action via Steam's configurator to a DIFFERENT physical button than the engine default | The action fires from the remapped button, not the engine default | **BLOCKED**, same root cause as row 2 — no manifest means no action exists to bind or remap. Authoring a manifest for App 480 needs partner-site access to a game this project doesn't own; this needs a real owned App ID (i.e. once #878/Drift ships with its own App ID) to test for real. |
| 4 | Open Steam's own "Manage controller layout" from the overlay | Reflects the game's action-set names from the manifest | **BLOCKED**, same root cause. |
| 5 | Disconnect the controller mid-session | Engine falls back to keyboard/mouse cleanly, no crash, no stuck-pressed action | **PASS.** Live-traced `ConnectedControllers` dropping `1 → 0` on physical disconnect; editor stayed fully responsive afterward (368 FPS, scene still rendering, no stuck state) — screenshotted. |
| 6 | `GetGlyphPngForDigitalAction`/`GetGlyphLabelForDigitalAction` for a bound action | Returns a real PNG path / human string matching the connected controller type | **BLOCKED**, same root cause as rows 2–4 — nothing is bound to look up a glyph for. |
| 7 | Launch with the Steam client NOT running | `IsAvailable()` and `IsInputAvailable()` both false, engine starts normally, gamepad bindings work via GLFW as before | Not run this session — see §5's Variant A/B contract, exercised automatically by `SteamStubOnPathTest` and `SteamManagerTest` instead. |

**Session notes (2026-08-31):** verification used a temporary trace (`OLO_CORE_WARN` calls in
`InputActionManager::Update`/`ActivateSteamActionSet`, reverted before commit — not shipped) to
observe `SteamManager` state live, since the MCP diagnostics tool registration needs a session
reconnect this run never got. Two real findings came out of it, both structural rather than bugs:
first, the engine's own raw-gamepad diagnostics panel (`GamepadManager`, GLFW-based) correctly
shows 0 connected devices for a Steam Controller under active Steam Input management — that
panel is not, and never will be, the right place to check Steam Input connectivity, since Valve's
controller deliberately does not expose itself as a standard XInput/DirectInput device while
Steam Input owns it. Second, and more load-bearing: **rows 2, 3, 4 and 6 cannot be meaningfully
exercised against App 480**, because Steam Input requires a per-app action manifest (action-set
and action names) that only the app's Steamworks partner can author, and this project does not
own Spacewar. The plumbing that *reaches* the real SDK is proven (rows 1 and 5, both live,
both real); the plumbing that depends on manifest content is structurally blocked until Drift
(#878) has its own App ID to author a manifest against. That is a content/publishing
prerequisite, not an engine-code gap — record it as such rather than reporting a false pass.
