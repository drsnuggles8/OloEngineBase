#!/usr/bin/env bash
# Root-only host steps for the self-hosted CI box that the workflows VERIFY but
# never perform (issue #1015). Companion to setup-olo-ci-runners.sh, which
# provisions the runner pool; this one owns the two things that pool did not:
#
#   1. The sanitizer runtimes for the system compiler. The box builds with
#      Rocky 10's own clang (21.1.8) -- there is no version pin any more, because
#      the hosted arm is on clang-23 and EPEL tops out at clang20, so no pin can
#      match. What the box DOES need is compiler-rt and lld beside that clang:
#      without compiler-rt every sanitizer link fails on "cannot find
#      libclang_rt.asan.a". setup-linux-build VERIFIES the runtimes and WARNS
#      (then builds anyway) when they are absent, so this step flips a warning,
#      not a failure. docs/ops/self-hosted-linux-toolchain.md says why the
#      version skew against the hosted arm is fine.
#
#   2. Unattended updates that reboot under a running job.
#      dnf-automatic-install.timer (06:00 local + up to 60 min) applies updates
#      and, with reboot=when-needed, runs `shutdown -r +5` after a kernel
#      update -- it did so on 2026-08-11, 08-14 and 08-22 (`last -x reboot`).
#      A runner that reboots itself mid-job produces failures indistinguishable
#      from flakes. Rather than disable updates, make the service SKIP a slot
#      while any runner is busy: systemd's ExecCondition runs before the
#      service body and a non-zero exit means "not now". Persistent=true on the
#      timer catches up on the next quiet slot. Runner.Worker only exists while
#      a job executes (Runner.Listener is the idle service), and pgrep is not
#      restricted to one user on purpose: gh-runner-1/2/3 serve another
#      repository on this host and their jobs deserve the same protection.
#
#      WHAT THIS DOES NOT CLOSE: the check runs once, before the update. A job
#      that starts while dnf is working is not seen, and if that update pulls a
#      kernel the `shutdown -r +5` still lands on it. The condition turns a
#      nightly collision into a rare one; it does not make the window zero.
#      Closing it needs the pool taken OUT OF ROTATION for the maintenance
#      window -- stop the Runner.Listener processes, or set the runners offline
#      through the GitHub API -- and restored afterwards. That is a deliberate
#      maintenance procedure, not something to bolt onto a timer: a listener
#      stopped by a drop-in that then fails, or a reboot that lands between the
#      stop and the restore, leaves the pool silently offline, which is a worse
#      failure than the one being fixed. Do it by hand when a big update is due.
#
#   3. The GPU must not runtime-suspend between test processes. amdgpu's
#      runtime power management (power/control=auto) puts the idle card into
#      BACO and wakes it on the next open; every GPU test process after an idle
#      gap is such a wake ("PSP is resuming... / SMU is resuming..." in the
#      journal -- 86 times on 2026-09-02 alone), and one wake failed at
#      2026-09-02 00:38:56 ("amdgpu asic init failed") and rebooted the host
#      under two running jobs. Pinning the card active removes the wake path.
#      A udev rule makes it persistent without a kernel-parameter reboot; the
#      immediate write applies it now. (`amdgpu.runpm=0` on the kernel command
#      line is the equivalent for a future reinstall.)
#
# Usage (as root, idempotent):  sudo bash scripts/setup-olo-ci-host.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root: sudo bash $0" >&2
  exit 1
fi

# ------------------------------------------------- 1. sanitizer runtimes + lld
# Asked of the compiler itself rather than hardcoded: the runtime directory
# layout is the distro's business, and it moves between clang majors.
rt_dir=$(clang++ -print-runtime-dir 2>/dev/null || true)
if [ -n "$rt_dir" ]    && ls "$rt_dir"/libclang_rt.asan*.a >/dev/null 2>&1    && ls "$rt_dir"/libclang_rt.tsan*.a >/dev/null 2>&1    && ls "$rt_dir"/libclang_rt.ubsan*.a >/dev/null 2>&1    && command -v ld.lld >/dev/null 2>&1; then
  echo "sanitizer toolchain already provisioned: $(clang++ --version | head -1)"
