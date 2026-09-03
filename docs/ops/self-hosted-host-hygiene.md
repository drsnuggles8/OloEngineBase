# Self-hosted box: host hygiene

Rules for the host behind the `olo-*` runners, and the evidence for each. The runner
provisioning itself is in [self-hosted-gpu-runner.md](self-hosted-gpu-runner.md); the
root-only steps below are applied by
[`scripts/setup-olo-ci-host.sh`](../../scripts/setup-olo-ci-host.sh). Issue
[#1015](https://github.com/drsnuggles8/OloEngineBase/issues/1015), item E.

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

## 4. The compiler is pinned by provisioning, verified by the workflow

See [self-hosted-linux-toolchain.md](self-hosted-linux-toolchain.md). The short version: the
job warns and builds with the system clang 21 until `clang19` from EPEL is installed, and the
warning is the only thing the install changes.

## Root steps, once

```
sudo bash scripts/setup-olo-ci-host.sh
```

Idempotent. Installs the pinned compiler, the update-timer drop-in and the GPU runtime-PM
rule, then prints the resulting state. Nothing in any workflow installs or changes host state; a self-hosted step
verifies and names this script when it finds something missing.
