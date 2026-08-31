# Shipping a game to Steam

Issue [#894](https://github.com/drsnuggles8/OloEngineBase/issues/894), the last of the four
shipping gates filed alongside epic #878 (*Drift*). `GameBuildPipeline` (see
[GameBuildPipeline.h](../../OloEngine/src/OloEngine/Build/GameBuildPipeline.h)) turns an active
editor project into a self-contained game folder — this document covers everything **after**
that: what of the folder actually ships, how it's identified, how it gets uploaded, and what a
person still has to do by hand.

**Explicitly out of scope** (per the issue): store page assets, trailers, pricing, review
scheduling.

## Table of contents

- [Depot layout](#depot-layout)
- [Build identity](#build-identity)
- [Installer decision](#installer-decision)
- [steamcmd upload automation](#steamcmd-upload-automation)
- [Doing a real publish](#doing-a-real-publish)

## Depot layout

`GameBuildPipeline::Build` writes this structure (Windows target shown — see
[GameBuildPipeline.h](../../OloEngine/src/OloEngine/Build/GameBuildPipeline.h) for the Linux
differences):

```
OutputDirectory/GameName/
├── GameName.exe
├── *.dll
├── game.manifest
├── config/renderer.yaml
├── Assets/
│   ├── AssetPack.olopack
│   └── <loose .lua scripts, loose runtime textures>
├── Config/
│   └── InputActions.yaml
├── Scenes/
│   └── *.olo
├── assets/
│   ├── shaders/
│   ├── ShaderPack.osp        (optional, issue #908)
│   ├── fonts/                (optional)
│   └── textures/             (optional)
├── mono/
│   ├── lib/
│   └── etc/
└── Resources/
    └── Scripts/
        └── OloEngine-ScriptCore.dll
```

Nobody had drawn the line on what of this a Steam depot actually needs. It does — **every file the
pipeline writes ships**, because the pipeline already only writes files the runtime reads at
some point (this was true before #894; it's the reason the depot line turned out to be short).
The include/exclude decisions that matter are about what a naive "just zip the build output
folder" approach would drag in that the pipeline does **not** write:

| Path | Ship? | Why |
|---|---|---|
| `GameName.exe` / `*.dll` | **Ship** | Entry point + runtime-adjacent dependencies (Windows only — Linux has no DLLs). |
| `game.manifest` | **Ship** | Required — the runtime reads it before anything else. |
| `config/renderer.yaml` | **Ship** | Read before the window opens; a missing file falls back to OpenGL, but shipping it makes the default explicit rather than accidental. |
| `Assets/AssetPack.olopack` | **Ship** | The packed asset content. |
| `Assets/<loose .lua / textures>` | **Ship** | Resolved by asset-relative path at runtime (`Project::GetAssetFileSystemPath`) — see `CopyScriptFiles`'s doc comment. Not optional; the game breaks without them. |
| `Config/InputActions.yaml` | **Ship** | Default bindings. Deliberately **writable** — `RuntimeInputRebindMenu` overwrites this exact file when the player rebinds a control, so it must live inside the installed depot tree, not in a read-only resource pack. |
| `Scenes/*.olo` | **Ship** | Loaded from disk at runtime; the asset registry doesn't track `.olo` files. |
| `assets/shaders/` | **Ship** | Fallback source the runtime compiles from when no shader pack is staged. |
| `assets/ShaderPack.osp` | **Ship, when present** | Pre-compiled SPIR-V — cuts first-launch shader compile time. Its absence is not an error (#908); older builds and non-CI-baked local builds simply don't have one. |
| `assets/fonts/`, `assets/textures/` | **Ship, when present** | Non-fatal if missing (falls back to the built-in font); ship them when the project has them. |
| `mono/lib/`, `mono/etc/`, `Resources/Scripts/*.dll` | **Ship (Windows only)** | Required for C# scripting. `IsScriptingAvailableOnPlatform` already makes this a no-op on Linux — nothing extra to exclude there. |

### What must NOT reach the depot

None of these are things `GameBuildPipeline` writes today — they're the mistakes a
"skip the pipeline, just zip `build-cached/…/OloRuntime/`" shortcut would make, called out
explicitly so nobody reaches for that shortcut:

- **Debug symbols (`.pdb`, `.ilk`)** — never produced by the pipeline (it copies the runtime
  *binary*, not the build tree it came from), but flagged here as a standing decision: the retail
  depot ships **no** debug symbols. Keep them locally (or in a separate symbol store) for
  post-mortem debugging; revisit only if Steam's minidump/crash-report symbol upload is adopted.
- **`steam_appid.txt`** — the developer-only App ID 480 (Spacewar) affordance documented in
  [build.md](build.md#steamworks-sdk-optional--you-must-obtain-it-yourself). Already git-ignored;
  `Publish-ToSteam.ps1` refuses to run if one is found inside the content root (see below).
- **The build tree itself** — `vcpkg_installed/`, `CMakeCache.txt`, object files, `.git/`. None of
  these are ever inside `OutputDirectory/GameName/`; called out only because "zip the whole repo"
  is the failure mode this line exists to prevent.
- **The Linux `.desktop` entry, on the Steam depot specifically.** `WriteLinuxDesktopEntry` (#891)
  writes one for a plain tarball/itch.io-style distribution where the player manages their own
  desktop launcher. On Steam, the client owns the launcher (Big Picture / library entry), so a
  shipped `.desktop` file is redundant, not harmful — it doesn't need a `FileExclusion` entry
  either, since Steam simply never reads it. Documented so nobody spends time trying to make
  Steam use it.

### Per-platform depots

Today there is one depot: Windows, because `IsBuildTargetSupportedOnThisHost` only supports
building for the host platform and this project's host is Windows. A Linux depot is a **direct
consequence of #892** (the Linux/Proton editor decision): if that gate lands on native Linux
rather than relying on Proton to run the Windows build, add a second Steam depot ID with the
Linux build's own `contentroot` — the layout table above already documents the Linux differences
(no DLLs, no `mono/`, no `Resources/Scripts/`). Until #892 resolves, Linux players run the Windows
depot under Steam's own Proton compatibility layer, which needs no second depot at all.

## Build identity

*"The piece whose absence hurts most after launch and costs least to add before it."*

`OloEngine/src/OloEngine/Core/BuildInfo.{h,cpp}` exposes the version + git identity baked in at
CMake configure time (root `CMakeLists.txt`, computed once and passed to `OloEngine`'s compile
definitions):

- `BuildInfo::GetEngineVersion()` — the static semver (`CMAKE_PROJECT_VERSION`, currently
  `"0.0.1"`).
- `BuildInfo::GetGitHash()` — a 10-char abbreviated commit hash (`git rev-parse --short=10 HEAD`),
  or `"unknown"` outside a git checkout.
- `BuildInfo::GetGitDescribe()` — `git describe --tags --always --dirty`, for display; falls back
  to the abbreviated hash when the checkout has no tags (true of this repo today). This is
  display-only — see `IsWorkingTreeDirty()` below for why nothing parses its `-dirty` suffix.
- `BuildInfo::GetBuildTimestamp()` — the UTC configure-time timestamp, ISO 8601.
- `BuildInfo::IsWorkingTreeDirty()` — whether the working tree had uncommitted changes to tracked
  files at configure time, computed independently via `git diff-index --quiet HEAD --` (the same
  check `git describe --dirty` uses internally) rather than by string-matching a `-dirty` suffix on
  `GetGitDescribe()`'s text. That distinction matters: a real, permitted git tag literally named
  `release-dirty` would make a perfectly clean checkout's `describe` output end in `-dirty` too, so
  parsing the string can't tell "dirty tree" from "tag that happens to end that way" apart.
- `BuildInfo::GetBuildId()` — `"<version>+<git hash>"`, with a `-dirty` suffix appended when
  `IsWorkingTreeDirty()` is true — the one string every bug report should be able to quote. Falls
  back to just the version when the hash is `"unknown"` (no dangling `+unknown` suffix).

**Where it shows up:**

- `Application::Run` logs `"[<AppName>] Build: <id>"` at startup for every app — editor, runtime,
  server.
- `game.manifest`'s `Game.EngineVersion` and `Game.BuildId` fields (`GameBuildPipeline::WriteGameManifest`)
  — the depot's own record of what it was packaged from.
- `OloRuntimeApp` logs the build id it read **from the manifest** (falling back to the running
  binary's own `BuildInfo` if an older manifest predates the `BuildId` field) — so a player running
  a build that was re-packaged without a matching engine rebuild still reports the identity that
  was actually shipped, not whatever happens to be linked into the exe.

This is enough to answer "which build is this bug report about" from a log line or the manifest
alone — no in-game overlay was added, since neither the acceptance criteria nor Drift's existing
UI called for one; the log line is the "readable at runtime" the issue asked for. A future debug
overlay can call `BuildInfo::GetBuildId()` directly if one is added later.

## Installer decision

**No installer.** Steam's client handles install, update, verify-integrity and uninstall for
every platform this project ships on (Windows depot today; Linux under Proton, or natively if
#892 lands that way). Adding NSIS/WiX/etc. would duplicate what Steam already does, diverge from
what a Steam player expects (a Steam game that pops its own installer is a known bad pattern), and
give this project a second update mechanism to keep in sync with the depot. This decision is
final for the Steam release; revisit only if a non-Steam distribution channel (itch.io, a direct
download) is added, at which point a self-contained zip of the depot folder is the more likely
answer than a traditional installer.

## steamcmd upload automation

`scripts/steam/` holds the upload tooling — **it is developer-run, not CI-run**, for the same
reason Steamworks credentials never reach this repo's SDK build (see
[build.md](build.md#steamworks-sdk-optional--you-must-obtain-it-yourself)): `steamcmd` login
normally requires interactive Steam Guard 2FA on first use per machine, this is a public repo, and
publish credentials are exactly the kind of secret that should never sit in a CI runner's
environment for a build system that also runs untrusted PR code.

```
scripts/steam/
├── templates/
│   ├── app_build.vdf.template     — Valve's app-build config, tokenised
│   └── depot_build.vdf.template   — Valve's depot-build config, tokenised
└── Publish-ToSteam.ps1            — renders the templates and (optionally) runs steamcmd
```

### The VDF templates

Standard Valve `app_build.vdf` / `depot_build.vdf` shape (see
[Steamworks: Build Scripts](https://partner.steamgames.com/doc/sdk/uploading#2)), with `{{TOKEN}}`
placeholders `Publish-ToSteam.ps1` substitutes at run time — committed so the app/depot
configuration is reviewable in a PR rather than hand-typed on someone's machine each release. Real
App/Depot IDs are **not** hardcoded in the templates: Drift doesn't have a published Steam App ID
yet, and hardcoding an ID that later turns out wrong is exactly the kind of mistake a template
should make impossible to skip past. Pass `-AppId` / `-DepotId` explicitly (or set
`STEAM_APP_ID` / `STEAM_DEPOT_ID`) every run.

The depot template excludes `*.pdb`, `*.ilk` and `steam_appid.txt` via `FileExclusion` entries —
belt-and-suspenders on top of the depot-layout decisions above, since `GameBuildPipeline` doesn't
produce the first two and `steam_appid.txt` is git-ignored, not pipeline-produced.

### `Publish-ToSteam.ps1`

```powershell
pwsh scripts/steam/Publish-ToSteam.ps1 `
    -BuildPath  "C:\Builds\Drift\Drift"     `   # a GameBuildPipeline output folder
    -AppId      480                         `   # your real Steam App ID (480 = Spacewar, dev-only)
    -DepotId    481                         `   # your real Depot ID
    -BetaBranch "beta"                      `   # NEVER omit — see the branch guard below
    -WhatIf                                     # render + validate only, never touches steamcmd
```

Parameters, and the environment variables that back the ones you'd otherwise retype every run:

| Parameter | Env fallback | Notes |
|---|---|---|
| `-BuildPath` | — | A `GameBuildPipeline` output folder (must contain `game.manifest`). |
| `-AppId` | `STEAM_APP_ID` | |
| `-DepotId` | `STEAM_DEPOT_ID` | |
| `-BetaBranch` | — | Required. See the guard below. |
| `-SteamUser` | `STEAM_BUILD_ACCOUNT` | The steamcmd login name — **not** the password. Only required past the `-WhatIf` gate — rendering and validating the VDFs needs no Steam credentials at all. |
| `-SteamCmdPath` | `STEAMCMD_PATH` | Path to `steamcmd.exe`; the developer installs this themselves, same pattern as `STEAMWORKS_SDK_ROOT`. Same "only required for a real run" rule as `-SteamUser`. |
| `-Description` | — | Optional build description shown in the Steamworks partner site. |
| `-WhatIf` | — | Renders and validates the VDFs (a REAL local file write, not suppressed) and prints them, then stops **before** invoking steamcmd. Always try this first. |
| `-Force` | — | Required in addition to `-BetaBranch public`/`-BetaBranch default` — see below. |

**The branch guard exists on purpose.** `-BetaBranch public` or `-BetaBranch default` (steamcmd's
name for the default/live branch) is refused unless `-Force` is also passed, with a loud warning
printed either way — publishing straight to what players are currently running should never be
the accidental result of a copy-pasted command. #894's own acceptance criterion is a **beta**
branch dry run for exactly this reason.

**Credentials.** `steamcmd`'s own login flow handles the password and 2FA prompt interactively;
the script never asks for, stores, or accepts a password — only the login *name*. After the first
successful interactive login on a machine, steamcmd caches a session so subsequent runs from that
same machine don't re-prompt. This mirrors the account-level, developer-supplied-credential
pattern documented for the Steamworks SDK itself.

Rendered VDFs are written to `scripts/steam/generated/` (git-ignored — they embed a real,
machine-local `contentroot` path).

## Doing a real publish

This session has no Steam partner credentials and, per `CLAUDE.md`, must never execute a real
`steamcmd` upload without explicit user confirmation even if it did. What's automated stops at
`-WhatIf`. To actually complete the issue's "one complete dry run to a Steam beta branch"
acceptance criterion, a human with Steamworks partner access needs to:

1. Build a real depot folder — open the project in `OloEditor`, use the Build Game panel
   (`GameBuildPipeline`) to produce `OutputDirectory/GameName/`.
2. Install `steamcmd` and set `STEAMCMD_PATH` (and `STEAM_BUILD_ACCOUNT`) — see Valve's
   [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD) docs.
3. Run `Publish-ToSteam.ps1` with `-WhatIf` first and read the rendered VDFs.
4. Re-run without `-WhatIf`, targeting a **beta** branch (never `-BetaBranch public`/`default`
   without deliberately also passing `-Force`), against either:
   - App 480 (Spacewar) — free, needs no published app, proves the upload mechanics end to end but
     isn't a *real* app's depot, or
   - the project's actual (once created) Steam App/Depot ID, once Steam Direct is paid and an app
     page exists.
5. Verify the build landed on the Steamworks partner site (SteamPipe → Builds), then install it
   from the beta branch via the Steam client and confirm it launches.

Until a human does that, treat build identity, the depot layout doc and the upload scripts as
**done and reviewable**, and the actual dry-run execution as the one open item this PR cannot
close on its own.
