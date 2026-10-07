# Mono debugging

The Mono debugger agent starts by default only in an `OLO_DEBUG` editor build.
Release editors, packaged runtimes, dedicated servers and direct `ScriptEngine::Init()`
hosts leave it disabled. Managed assembly loading and execution still work with it off.

Pass `--mono-debug` to any engine application to enable the agent explicitly:

```powershell
./MyGame.exe --mono-debug
```

Attach a Mono soft debugger to `127.0.0.1:2550`. Enabled sessions use the existing
fixed port, run without waiting for attachment (`suspend=n`), and write
`MonoDebugger.log` in their working directory. Only one enabled session can own
that port at a time. Ordinary runtime and server sessions do not contend for it.

The startup log reports `Mono debugging enabled (127.0.0.1:2550)` or
`Mono debugging disabled`. The decision is fixed before Mono initializes and
applies to both core and app portable PDB loading, including assembly reload.
Disabled sessions do not look for PDBs or create a debugger log. With debugging
disabled, managed exceptions still report their messages and managed stack traces;
portable PDB source locations require an enabled session.

Build script assemblies with portable PDBs (`DebugType=portable`, already set in
the project's C# projects). Build Game stages matching PDBs beside the core and
game DLLs when available, so an opted-in package can use source breakpoints.
If a script module is configured, missing ScriptCore or app DLLs fail Build Game
with the missing source path. Projects without a configured script module may omit ScriptCore.
Ordinary launches leave those files unread. `OLO_WITH_CSHARP` controls assembly build
targets; disabling it does not remove embedded Mono from Windows hosts. Hosts compiled
without Mono (currently Linux) ignore the opt-in and report that C# scripting is
unavailable. Lua continues to use its own script engine.

The [host verification evidence](../testing/evidence/mono-debugger-1540/README.md)
records the Debug/Release default and opt-in matrix, source breakpoints, packaged
game concurrency and startup with managed assembly targets disabled.