else
  dnf install -y clang compiler-rt lld
  rt_dir=$(clang++ -print-runtime-dir 2>/dev/null || true)
  echo "sanitizer toolchain provisioned: $(clang++ --version | head -1)"
fi
# All three runtimes or none: a compiler-rt missing tsan configures fine and
# fails only the TSan job's link, hours later and in a job nobody was watching.
for san in asan tsan ubsan; do
  ls "$rt_dir"/libclang_rt.$san*.a >/dev/null 2>&1     || { echo "compiler-rt did not install libclang_rt.$san beside clang++ (looked in '${rt_dir:-<unknown>}')" >&2; exit 1; }
done
command -v ld.lld >/dev/null 2>&1 || { echo "lld did not install ld.lld" >&2; exit 1; }

# ------------------------------------------- 2. no reboot under a running job
# Idempotent for real: write to a temp file and only install + reload when the
# content differs, so a re-run on a live host reloads nothing.
dropin=/etc/systemd/system/dnf-automatic-install.service.d/idle-runners.conf
tmp=$(mktemp)
cat > "$tmp" <<'EOF'
# Skip this update slot while any GitHub Actions job is executing on this host
# (issue #1015 item E). Runner.Worker exists only for the duration of a job.
# The timer is Persistent=true, so a skipped slot is retried at the next one.
[Service]
ExecCondition=/bin/sh -c '! pgrep -x Runner.Worker >/dev/null'
EOF
if ! cmp -s "$tmp" "$dropin" 2>/dev/null; then
  install -d -m 0755 "$(dirname "$dropin")"
  install -m 0644 "$tmp" "$dropin"
  systemctl daemon-reload
fi
rm -f "$tmp"
systemctl cat dnf-automatic-install.service | grep -q 'ExecCondition=' \
  || { echo "drop-in did not take: systemctl cat dnf-automatic-install.service" >&2; exit 1; }
echo "dnf-automatic-install.service now skips a slot while a runner job is executing"

# ------------------------------------------ 3. GPU stays awake (no runtime PM)
rule=/etc/udev/rules.d/90-olo-amdgpu-no-runpm.rules
tmp=$(mktemp)
cat > "$tmp" <<'EOF'
# Keep the AMD GPU out of runtime suspend (issue #1015 item E): a failed
# resume from BACO rebooted the host under running CI jobs on 2026-09-02.
ACTION=="add|change", SUBSYSTEM=="pci", DRIVER=="amdgpu", ATTR{power/control}="on"
EOF
if ! cmp -s "$tmp" "$rule" 2>/dev/null; then
  install -m 0644 "$tmp" "$rule"
  udevadm control --reload-rules
fi
rm -f "$tmp"
gpus=0
for dev in /sys/bus/pci/drivers/amdgpu/0000:*; do
  [ -e "$dev/power/control" ] || continue
  [ "$(cat "$dev/power/control")" = on ] || echo on > "$dev/power/control"
  gpus=$((gpus + 1))
  echo "amdgpu $(basename "$dev"): power/control=$(cat "$dev/power/control") runtime_status=$(cat "$dev/power/runtime_status")"
done
[ "$gpus" -gt 0 ] || { echo "no amdgpu PCI device found under /sys/bus/pci/drivers/amdgpu" >&2; exit 1; }

# ------------------------------------------------------------------- report
echo
echo "state:"
echo "  clang:     $(command -v clang++) -> $(clang++ --version | head -1)"
echo "  runtimes:  $(clang++ -print-runtime-dir 2>/dev/null || echo '<unknown>')"
echo "  timer:     $(systemctl list-timers dnf-automatic-install.timer --no-pager | sed -n 2p)"
echo "  condition: $(grep ExecCondition "$dropin")"
echo "  gpu:       $(for dev in /sys/bus/pci/drivers/amdgpu/0000:*; do printf '%s control=%s status=%s ' "$(basename "$dev")" "$(cat "$dev/power/control")" "$(cat "$dev/power/runtime_status")"; done)"
