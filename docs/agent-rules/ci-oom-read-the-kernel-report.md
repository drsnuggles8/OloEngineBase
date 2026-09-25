# A CI OOM kill: read the kernel's report before naming a cause

**When a job on the self-hosted box dies with `Killed signal terminated program cc1plus`
(or loses its runner), read the kernel's OOM report first.** It names the cgroup that was
full and lists every process resident in it, with RSS. That settles "our job, or a
neighbour?" and "which compiles?" in one command, where the job log can only say that
something was killed.

```bash
ssh 192.168.178.20 'journalctl -k --since "<date> <time>" --no-pager' \
  | grep -E "oom-kill:constraint|Killed process|\[ *[0-9]+\]"
```

Read two things:

- **`oom-kill:constraint=CONSTRAINT_MEMCG ... oom_memcg=<path>`.** The path is the cgroup
  whose `memory.max` was reached. On this box `actions-runner-ci-N.service` is one runner
  unit (14 GiB) and `user-1004.slice` is the account slice both slots share (19 GiB). A
  kill under the unit's path means that job filled its own unit; its neighbour on the
  other slot was not involved. `CONSTRAINT_NONE` / `global_oom` means the whole host ran out.
- **The task table** above that line: one row per process in the cgroup, with `rss_anon`
  and `swapents` in 4 KiB pages. Add up the compilers to get what was resident.

The box's journal keeps the report across nights; the Actions log expires first.
`scripts/heavy-compile-semaphore.py --report` and the `Compile memory report` step of
`gpu-conformance-amd.yml` now print the same numbers in the job log, from inside the job.

## What this found (#1473)

`gpu-conformance-amd.yml` was red for 20 nights. The workflow's own comment blamed an
overlap with asan.yml's nightly on the other runner slots, and the fix it recorded,
retiring a third runner, left the job red. The kernel report for every night from
2026-09-09 to 2026-09-25 disagreed on both counts:

- **Constraint:** `CONSTRAINT_MEMCG` on the job's **own** runner unit, on the old dedicated
  runner and on `olo-ci` alike. asan.yml's nightly does not even run on the box.
- **Task table, 2026-09-25:** six `cc1plus`, `SceneSerializer.cpp` and five
  `LuaScriptGlue*.cpp` parts, **2.08-2.41 GiB anon each, 13.66 GiB together**, plus
  ~0.85 GiB swapped, in a 14 GiB unit. The first 829 build edges had run at the same
  `-j6` without trouble. The problem was which six TUs ran together, not how many.

Those TUs are exactly the `olo_heavy` compile-pool set, and the pool did not exist in that
build: it needs CMake 4.4 for a per-file-set `JOB_POOL_COMPILE`, and the runner has the
distro's CMake 3.31.8. Nothing reported that. The fix, `OLO_HEAVY_COMPILE_SEMAPHORE`
(`cmake/HeavyCompileSemaphore.cmake`), applies the same bound through a compiler launcher,
which works on any CMake version with the Ninja and Makefile generators.

## Two traps met on the way

**`cmake --version` over SSH is not the runner's CMake.** The admin account resolves
`~/.local/bin/cmake` (4.4.0). The runner user resolves `/usr/bin/cmake` (3.31.8). A feature
gated on the CMake version has to be checked against the job's own log line
(`cmake version ...` in the toolchain preflight), never against an interactive shell.

**A `#` line inside a folded `run: >` block comments out the rest of the command.** YAML
folds the block into one shell line, so the first `#` starts a shell comment that runs to
the end of the command. `-DOLO_LINK_SEMAPHORE_SLOTS=1` sat after such a comment in this
workflow, and the configure log said `Link semaphore: ON, 2 permit(s)`. Put comments above
`run:`, and check a flag took effect in the configure output rather than in the YAML.
