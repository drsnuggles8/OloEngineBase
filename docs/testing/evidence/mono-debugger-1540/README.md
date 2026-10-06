# Mono debugger host policy evidence

Issue #1540 is verified against real embedded Mono and shipped Windows hosts in
Debug and Release. `manifest.json` records source, executable, input and raw-log
hashes. The debugger protocol probe performs the DWP handshake, finds portable
PDB source sequence points and installs a source breakpoint. Runtime and server
opt-in reports also require a matching breakpoint event.

| Cell in each configuration | Reports | Oracle |
|---|---|---|
| Editor, packaged runtime, server; default and opt-in | `*-live-*.json` | Owned listeners, PDB reads, debugger log, managed exception, Lua execution and clean exit; editor reload and Play/Stop through MCP |
| Application, editor and direct API; default and opt-in | `*-application-host-*.json`, `*-editor-*.json`, `*-direct-init-*.json` | Stored policy survives reload; managed transform mutation and exception capture remain available |
| Production Build Game | `*-package.json` | Cooked pack and staged runtime; managed DLL/PDB bytes match their sources |
| Production symbol-staging failure | `*-package-symbol-failure.json` | Builder fails and reports the failed symbol copy |
| 87 focused regressions | `*-regression-tests.json`, `*.xml` | 86 pass; only the Linux launcher test skips on Windows; full output retains startup messages after 128 KiB of logging and empty project script paths stay unset |
| Required managed assemblies | `*-package-missing-core*.json`, `*-package-missing-app*.json` | A configured script module requires ScriptCore and the app DLL; no-module projects retain the optional skip. Before-fix Release reports record both reproduced false successes |
| C# assembly targets OFF, assemblies absent | `*-assemblies-off.json` | Six shipped-host startup cells plus a real Lua server scene; policy and clean shutdown survive missing assemblies |

`Release-cooked-concurrent.json` records two ordinary cooked games alive together,
executing C# and Lua while the verifier binds and listens on port 2550. Neither
game owns a Mono listener, reads PDBs or creates `MonoDebugger.log`.

`build-verification.json` and its log excerpts cover the complete native/Lua
target graph in both configurations with C# assembly targets ON and OFF. The ON
build also includes both managed assembly targets. The OFF configure report pins
their absence from the generated target graph. Windows still embeds Mono when
`OLO_WITH_CSHARP=OFF`; missing assemblies are removed only from a private probe
directory and restored afterwards.

The before-fix server log and crash stack identify the headless shutdown failure:
cursor release reached `TryGetGlfwWindow` without a window. The scene-start
oracle in `AppLaunchSmoke.OloServerStopsAHeadlessSceneCleanly` verifies the repaired
shutdown path in both configurations.

Linux uses the Mono-free scripting stub. The UBSan build runs the policy and
shipped-host smoke cases before uploading its tests-only shard tree, requires
only the two unavailable editor cases to skip, and uploads
`linux-mono-free-host-startup` XML separately as PR CI evidence.
`Linux-host-startup-before-ci-fix.{json,xml}` preserves the initial run in which
all eight host cases skipped because the native binaries were absent.
Editor startup is unavailable on Linux by ADR 0015. Renderer backend, path, MSAA
and resolution are outside this policy change. Scene source YAML and cooked pack
routing are exercised; save-game paths do not call the affected source methods.

Scratch probes and full logs remain untracked. Committed log excerpts retain
the policy, execution and packaging diagnostics; hashes identify the full captures.
Committed text artifacts use LF and omit trailing whitespace. `captureSha256`
identifies an original report when its bytes differ from the normalized copy.
Editor probes save their disposable scene before requesting close when its undo
history is dirty, then restore the original fixture bytes. Default-host reports
record any pre-existing Mono listener independently; they assert no owned Mono
listener, PDB read, debugger log or contention error while that session remains
available. Enabled-debugger probes wait for port 2550 before launching.
