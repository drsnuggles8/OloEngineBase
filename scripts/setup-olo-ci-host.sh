#!/usr/bin/env bash
# Root-only host steps for the self-hosted CI box that the workflows VERIFY but
# never perform (issues #1015 and #1036). Companion to setup-olo-ci-runners.sh,
# which provisions the runner pool; this one owns what that pool did not:
#
#   1. The sanitizer runtimes for the SYSTEM compiler (Rocky 10's clang 21.1.8).
#      That clang is no longer what CI builds with -- item 4 pins clang-23 -- but
#      it stays the fallback the workflow warns down to, and the box's default
#      for everything else. Without compiler-rt beside it every sanitizer link
#      fails on "cannot find libclang_rt.asan.a". setup-linux-build VERIFIES the
#      runtimes and WARNS (then builds anyway) when they are absent, so this step
#      flips a warning, not a failure.
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
#   4. clang-23 at /opt/llvm-23.1.0, from the official LLVM release tarball, so
#      the self-hosted sanitizer arm and its hosted twin run the same compiler
#      major (#1036). Not a package -- EPEL on Rocky 10 tops out at clang20 --
#      and deliberately not on the system PATH. Details at the step itself and
#      in docs/ops/self-hosted-linux-toolchain.md.
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
# NOT an immediate exit. Step 4 below is what the workflow's ::warning tells an
# operator to run when the CI compiler is missing, and a host with no amdgpu
# loaded -- a fresh kernel, a card pulled for testing -- would otherwise get a
# GPU error and no compiler. Record it and fail at the very end instead, so the
# script still does everything it can and the exit code still says it did not.
gpu_missing=0
if [ "$gpus" -eq 0 ]; then
  echo "no amdgpu PCI device found under /sys/bus/pci/drivers/amdgpu" >&2
  gpu_missing=1
fi

# ----------------------------------- 4. the pinned clang-23 for the CI arm
# LAST ON PURPOSE: this is the only step that needs the network and the only
# one that moves gigabytes, and `set -e` means anything after it would be
# skipped when it fails. The three host-hygiene steps above are cheap, matter
# more, and are already applied by the time this starts.
#
# WHY A TARBALL. The hosted Linux sanitizer arm builds with clang-23 from
# apt.llvm.org (#1025). Rocky 10 cannot follow by package -- EPEL tops out at
# clang20 and there is no clang22/clang23 -- and that was written down as
# making the skew permanent. It does not: LLVM publishes prebuilt release
# tarballs, so the skew was a choice. The two arms are meant to be twins;
# asan.yml routes every same-repo PR's sanitizer jobs to this box and the
# hosted arm is the tiebreaker when one goes red and the other does not, and
# while the compilers differed "or it's the compiler" was a standing
# explanation nobody could cheaply rule out.
#
# VERSIONED PREFIX, NOT ON PATH. /opt/llvm-23.1.0, referenced by absolute path
# from .github/actions/setup-linux-build. Rocky's clang 21 stays the default
# for everything else on this host -- which includes another repository's
# runners and any interactive use.
#
# THE CHECKSUM IS THE PIN. LLVM publishes no SHA256SUMS file, but it does
# publish a sigstore bundle (LLVM-23.1.0-Linux-X64.tar.xz.jsonl) whose SLSA
# in-toto statement carries the artifact digest. The value below is that
# attested digest, and it matches a sha256sum of the file as downloaded here.
llvm_version=23.1.0
llvm_prefix=/opt/llvm-$llvm_version
llvm_dirname=LLVM-$llvm_version-Linux-X64
llvm_tarball=$llvm_dirname.tar.xz
llvm_url=https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version/$llvm_tarball
llvm_sha256=18da30f77f475688a18f7704d23f9f155ae007ed9922dbed6850a9419d9fec8c

if [ -x "$llvm_prefix/bin/clang++" ] && [ "$("$llvm_prefix/bin/clang++" -dumpversion 2>/dev/null || true)" = "$llvm_version" ]; then
  echo "clang-23 already provisioned: $llvm_prefix ($("$llvm_prefix/bin/clang++" --version | head -1))"
