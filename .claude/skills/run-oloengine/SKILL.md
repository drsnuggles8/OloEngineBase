---
name: run-oloengine
description: Build, run, and screenshot the OloEngine apps on Windows — the OloEditor GUI, the OloServer headless dedicated server, and the GoogleTest suite. Use when asked to start/launch/run the editor, screenshot the editor, build OloEditor/OloServer/OloRuntime, run the tests, or confirm a change works in the real running app.
---

OloEngine is a native C++23 / OpenGL 4.6 desktop engine. The headline binary,
**OloEditor**, is a GLFW + ImGui window — there is no DOM and no Playwright
handle, so it is driven through the Win32 window manager by
[driver.ps1](.claude/skills/run-oloengine/driver.ps1): it launches the process
with the correct working directory, waits for the top-level window, and captures
the window's own surface with `PrintWindow` (works even when the editor is **not**
the focused/top-most window — which it usually isn't, since a background process
can't steal foreground on Windows). OloServer is headless (`curl`/log-driven, no
window); the tests are a plain console exe.

All paths below are relative to the repo root (`e:\repos\OloEngineBaseTwo`). All
commands were run on Windows 11 + VS 2026 + an RTX 4090 (real OpenGL 4.6).

## Prerequisites

Already provisioned on this machine; listed for a clean box. There is no
`apt-get` — this is Windows:

- **CMake 4.2+** — the `msvc` preset uses the `Visual Studio 18 2026` generator, which only exists in CMake 4.2+. (`cmake --version` → 4.2.0 here.)
- **Visual Studio 2026** (Community is fine) with the C++ desktop workload. VS 2022 also works via `scripts\Win-GenerateProjectVS2022.bat`.
- **Vulkan SDK 1.3+** with `VULKAN_SDK` set (provides `glslc` for SPIR-V shader compilation). Here: `C:\VulkanSDK\1.4.309.0`.
- **Python 3.10+** with `jinja2` (used to generate the glad2 GL loader at configure time).
- **A GPU with real OpenGL 4.6** for OloEditor/OloRuntime. WSL2's software GL is only 4.5 and will not run the editor.
- **PowerShell 7 (`pwsh`)** to run the driver (Windows PowerShell 5.1 also works; the driver is `#requires -version 5.1`).

## Build

The `build/` tree already exists here. Configure (only needed on a clean clone,
or after editing a `CMakeLists.txt`):

```powershell
cmake --preset msvc
```

Build the targets you need (each line verified, exit 0):

```powershell
cmake --build build --target OloEditor       --config Debug --parallel
cmake --build build --target OloServer        --config Debug --parallel
cmake --build build --target OloEngine-Tests  --config Debug --parallel
```

