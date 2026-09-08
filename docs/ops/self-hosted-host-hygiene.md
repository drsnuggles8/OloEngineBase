# Self-hosted box: host hygiene

Rules for the host behind the `olo-*` runners, and the evidence for each. The runner
provisioning itself is in [self-hosted-gpu-runner.md](self-hosted-gpu-runner.md); the
root-only steps below are applied by
[`scripts/setup-olo-ci-host.sh`](../../scripts/setup-olo-ci-host.sh). Issue
[#1015](https://github.com/drsnuggles8/OloEngineBase/issues/1015), item E, plus the pinned
compiler from [#1036](https://github.com/drsnuggles8/OloEngineBase/issues/1036).

## 1. Unattended updates must not reboot under a running job

`dnf-automatic-install.timer` is enabled (`OnCalendar=06:00` local, `RandomizedDelaySec=60m`,
`Persistent=true`) and `/etc/dnf/automatic.conf` has `reboot = when-needed` with
`shutdown -r +5`. So a kernel update reboots the box between 06:05 and 07:05 local. It did on
2026-08-11 06:50, 08-14 06:28 and 08-22 06:19 (`last -x reboot`; each boot carries the new
kernel). Any job running then is CANCELLED with no error of its own.

The fix keeps the updates and removes the collision: a drop-in on
`dnf-automatic-install.service` adds

```
ExecCondition=/bin/sh -c '! pgrep -x Runner.Worker >/dev/null'
```

`Runner.Worker` exists only while a job executes (`Runner.Listener` is the idle service), so a
busy slot is skipped and `Persistent=true` retries at the next one. It is not scoped to the
`gh-runner-olo` user on purpose: `gh-runner-1/2/3` serve another repository on this host.

**It narrows the window, it does not close it.** The condition is evaluated once, before the
update runs. A job picked up while `dnf` is working is not seen, and a kernel update's
`shutdown -r +5` will still land on it. To make a planned update safe, take the pool out of
rotation first — stop the `Runner.Listener` processes, or mark the runners offline through the
GitHub API — and restore it afterwards. That stays a manual procedure on purpose: a drop-in
that stops listeners and then fails, or a reboot between the stop and the restore, leaves the
pool quietly offline, which is worse than the collision it was meant to prevent.

**What the timer did NOT do:** reboot the box at 2026-09-02 00:39 under two sanitizer jobs.
That claim is in commit `b704e7810`, issue #1015 and the Troubleshooting row of the runner
doc, and the journal contradicts it: `dnf-automatic-install.service` ran at 09-01 06:27 and
09-02 06:20, both "No security updates needed"; the 00:39 reboot was the GPU (§2). The timer's
slot is 06:00, not midnight.

## 2. The GPU must stay awake: a failed runtime-PM resume rebooted the host

`/sys/class/drm/card0/device/power/control` is `auto`, so amdgpu puts the idle card into
runtime suspend (BACO) and wakes it on the next open. Every GPU test process that starts after
an idle gap is such a wake, and each one logs the full bring-up (`PCIE GART ... enabled`,
`PSP is resuming...`, `SMU is resuming...`, the ring list): 4 to 12 per conformance nightly
between 08-20 and 09-01, 86 on 2026-09-02 while #1008 dispatched GPU runs all day. These are
not hang recoveries — there is no `GPU reset` or ring-timeout line anywhere near them.

Under the ASan sanitizer run of 2026-09-01 (run 33561256256) the test step started at
00:34:43 local; the card woke at 00:34:46, 00:35:18, 00:36:06 and 00:38:29, and the wake at
00:38:56 logged `atombios stuck in loop`, `atombios stuck executing 937E` and
`amdgpu asic init failed`. The journal ends there; the host was back at 00:39:31 with no
shutdown record, which took both `olo-ci` runners down mid-job ("The self-hosted runner lost
communication with the server" at 00:48).

The fix is to pin the card active: `power/control=on`, made persistent by the udev rule
`scripts/setup-olo-ci-host.sh` installs (a kernel-parameter `amdgpu.runpm=0` needs a
reboot; the rule and the immediate sysfs write do not). The card then never enters the path
that failed. The cost is idle power on a box that is a server anyway.

Read `journalctl -b -1` before blaming the update timer or the network for a job that
vanished: `journalctl --list-boots` shows whether the box rebooted, and the previous boot's
last lines show why. A GPU-induced reboot kills `gh-runner-1/2/3`'s jobs and the media server
too, which is why [gpu-sanitizers-amd.yml](../../.github/workflows/gpu-sanitizers-amd.yml)
is scheduled when nothing else runs.

## 3. The runners share one host, one GPU and 31 GiB

`olo-gpu-amd`, `olo-ci-1` and `olo-ci-2` all run as `gh-runner-olo`, which is in `video` and
`render`, so all three can open `/dev/dri/renderD128`. Labels, not hardware, keep CI off the
GPU: a CI job that passes `--olo-gl-backend=none` (the sanitizer jobs) touches it never; one
that passes `--olo-gl-backend=egl` (the GPU jobs) shares it with whatever else is running.
Memory is the other shared budget: an instrumented compile is ~3 GB per translation unit and
an instrumented `OloEngine-Tests` link several more, so every self-hosted build caps its
parallelism and sets `OLO_LINK_JOBS=1`; two sanitizer jobs at once already use most of the box.

## 4. The CI compiler is provisioned; the workflow verifies it and never installs

See [self-hosted-linux-toolchain.md](self-hosted-linux-toolchain.md). The short version: CI
builds with **clang-23 from an LLVM release tarball at `/opt/llvm-23.1.0`**, so the two Linux
sanitizer arms run the same compiler major; Rocky's system clang 21 stays the host default and
is the warn-and-fall-back path. The prefix is ~12 GB, so leave ~16 GB free on `/` before
running the script.

One detail worth knowing before it confuses a link failure: the tarball's `lld` is linked
against ICU 70 and this box has ICU 74, so it will not start until the script builds ICU 70
into `/opt/llvm-23.1.0/lib` (found there by the tarball's own `$ORIGIN/../lib` RUNPATH, not by
anything system-wide). If those three `libicu*.so.70` files go missing, `lld` stops starting and
the script falls the box back to the system LLD 21 — which links everything except LTO.

The two missing-piece outcomes are NOT the same, which matters when you read a red job:

- **`lld` missing — the job FAILS.** `lld` is in the "Preflight — toolchain" step's required
  binary list, alongside `cmake`, `ninja`, `ccache`, `clang`, `clang++`, `python3` and `nasm`.
  A missing one is an `::error` and `exit 1` before anything is configured.
- **`compiler-rt` missing — the job WARNS and carries on.** The runtime check in "Resolve
  toolchain" emits a `::warning` naming the `dnf` command. The build then fails later at the
  first sanitizer link (`cannot find libclang_rt.asan.a`), so nothing is silently skipped —
  the warning just tells you why, several minutes earlier than the linker would.

## 5. A running job holds a shutdown inhibitor

**Rule: while a job executes on any runner, the host refuses to reboot; override it knowingly
with `-i`, never by accident.** Section 1 guards one path -- the update timer. On 2026-09-07 at
06:54 UTC the box was rebooted from a logged-in session under a running sanitizer job, and
nothing stood in the way; `olo-ci-2` stayed down for the rest of the day.

Two runner hooks, [`scripts/ci-host/job-started.sh`](../../scripts/ci-host/job-started.sh) and
`job-completed.sh`, take and release a `systemd-inhibit --what=shutdown --mode=block` lock per
job. `systemctl reboot` / `poweroff` and `shutdown` then refuse with *"Operation inhibited by
actions-runner olo-ci-1 ..."* and Cockpit shows the same; `systemctl reboot -i` overrides. The
runner user has no active seat, so a polkit rule grants it `inhibit-block-shutdown`;
`setup-olo-ci-host.sh` installs the hooks to `/usr/local/lib/olo-ci/`, the rule, and the two
`ACTIONS_RUNNER_HOOK_JOB_*` lines in each runner's `.env`. A hook never fails a job: a lock it
cannot take is logged in the job's output and the job runs without it.

Check it during any job: `systemd-inhibit --list` names the runner and the run id. A stale lock
(runner crashed between hooks) is released by the next job's started hook.

## 6. Runner memory ceilings: a job that runs out fails the job, not the runner

**Rule: every runner unit carries `MemoryMax=` and `OOMPolicy=continue`; a runner cgroup at
`memory.max=max` is a bug, not a default.** On 2026-09-07 at 09:01 UTC `systemd-oomd` killed
both CI runner cgroups 16 s apart -- two sanitizer jobs plus the GPU nightly's arm on 31 GiB.
A whole cgroup goes at once, listener included, and the jobs surface on GitHub as *"The
self-hosted runner lost communication with the server"* with zero steps run.

With a ceiling, the kernel OOM-kills the largest process *inside* the runner's cgroup first --
a compiler or a test process, a few GB -- long before slice-wide pressure reaches oomd's
threshold. `OOMPolicy=continue` keeps the service up when that happens (the default `stop`
would take the runner down on every in-job OOM, which is the failure being fixed). The job
fails visibly at the step that ran out. **Not `MemoryHigh`:** throttling works by forcing
reclaim, reclaim is the pressure oomd measures, so a `MemoryHigh` ceiling makes the oomd kill
*more* likely.

The default is 14G per runner (`OLO_RUNNER_MEMORY_MAX=` overrides): a 4-wide instrumented
build is ~12 GB and the sanitizer's symbolizer ~0.9 GB. Two CI runners and the GPU runner at
14G is 42 GB against 31 GiB -- three peaks at once do not fit, and under that overlap the
ceiling turns a runner kill into a red job. The script's report prints each runner cgroup's
`memory.peak` since its last start; revise the default from that number, not from a guess.

## Root steps, once

```
sudo bash scripts/setup-olo-ci-host.sh
```

Idempotent, and safe to re-run: everything it does is checked first and skipped when already
in place. It installs the sanitizer runtimes and `lld` for the system clang, the update-timer
drop-in, the GPU runtime-PM rule, the job-inhibitor hooks and their polkit rule, the runner
memory ceilings, and the pinned clang-23 at `/opt/llvm-23.1.0`, then prints the resulting
state -- including whether a job lock is held right now and each runner cgroup's `memory.peak`. The compiler step is **last on purpose** -- it is the only one that needs the
network and moves gigabytes, and under `set -e` anything after it would be skipped when a
download fails. Budget ~20 minutes and ~16 GB of free space on the first run; a re-run with the
prefix already present only re-verifies, which takes seconds.

Nothing in any workflow installs or changes host state; a self-hosted step verifies and names
this script when it finds something missing.
