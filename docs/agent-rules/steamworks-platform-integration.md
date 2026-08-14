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

To reproduce the stub build on Windows, configure it like the normal tree — leave USD / Alembic /
MaterialX **on** — and add only `-DOLO_WITH_STEAM_STUB_SDK=ON`:

```powershell
cmake -S . -B build-steamstub -DOLO_WITH_STEAM_STUB_SDK=ON `
  -DOLO_FFMPEG_PREFIX="<repo>/OloEngine/vendor/ffmpeg-install"
```

Note also that `STEAMWORKS_SDK_ROOT` should be **unset** for that configure — the stub path must
not need an SDK, and unsetting it is what proves so.

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