- OloEditor links to `bin\Debug\OloEditor\OloEditor.exe` (note: `bin\`, not `build\`).
- A first full editor build is long; here only the editor + networking objects were stale, so it linked in a few minutes off the 899 prebuilt engine objects.

## Run the editor (agent path)

One shot — launch, wait out the 42-shader warmup, screenshot, kill:

```powershell
pwsh -NoProfile -File .claude\skills\run-oloengine\driver.ps1 -Action capture
```

The PNG lands at `.claude\skills\run-oloengine\shots\OloEditor-Debug.png` and the
driver prints its size + a luminance mean/spread (a near-zero `StdLum` warns that
the frame is blank — see Gotchas). **Open the PNG and look at it** — a good
capture shows the menu bar, Scene Hierarchy (left), the 3D Viewport, and the
docked Console/Content Browser.

The driver also **snapshots `OloEngine.log` next to the PNG** (`OloEditor-Debug.png`
→ `OloEditor-Debug.log`) on every `capture`/`shot`, because the editor truncates
that file on the *next* launch. If the editor crashes during init (no window
appears), the driver prints the last 30 log lines inline and points you at the
snapshot — read it for shader compile/link errors.

Interactive — leave it running and shoot it repeatedly (e.g. after poking the UI):

```powershell
pwsh -NoProfile -File .claude\skills\run-oloengine\driver.ps1 -Action launch   # detached; stores the PID
pwsh -NoProfile -File .claude\skills\run-oloengine\driver.ps1 -Action shot -Out .claude\skills\run-oloengine\shots\after.png
pwsh -NoProfile -File .claude\skills\run-oloengine\driver.ps1 -Action stop
```

Driver options:

| flag | meaning |
|---|---|
| `-Action capture\|launch\|shot\|stop` | one-shot capture (default), or detached launch / shoot / kill |
| `-Target OloEditor\|OloRuntime` | which GUI binary (default `OloEditor`) |
| `-Config Debug\|Release\|Dist` | build config to launch (default `Debug`) |
| `-SettleSeconds <n>` | render-settle before capture (default 30 — covers the shader warmup) |
| `-WaitSeconds <n>` | max wait for the window to appear (default 60) |
| `-Method print\|screen` | `print` (default) = `PrintWindow`, captures the window even when occluded. `screen` = desktop BitBlt at the window rect, **only** correct if OloEditor is the top-most window |
| `-Out <path>` | output PNG (default `shots\<Target>-<Config>.png`) |
| `-KeepOpen` | with `capture`: leave the app running after the shot |

## Run the server (headless)

No window — it binds a UDP port and reads stdin console commands. Run from the
`OloEditor\` working directory (assets resolve relative to it):

```powershell
$p = Start-Process bin\Debug\OloServer\OloServer.exe -WorkingDirectory OloEditor `
       -RedirectStandardOutput "$env:TEMP\oloserver.log" -PassThru
Start-Sleep 5; Stop-Process -Id $p.Id -Force
Select-String "Listening on port" "$env:TEMP\oloserver.log"
```

Expected: `[Server] Listening on port 7777` (defaults: port 7777, 64 players,
60 Hz). Override with CLI args parsed by `ServerConfigSerializer::ParseCommandLine`.

## Test

The test binary runs from the **repo root** (not `OloEditor\`):

```powershell
build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=FastRandomTest.*:ContainerSmoke.*
```

→ `[ PASSED ] 13 tests.` Drop the filter to run the whole suite. List suites with
`--gtest_list_tests`. Note: a guessed filter that matches nothing exits 0 with a
`did not match any test` warning — confirm tests actually ran.

## Run the editor (human path)

```powershell
# from a normal terminal; a 1280x720 window opens, Ctrl-C or close it to quit
cd OloEditor; ..\bin\Debug\OloEditor\OloEditor.exe
```

Useless from a non-interactive/disconnected session (no visible window to see).

## Gotchas

- **`SetForegroundWindow` from a background process is silently blocked by Windows.** So `-Method screen` (desktop BitBlt at the window rect) captures whatever is actually on top — in testing it grabbed the VS Code window sitting over OloEditor, not OloEditor. The driver defaults to `-Method print` (`PrintWindow` with `PW_RENDERFULLCONTENT`), which copies the window's *own* surface regardless of z-order/occlusion. Only use `-Method screen` if you've confirmed the "correct" OloEditor is the visible top-most window.
- **The editor shows a warmup splash for loading shaders for ~10–25 s before the real dockspace UI renders.** Capturing too early gets the splash, not the editor. Default `-SettleSeconds 30` clears it. First launch is slowest (SPIR-V cross-compile + GL link of 42 PBR variants); later launches hit the mesh/shader cache and are faster.
- **`-Action launch` must detach the process** (the driver uses `UseShellExecute=$true` for it). An earlier version started the editor as a console child of the launching `pwsh`; when that `pwsh` exited, the editor died with it ("Renderer Memory Tracker shutdown" right after launch) and the follow-up `shot` found no window. `capture` is immune because one `pwsh` owns the whole launch→shot→kill sequence.
- **Working directory must be `OloEditor\`.** Editor, runtime, and server resolve shaders, assets, and Mono assemblies relative to it. The driver sets this for you; the raw exe will fail to find shaders if run from elsewhere.
- **The editor and server both write `OloEngine.log` to the cwd (`OloEditor\OloEngine.log`), truncating on open** — a server run clobbers the editor's log and vice-versa, and the *next* editor launch wipes the previous run's log. That's why the driver snapshots it to `shots\<name>.log` per run; for the server, use `-RedirectStandardOutput` instead of relying on the shared file.
- **Captured PNG size varies** (1924×1127 vs 3840×2088 here) because the window opens at whatever size `OloEditor\imgui.ini` last saved — sometimes 1280×720, sometimes maximized to the desktop.
- **Editing any `CMakeLists.txt` forces a full CMake reconfigure on the next build** (~60 s here — `cmake --preset msvc` reported "Configuring done (59.4s)" — and FetchContent re-checks several vendored repos). A reconfigure also relinks the editor/tests on the next build even with no source change, so the "no-op" build after a configure isn't instant. Expected, not an error.

## Troubleshooting

- **`Could not find OloEditor.exe`**: it isn't built. Run the `cmake --build ... --target OloEditor` line above. The driver also searches `bin\` and `build\` for the freshest matching exe as a fallback.
- **Capture is black / `StdLum` < 3 (driver warns)**: the session is non-interactive or RDP-disconnected, so the DWM never composited the window. Run from an interactive desktop session; `PrintWindow` needs the window to have been drawn at least once.
- **`Process exited early ... before a window appeared`**: the editor crashed during init. The driver prints the last 30 log lines inline; read the full `shots\<name>.log` snapshot for shader compile/link errors or a missing asset.
- **The *first* `capture` after a fresh editor build occasionally exits 1 with no PNG** (only the `.log` snapshot is written). Observed this session: the failing run was still recompiling shaders from source (`Shader source or include newer than cache, recompiling: …` in the snapshot) when the 30 s settle elapsed. The immediate re-run hit the warm shader cache and captured cleanly (`StdLum 40.1`), as did every subsequent `launch`/`shot`. **Just re-run `capture`**, or bump `-SettleSeconds` for the first shot after a build.
- **Server exits a few seconds after `Listening on port 7777` when launched non-interactively**: its console reads stdin EOF and shuts down. Fine for a smoke check; for a real session launch it in an interactive terminal.