else
  # ~2 GB compressed and ~12 GB unpacked, and both exist at once while the
  # tarball is being extracted. A prefix already at this path is ALSO still on
  # disk for that whole window -- it is only moved aside once the new tree is
  # complete, so a half-finished install never leaves the box with no compiler
  # -- so a replace needs its size on top. Check before spending twenty minutes
  # to fill a root filesystem that also carries /opt/vulkan-sdk.
  need_kb=$((16 * 1024 * 1024))
  if [ -e "$llvm_prefix" ]; then
    need_kb=$((need_kb + $(du -sk "$llvm_prefix" | awk '{ print $1 }')))
  fi
  avail_kb=$(df -Pk /opt | awk 'NR == 2 { print $4 }')
  if [ "${avail_kb:-0}" -lt "$need_kb" ]; then
    echo "not enough room on the filesystem holding /opt: $((avail_kb / 1024)) MB free, need ~$((need_kb / 1024)) MB for the download, the unpacked tree and whatever is being replaced" >&2
    exit 1
  fi

  # The staging directory is under /opt so the final install is a rename on
  # one filesystem, not a 12 GB copy that can half-finish.
  work=$(mktemp -d /opt/.llvm-install.XXXXXX)
  trap 'rm -rf "$work"' EXIT
  echo "downloading $llvm_tarball (~2 GB) ..."
  curl -fL --retry 3 --retry-delay 5 -o "$work/$llvm_tarball" "$llvm_url"
  echo "$llvm_sha256  $work/$llvm_tarball" | sha256sum -c - \
    || { echo "checksum mismatch on $llvm_tarball -- refusing to install it" >&2; exit 1; }
  # --no-same-owner: the archive records the LLVM release runner's uid/gid,
  # which means nothing here; root-owned and world-readable is what the
  # gh-runner-olo account needs.
  echo "unpacking (~12 GB, a few minutes) ..."
  tar --no-same-owner -xf "$work/$llvm_tarball" -C "$work"
  rm -f "$work/$llvm_tarball"
  test -x "$work/$llvm_dirname/bin/clang++" || { echo "$llvm_tarball did not contain $llvm_dirname/bin/clang++" >&2; exit 1; }
  chmod -R a+rX "$work/$llvm_dirname"
  if [ -e "$llvm_prefix" ]; then
    rm -rf "$llvm_prefix.replaced"
    mv "$llvm_prefix" "$llvm_prefix.replaced"
  fi
  mv "$work/$llvm_dirname" "$llvm_prefix"
  rm -rf "$llvm_prefix.replaced"
  rm -rf "$work"
  trap - EXIT
  echo "clang-23 provisioned: $llvm_prefix ($("$llvm_prefix/bin/clang++" --version | head -1))"
fi

# A VERSION BUMP CHANGES THE PATH, so the previous prefix is not replaced by the
# block above -- it is orphaned, 12 GB at a time. Name it rather than delete it:
# a branch that has not merged yet may still pin the old one, and this script
# cannot know. Removing it is a one-line manual step once nothing does.
for other in /opt/llvm-*; do
  case "$other" in
    "$llvm_prefix" | '/opt/llvm-*') continue ;;
  esac
  [ -d "$other" ] || continue
  echo "note: an older LLVM prefix is still on disk ($other, $(du -sh "$other" | awk '{ print $1 }')). Remove it once no branch pins it: rm -rf $other"
done

# THE TARBALL'S OWN ld.lld DOES NOT RUN ON ROCKY 10, and every Linux configure
# line in this repo passes -fuse-ld=lld, which clang resolves from its own bin
# directory before PATH. The release is built on an Ubuntu with ICU 70; this
# box has ICU 74, and nothing bridges an ICU soname:
#     ld.lld: error while loading shared libraries: libicui18n.so.70
# reported by clang as "unable to execute command: No such file or directory"
# at the first link rather than at configure. Point that one name at the
# system LLD instead and -fuse-ld=lld keeps working unchanged everywhere.
# The residual hosted/self-hosted skew is then the LINKER only (LLD 21 driving
# clang 23 objects), which all three sanitizers link, run and symbolise
# through. lld / lld-link / ld64.lld / wasm-ld / llvm-mt beside it are the same
# ICU-broken binary and nothing here invokes them; every other tool in the
# tarball resolves cleanly.
command -v ld.lld >/dev/null 2>&1 || { echo "no system ld.lld to point the pinned clang at" >&2; exit 1; }
system_lld=$(command -v ld.lld)
if [ "$(readlink -f "$llvm_prefix/bin/ld.lld" 2>/dev/null || true)" != "$(readlink -f "$system_lld")" ]; then
  ln -sfn "$system_lld" "$llvm_prefix/bin/ld.lld"
  echo "pointed $llvm_prefix/bin/ld.lld at $system_lld (the bundled one needs ICU 70)"
