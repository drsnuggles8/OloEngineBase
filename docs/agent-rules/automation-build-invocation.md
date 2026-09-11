# A build started from inside the editor goes through the lock, or it does not happen

Issue #1163. `olo_build_run` spawns `.claude/skills/run-oloengine/build-lock.ps1` and nothing else.
It never runs `cmake --build` directly, never sets `OLO_BUILD_LOCK_OVERRIDE`,
`OLO_BUILD_LOCK_BYPASS` or `OLO_NOT_A_BUILD`, and refuses to start at all when the script is
missing.

Those three are **not one group**, and the difference is the point:

- `OLO_BUILD_LOCK_OVERRIDE` (and the legacy `OLO_BUILD_LOCK_BYPASS`) permit a **real unlocked
  build**. They are **audited** to `olo-build-metrics.jsonl`, and by policy a human asks for one
  per build.
- `OLO_NOT_A_BUILD` only allows a command that *mentions* a build tool without running one. It is a
  silent allow and is **not** audited.

So an automation command setting the first would be an unaudited build; setting the second would be
a lie about what it is doing. Neither appears here, and the whole point of the lock is that there is
no third path.

The rest of this file is the contract that follows from that, and the reason the exit code of a build
is not evidence that a build happened.

---

## 1. Wait for the lock, bounded, and name the holder when you give up

**Do not fail fast by default, and do not wait forever.** Both are wrong here, for opposite reasons:

- Refusing whenever the lock is held would fail for a hold about to end in two seconds. Contention is
  the normal state on this box — five worktrees is routine — which is exactly why the script has a
  FIFO ticket queue rather than a refusal. Its own contract is *"a build that cannot start **queues**;
  it does not fail"*.
- Waiting the script's default `-TimeoutMinutes 180` turns an automation call into an indefinite
  hang. The caller's own timeout fires first, it abandons the call, and the build keeps running with
  nothing in the call scope watching it.

So the caller sets a budget (`lockWaitSeconds`, default 300) and it becomes the script's
`-TimeoutMinutes`. **That parameter is integer minutes, so the conversion is a ceiling division**:
300 s becomes 5 minutes, and any non-zero value under a minute rounds **up** to 1 — a caller asking
for 30 s waits 60. `0` is the one exact case, and it is exact fail-fast: the deadline is already
past when the first acquisition attempt fails. Report the applied budget alongside the requested
one, or a refusal is measured against a number the caller never sees. Never pass `-Priority` —
jumping the queue is a decision the user makes, per build.

When the budget expires the script throws naming the holder's pid and worktree; that text is the
error. **A build that could not start is an error naming the reason, never an empty success.**

## 2. `build-lock.ps1` exits 0 without building, and the case is easy to miss

Its `Test-Superseded` path: when an identical command for the same worktree has been queued *later*,
the older waiter stands down — `exit 0`, having built nothing. That is correct behaviour (the newer
request supersedes a stale snapshot), and it is indistinguishable from a successful build if you only
read the exit code.

Two more ways to get a zero exit with no build: an incremental build with nothing to do, and a build
that never started while last week's binary sat exactly where you look.

**Therefore: take the verdict from the artefact, not the exit code.** Stat the target's primary output
before and after, and compare its mtime against the instant the build *started* — not against the
previous stat, which cannot tell a first build from a no-op and is fooled by a byte-identical rebuild
at the same timestamp granularity. Keep `rebuilt` and `up-to-date` as separate answers. Merging them
is how a stale binary comes to be reported as fresh, which has already burned this repo once
(a stale prebuilt binary read as a successful build, and a crash was attributed to the wrong commit).

`olo_build_run` reads the script's own `[build-lock] …` lines out of the captured console for this:
the stand-down, the orphan kill, the concurrency refusal and the chosen job count are all only
visible there.

## 3. The launching process is the lock's identity — so spawn the child directly

`Get-ParentIdentity` reads `Win32_Process.ParentProcessId` of the pwsh it runs in. Whatever spawns
pwsh *is* the identity, pinned by (pid, StartTime), and the parent watch kills the whole build tree
when that process disappears.

So spawn pwsh **directly** from the editor. Inserting anything between them — a `cmd /c`, a detached
launcher, a shell wrapper — silently moves the identity to the shim, and the editor dying then reaps
nothing. Nothing warns you; the build simply keeps running and keeps the lock.

The lock has no notion of a *call*, only of a process, so call-scoped cancellation is yours:

## 4. Cancelling must kill the tree, not the shim

Killing pwsh alone is worse than not cancelling. The OS closes its lock handle — releasing the lock —
while ninja and its compilers keep running unbounded. That is precisely the shape the lock exists to
prevent, and this box has been OOM-killed by an uncapped build.

So the child is created **suspended**, assigned to a Win32 job object with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, then resumed; cancelling terminates the *job*. Suspended-then-assign
is not fussiness: start it running and there is a window in which it can spawn ninja outside the job,
and those grandchildren survive the cancel.

This is the "stronger version" `build-lock.ps1`'s own header says it wants and skipped because it
needs P/Invoke from PowerShell. From C++ it is three Win32 calls, and it also covers what the parent
watch cannot: an editor killed with `TerminateProcess` runs no cleanup, but the OS closes the job
handle on process exit and the build dies with it — immediately, rather than after the ~10 s grace
window, and without needing pwsh alive to notice. If the job object cannot be created, **refuse the
build** rather than running it uncontained.

## 5. The editor may not build itself, and the refusal is an allow-list

Windows keeps a running image mapped, so the linker cannot replace `OloEditor.exe`: the build compiles
everything and *then* fails with `LNK1168` — the most expensive possible way to say no. Mono holds
`OloEngine-ScriptCore`'s assembly the same way.

Refuse by allow-list, not deny-list. A deny-list leaks through `all`, `ALL_BUILD` and `install`, which
reach `OloEditor` without naming it, and through any target added later. There is deliberately **no
default target**, because CMake's default is `all`.

The allow-list is also the injection boundary: target, configuration and build directory are
interpolated into a PowerShell command string that the script writes to a file and executes. No
escaping scheme makes freehand text safe there, so all three are matched against fixed tables in
`OloEditor/src/Automation/AutomationBuildInvocation.h` and anything else is refused by name.

## 6. A redirected PowerShell log is not UTF-8

`build-lock.ps1`'s source writes an em dash in `waiting — …` and `not building alongside the current
build — …`. By the time it reaches a redirected log it is a plain ASCII `-`: the host transcodes its
output to the console code page on the way out. Verbatim, from this box on 2026-09-10:

```
[build-lock] not building alongside the current build - only 22.6 GB free (need 24)
```

A parser matching the literal em dash passes every test written from the script's source and returns
garbage on every real machine. Anchor on the ASCII prefix and skip the separator by character class.

## 7. Two facts about where artefacts land

- **`bin/` is under the SOURCE tree**, not the build tree (`olo_configure_app` →
  `olo_set_output_directories` in `cmake/CommonProperties.cmake`). `build-cached/` and `build/` write
  the same `bin/Debug/OloRuntime/OloRuntime.exe`, so the artefact path can never say which tree
  produced it. The timestamp can say when. `OloEngine-Tests` calls neither helper and keeps the
  Ninja-multi-config default *inside* the build directory — the two layouts are not interchangeable.
- **Building `OloEngine` runs `GenerateBindings`**, which rewrites tracked generated sources. A build
  command moves the working tree; say so where a caller will read it.

## 8. This is not a second `BuildGamePanel`

`BuildGamePanel` **packages** a game: it copies a prebuilt `OloRuntime` and can only *warn* when that
binary is stale. `olo_build_run` is what produces it — literally the answer to the panel's own
*"Build OloRuntime in `<config>` configuration first"* error. They are two halves, not two opinions.
Packaging stays out of scope for the build command (#1163 says so explicitly); if that changes, the
panel's pipeline is the thing to invoke, not to reimplement.