fi

# ASSERT THE INSTALL, every run, provisioned now or already there. The pin this
# one replaces named /usr/lib64/llvm19, a directory that never existed, and no
# check anywhere said so -- every self-hosted sanitizer run silently used the
# fallback for as long as it stood. So: the compiler runs, its compiler-rt is
# complete, and a C++23 program with a sanitizer actually links through the
# swapped linker and executes.
for san in asan tsan ubsan; do
  ls "$("$llvm_prefix/bin/clang++" -print-runtime-dir)"/libclang_rt.$san*.a >/dev/null 2>&1 \
    || { echo "$llvm_prefix has no libclang_rt.$san" >&2; exit 1; }
done
probe=$(mktemp -d)
cat > "$probe/probe.cpp" <<'PROBE'
// std::forward_like is the reason the hosted arm needs a non-default libstdc++
// (Core/Reflection/MemberList.h uses it); std::stacktrace is the reason a Linux
// link here needs libstdc++exp, which gcc-toolset-15 omits and base GCC 14 has.
// The tarball clang selects base GCC 14, so both must work out of the box.
#include <cstdio>
#include <stacktrace>
#include <utility>
struct S { int a; };
int main()
{
    S s{7};
    auto&& forwarded = std::forward_like<const S&>(s.a);
    return (forwarded == 7 && !std::stacktrace::current().empty()) ? 0 : 1;
}
PROBE
for san in address undefined thread; do
  # -lstdc++exp AFTER the translation unit, never before it: a static archive
  # named ahead of the object that needs it is scanned while nothing is undefined
  # yet, and the linker drops every member. `undefined symbol:
  # std::__stacktrace_impl::_S_current` is what that looks like.
  "$llvm_prefix/bin/clang++" -std=c++23 -fsanitize=$san -fuse-ld=lld \
      -o "$probe/probe" "$probe/probe.cpp" -lstdc++exp >"$probe/log" 2>&1 && "$probe/probe" \
    || { echo "the pinned clang cannot build or run a C++23 -fsanitize=$san program:" >&2; cat "$probe/log" >&2; rm -rf "$probe"; exit 1; }
done
rm -rf "$probe"
echo "pinned clang verified: C++23 + std::stacktrace + asan/ubsan/tsan all link and run"

# ------------------------------------------------------------------- report
echo
echo "state:"
echo "  ci clang:  $llvm_prefix/bin/clang++ -> $("$llvm_prefix/bin/clang++" --version | head -1)"
echo "  ci lld:    $llvm_prefix/bin/ld.lld -> $(readlink -f "$llvm_prefix/bin/ld.lld") ($(ld.lld --version))"
echo "  ci rt:     $("$llvm_prefix/bin/clang++" -print-runtime-dir)"
echo "  fallback:  $(command -v clang++) -> $(clang++ --version | head -1)"
echo "  runtimes:  $(clang++ -print-runtime-dir 2>/dev/null || echo '<unknown>')"
echo "  timer:     $(systemctl list-timers dnf-automatic-install.timer --no-pager | sed -n 2p)"
echo "  condition: $(grep ExecCondition "$dropin")"
echo "  gpu:       $(for dev in /sys/bus/pci/drivers/amdgpu/0000:*; do printf '%s control=%s status=%s ' "$(basename "$dev")" "$(cat "$dev/power/control")" "$(cat "$dev/power/runtime_status")"; done)"

# Deferred from step 3 so that a host without the GPU driver loaded still gets
# its compiler provisioned. Everything else is done; this is still a failure.
if [ "$gpu_missing" -ne 0 ]; then
  echo "FAILED: no amdgpu PCI device under /sys/bus/pci/drivers/amdgpu -- the runtime-PM pin could not be applied" >&2
  exit 1
fi
