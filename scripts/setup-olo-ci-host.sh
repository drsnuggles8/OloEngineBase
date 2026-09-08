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
#   4. clang-23 AND LLD 23 at /opt/llvm-23.1.0, from the official LLVM release
#      tarball, so the self-hosted sanitizer arm and its hosted twin run the same
#      toolchain (#1036). Not a package -- EPEL on Rocky 10 tops out at clang20 --
#      and deliberately not on the system PATH. The lld half needs ICU 70 built
#      beside it, which is most of what that step does and why it takes twenty
#      minutes cold. Details at the step itself and in
#      docs/ops/self-hosted-linux-toolchain.md.
#
#   5. A running job holds a systemd SHUTDOWN INHIBITOR (steps 3b below).
#      Item 2 guards one reboot path -- the update timer. The box was rebooted
#      from a logged-in session at 2026-09-07 06:54 UTC under a running
#      sanitizer job, and nothing stood in the way. Two runner hooks
#      (scripts/ci-host/job-started.sh, job-completed.sh) take and release a
#      `systemd-inhibit --mode=block` lock per job, so `systemctl reboot`,
#      `shutdown` and Cockpit refuse -- or warn, and need `-i` -- while a job
#      runs. A service user has no active seat, so a polkit rule grants it
#      inhibit-block-shutdown. The hooks never fail a job: a lock that cannot
#      be taken is logged and the job runs without it.
#
#   6. RUNNER MEMORY CEILINGS (step 3c). On 2026-09-07 09:01 UTC systemd-oomd
#      killed both CI runner cgroups 16 s apart under memory pressure -- two
#      sanitizer jobs and the GPU nightly's arm on 31 GiB -- and the jobs
#      surfaced as "runner lost communication". The runner units had
#      memory.max=max: no ceiling, so nothing failed BEFORE the whole slice
#      was under pressure and oomd picked the largest cgroup, which was a
#      runner mid-job, listener and all. MemoryMax= per runner unit makes the
#      kernel OOM-kill the largest process INSIDE that cgroup first -- a
#      compiler or a test, ~3 GB, not the ~100 MB listener -- and
#      OOMPolicy=continue keeps the service up when it does. The job fails
#      visibly at the step that ran out; the runner survives. NOT MemoryHigh:
#      throttling forces reclaim, reclaim is the pressure oomd measures, so a
#      MemoryHigh ceiling makes the oomd kill MORE likely.
#
# Usage (as root, idempotent):  sudo bash scripts/setup-olo-ci-host.sh
#
#   Memory ceilings are overridable per run: OLO_RUNNER_MEMORY_MAX=14G (default)
#   applies to actions-runner-ci-1/2 and actions-runner-olo alike. The report at
#   the end prints each runner cgroup's memory.peak since its last start, which
#   is the number to revise the default from.
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

# --------------------------- 3b. a running job holds a shutdown inhibitor
runner_user=gh-runner-olo
runner_home="/home/${runner_user}"
runner_uid=$(id -u "$runner_user" 2>/dev/null || true)
if [ -z "$runner_uid" ]; then
  echo "user ${runner_user} does not exist -- run scripts/setup-olo-ci-runners.sh first" >&2
  exit 1
fi
# systemctl --user for the runner user, from root. The user manager lives under
# uid 1004's session; without XDG_RUNTIME_DIR the call silently addresses the
# SYSTEM manager and reports defaults for units that are not there.
runner_systemctl() { sudo -u "$runner_user" XDG_RUNTIME_DIR="/run/user/${runner_uid}" systemctl --user "$@"; }

hookdir=/usr/local/lib/olo-ci
script_dir=$(cd "$(dirname "$0")" && pwd)
install -d -m 0755 "$hookdir"
hooks_changed=0
for h in job-started.sh job-completed.sh; do
  src="${script_dir}/ci-host/${h}"
  [ -f "$src" ] || { echo "missing ${src} -- run this script from a repository checkout" >&2; exit 1; }
  if ! cmp -s "$src" "${hookdir}/${h}"; then
    install -m 0755 "$src" "${hookdir}/${h}"
    hooks_changed=1
  fi
done

# polkit: a service user has no active seat, and inhibit-block-shutdown is
# auth_admin for inactive subjects by default. polkit reloads rules.d on change.
rule=/etc/polkit-1/rules.d/50-olo-runner-inhibit.rules
tmp=$(mktemp)
cat > "$tmp" <<EOF
// Let the CI runner user hold a BLOCK shutdown inhibitor for the duration of a
// job (scripts/ci-host/job-started.sh, installed by setup-olo-ci-host.sh).
polkit.addRule(function (action, subject) {
    if (action.id == "org.freedesktop.login1.inhibit-block-shutdown" &&
        subject.user == "${runner_user}") {
        return polkit.Result.YES;
    }
});
EOF
if ! cmp -s "$tmp" "$rule" 2>/dev/null; then
  install -d -m 0755 "$(dirname "$rule")"
  install -m 0644 "$tmp" "$rule"
fi
rm -f "$tmp"

# Point every runner's .env at the hooks. The runner reads .env once, at start,
# so a changed .env is picked up at the next restart -- done below for runners
# that are idle, deferred (and said so) for one with a job in flight.
declare -A unit_of=()
env_changed=()
for dir in "${runner_home}/actions-runner" "${runner_home}"/actions-runner-ci-*; do
  [ -x "${dir}/run.sh" ] || continue
  case "$(basename "$dir")" in
    actions-runner)      unit="actions-runner-olo.service" ;;
    actions-runner-ci-*) unit="$(basename "$dir").service" ;;
    *) continue ;;
  esac
  unit_of["$dir"]="$unit"
  envf="${dir}/.env"
  [ -f "$envf" ] || { install -o "$runner_user" -g "$runner_user" -m 600 /dev/null "$envf"; }
  changed=0
  for kv in "ACTIONS_RUNNER_HOOK_JOB_STARTED=${hookdir}/job-started.sh" \
            "ACTIONS_RUNNER_HOOK_JOB_COMPLETED=${hookdir}/job-completed.sh"; do
    key=${kv%%=*}
    if grep -q "^${key}=" "$envf"; then
      grep -qxF "$kv" "$envf" || { sed -i "s|^${key}=.*|${kv}|" "$envf"; changed=1; }
    else
      printf '%s\n' "$kv" >> "$envf"
      changed=1
    fi
  done
  [ "$changed" -eq 1 ] && env_changed+=("$dir")
done
if [ "${#unit_of[@]}" -eq 0 ]; then
  echo "no runner directories under ${runner_home} -- run scripts/setup-olo-ci-runners.sh first" >&2
  exit 1
fi
# The restart that makes a runner re-read .env happens ONCE, after step 3c has
# written the memory drop-in too, so an idle runner comes back with both.
declare -A needs_restart=()
for dir in "${env_changed[@]}"; do needs_restart["$dir"]=1; done
echo "job hooks: ${hookdir}/job-{started,completed}.sh, polkit rule ${rule}"

# ------------------------------------------------ 3c. runner memory ceilings
# One drop-in per runner unit, under /etc/systemd/user so the user manager sees
# it without touching the runner's home. See item 6 in the header for why
# MemoryMax + OOMPolicy=continue and not MemoryHigh.
#
# 14G: a 4-wide instrumented build is ~12 GB (asan.yml's own arithmetic for the
# box), and the sanitizer's llvm-symbolizer is ~0.9 GB (#1111). Two CI runners
# and the GPU runner at 14G is 42 GB against 31 GiB of RAM -- three peaks at
# once do not fit, and under that overlap the ceiling fails the JOB at the step
# that ran out instead of killing a runner. The report below prints memory.peak
# per runner, which is the measurement to revise this from.
mem_max="${OLO_RUNNER_MEMORY_MAX:-14G}"
for dir in "${!unit_of[@]}"; do
  unit="${unit_of[$dir]}"
  d="/etc/systemd/user/${unit}.d"
  tmp=$(mktemp)
  cat > "$tmp" <<EOF
# Runner memory ceiling (scripts/setup-olo-ci-host.sh, item 6). The kernel
# OOM-kills the largest process INSIDE this cgroup at the ceiling -- a compiler
# or a test -- and OOMPolicy=continue keeps the runner service up when it does.
# Without this, systemd-oomd killed whole runner cgroups under slice-wide
# pressure (2026-09-07 09:01 UTC) and jobs died as "lost communication".
[Service]
MemoryMax=${mem_max}
OOMPolicy=continue
ManagedOOMPreference=avoid
EOF
  if ! cmp -s "$tmp" "${d}/olo-memory.conf" 2>/dev/null; then
    install -d -m 0755 "$d"
    install -m 0644 "$tmp" "${d}/olo-memory.conf"
    needs_restart["$dir"]=1
  fi
  rm -f "$tmp"
done
runner_systemctl daemon-reload

# ONE apply pass for both steps. An idle unit whose .env or drop-in changed is
# restarted and comes back with the hooks AND the full drop-in. A unit with a
# job executing is not touched: the cgroup properties are pushed into it live
# with set-property --runtime, and the rest waits for its next restart.
#
# OOMPolicy is deliberately NOT in the set-property call. It is a [Service]
# execution setting, not a cgroup resource property, and set-property rejects
# it -- "Cannot set property OOMPolicy, or unknown property" -- and because one
# call is all-or-nothing, MemoryMax went unapplied with it. Measured on the box
# on the first run of this step. It comes from the drop-in, at the next start.
runner_busy() { pgrep -f "$1/bin/Runner.Worker" >/dev/null; }
for dir in "${!unit_of[@]}"; do
  unit="${unit_of[$dir]}"
  if ! runner_systemctl is-active --quiet "$unit"; then
    echo "${unit}: not active; hooks and ceiling apply when it is started"
  elif runner_busy "$dir"; then
    runner_systemctl set-property --runtime "$unit" "MemoryMax=${mem_max}" ManagedOOMPreference=avoid
    echo "${unit}: a job is executing -- MemoryMax applied live; hooks and OOMPolicy=continue apply at its next restart"
  elif [ -n "${needs_restart[$dir]:-}" ]; then
    runner_systemctl restart "$unit"
    echo "${unit}: restarted (idle) -- hooks and ceiling active"
  else
    runner_systemctl set-property --runtime "$unit" "MemoryMax=${mem_max}" ManagedOOMPreference=avoid
    echo "${unit}: unchanged; ceiling re-applied live"
  fi
done

# VERIFY from the cgroup itself, not from systemctl show: the value that
# matters is the one the kernel enforces. OOMPolicy is reported per unit
# below rather than asserted, because a busy unit legitimately carries the
# old value until it restarts.
for dir in "${!unit_of[@]}"; do
  unit="${unit_of[$dir]}"
  cg="/sys/fs/cgroup/user.slice/user-${runner_uid}.slice/user@${runner_uid}.service/app.slice/${unit}"
  if [ -r "${cg}/memory.max" ]; then
    [ "$(cat "${cg}/memory.max")" != max ] \
      || { echo "${unit}: memory.max is still 'max' after the drop-in and set-property; check ${cg}" >&2; exit 1; }
  else
    echo "${unit}: cgroup ${cg} not present (unit not running?) -- ceiling applies at its next start"
  fi
done
units_capped=""
for dir in "${!unit_of[@]}"; do units_capped="${units_capped}${units_capped:+ }${unit_of[$dir]}"; done
echo "runner memory ceiling: MemoryMax=${mem_max} OOMPolicy=continue on ${units_capped}"

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
llvm_major=23         # what the workflow's pin asserts; kept explicit, not parsed out
llvm_prefix=/opt/llvm-$llvm_version
llvm_dirname=LLVM-$llvm_version-Linux-X64
llvm_tarball=$llvm_dirname.tar.xz
llvm_url=https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version/$llvm_tarball
llvm_sha256=18da30f77f475688a18f7704d23f9f155ae007ed9922dbed6850a9419d9fec8c

# A run that died between the rename-aside and the cleanup below left the old
# tree here: 12 GB that no later run counts, removes, or even classifies right
# -- the orphan scan further down would call it "an older LLVM prefix", which it
# is not. Clear it FIRST, before the disk estimate and before either branch, so
# neither the already-provisioned path nor the install path can inherit it.
rm -rf "$llvm_prefix.replaced"

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

# THE TARBALL'S OWN lld DOES NOT START ON ROCKY 10, and that matters more than
# it looks: every Linux configure line in this repo passes -fuse-ld=lld, and
# clang resolves ld.lld from its own bin directory before PATH.
#
#     ld.lld: error while loading shared libraries: libicui18n.so.70
#
# reported by clang as "unable to execute command: No such file or directory"
# at the FIRST LINK rather than at configure, so it reads like a missing linker
# and is a missing library. The release is built on an Ubuntu carrying ICU 70;
# this box has ICU 74, and an ICU soname cannot be bridged -- the symbols are
# suffixed per major (`ucnv_open_70`), so ICU 74 exports none of what lld wants.
# libxml2 is linked statically into lld for lld-link's Windows manifests and
# drags ICU in with it; nothing else in the tarball needs it.
#
# THE FIRST FIX WAS TO POINT ld.lld AT THE SYSTEM LLD 21, and it worked for
# ELF links -- but it cost LTO, silently. LLD 21 cannot read clang 23's bitcode:
#
#     ld.lld: error: Invalid summary version 14. Version should be in the range [1-12].
#
# so check_ipo_supported() flipped to NO on the box and every configure printed
# "LTO requested but not supported". The Debug sanitizer jobs never noticed,
# because LTO is Release/Dist only -- which is exactly why that is the kind of
# thing worth removing rather than documenting. (The BFD path is not an out
# either: the tarball ships libLTO.so but no LLVMgold.so, so ld.bfd cannot load
# an LLVM plugin at all.)
#
# SO SUPPLY ICU 70 INSTEAD OF WORKING AROUND ITS ABSENCE. Three shared objects,
# built from the pinned ICU4C source release, dropped into the prefix's own lib
# directory -- where they are found with no wrapper, no patchelf, no
# LD_LIBRARY_PATH and nothing system-wide, because every binary in the tarball
# already carries RUNPATH '$ORIGIN/../lib'. Rocky's own ICU 74 is untouched and
# no other program on the host can see these.
icu_version=70.1
icu_major=70          # the soname lld records; deliberately not derived from the above
icu_tag=release-70-1
icu_srcdir=icu4c-70_1-src
icu_url=https://github.com/unicode-org/icu/releases/download/$icu_tag/$icu_srcdir.tgz
icu_sha256=8d205428c17bf13bb535300669ed28b338a157b1c01ae66d31d0d3e2d47c3fd5

# `-flavor gnu` IS NOT OPTIONAL IN THESE CHECKS. lld takes its flavour from
# argv[0]; invoked under its own name it is the generic driver, prints "lld is a
# generic driver. Invoke ld.lld (Unix), ... instead" and exits 1 -- whether or not
# it is healthy. A guard written as `lld --version` therefore calls a perfectly
# good linker broken every single time, and wrongly in both directions: it
# rebuilds ICU on every run and then declares the result a failure. Asking for
# the GNU flavour explicitly is the same question without the argv[0] trap.
if ! "$llvm_prefix/bin/lld" -flavor gnu --version >/dev/null 2>&1; then
  echo "building ICU $icu_version so the pinned lld can start (a few minutes) ..."
  icu_work=$(mktemp -d)
  trap 'rm -rf "$icu_work"' EXIT
  curl -fL --retry 3 --retry-delay 5 -o "$icu_work/src.tgz" "$icu_url"
  echo "$icu_sha256  $icu_work/src.tgz" | sha256sum -c - \
    || { echo "checksum mismatch on $icu_srcdir.tgz -- refusing to build it" >&2; exit 1; }
  tar --no-same-owner -xf "$icu_work/src.tgz" -C "$icu_work"
  (
    cd "$icu_work/icu/source"
    # --disable-tools prints "This ICU cannot build its own data. Expect build
    # failures in the 'data' directory" and then builds fine anyway, because the
    # source release ships icudt70l.dat prebuilt. Verified on the pinned version;
    # re-check the note if that version ever moves.
    #
    # -j4, not -jnproc: this box runs CI, and a provisioning step is not worth
    # starving two in-flight sanitizer jobs of the machine.
    ./configure --prefix="$icu_work/out" --enable-static=no \
                --disable-tests --disable-samples --disable-extras --disable-tools >configure.log 2>&1 \
      || { echo "ICU configure failed:" >&2; tail -20 configure.log >&2; exit 1; }
    make -j4 >build.log 2>&1 && make install >>build.log 2>&1 \
      || { echo "ICU build failed:" >&2; tail -20 build.log >&2; exit 1; }
  )
  # Only the three lld actually needs -- 6.3 MB in total, of which libicudata is
  # a 9 KB STUB, because --disable-tools builds no ICU data. That is correct
  # here: lld touches ICU only through libxml2's encoding conversion for
  # lld-link's Windows manifests, which no ELF link reaches, and the converters
  # it would use are code rather than data. Verified by linking plain, LTO,
  # ThinLTO, ASan, TSan and UBSan through it. If a complete ICU is ever wanted
  # here, drop --disable-tools and expect ~30 MB and a few more minutes.
  #
  # cp -a keeps the .so.70 -> .so.70.1 symlink, which is what the soname
  # recorded in lld resolves to.
  for lib in icuuc icui18n icudata; do
    cp -a "$icu_work/out/lib/lib$lib.so.$icu_version" "$icu_work/out/lib/lib$lib.so.$icu_major" "$llvm_prefix/lib/" \
      || { echo "ICU built but lib$lib.so.$icu_major is not where it was expected" >&2; exit 1; }
  done
  rm -rf "$icu_work"
  trap - EXIT
  "$llvm_prefix/bin/lld" -flavor gnu --version >/dev/null 2>&1 \
    || { echo "$llvm_prefix/bin/lld still does not start after installing ICU $icu_version; unresolved libraries:" >&2; ldd "$llvm_prefix/bin/lld" | grep 'not found' >&2; exit 1; }
  echo "ICU $icu_version installed into $llvm_prefix/lib; the pinned lld starts"
fi

# ld.lld IS THE TARBALL'S OWN AGAIN, restoring it if an earlier run of this
# script pointed it at the system LLD. It is a symlink to `lld` in the tarball,
# and lld picks its flavour from argv[0], so the name is the whole mechanism.
# A run that cannot make the bundled one start keeps the system LLD 21 rather
# than leaving the box with no linker -- ELF links all work through it; only
# LTO does not, which is the state this section used to ship.
if "$llvm_prefix/bin/lld" -flavor gnu --version >/dev/null 2>&1; then
  if [ "$(readlink "$llvm_prefix/bin/ld.lld" 2>/dev/null || true)" != "lld" ]; then
    ln -sfn lld "$llvm_prefix/bin/ld.lld"
    echo "restored $llvm_prefix/bin/ld.lld to the tarball's own LLD"
  fi
  # Under the name ld.lld the flavour comes from argv[0] again, so this both
  # restates the result and checks the symlink points where it should.
  case "$("$llvm_prefix/bin/ld.lld" --version)" in
    "LLD $llvm_major"*) : ;;
    *) echo "$llvm_prefix/bin/ld.lld reports '$("$llvm_prefix/bin/ld.lld" --version)', not LLD $llvm_major" >&2; exit 1 ;;
  esac
else
  command -v ld.lld >/dev/null 2>&1 || { echo "the pinned lld does not start and there is no system ld.lld to fall back to" >&2; exit 1; }
  system_lld=$(command -v ld.lld)
  if [ "$(readlink -f "$llvm_prefix/bin/ld.lld" 2>/dev/null || true)" != "$(readlink -f "$system_lld")" ]; then
    ln -sfn "$system_lld" "$llvm_prefix/bin/ld.lld"
  fi
  echo "WARNING: $llvm_prefix/bin/lld does not start, so ld.lld points at $system_lld (LLD 21). ELF links work; LTO does not -- CMake will report 'IPO/LTO supported by this toolchain: NO'." >&2
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
for mode in -fsanitize=address -fsanitize=undefined -fsanitize=thread -flto; do
  # -lstdc++exp AFTER the translation unit, never before it: a static archive
  # named ahead of the object that needs it is scanned while nothing is undefined
  # yet, and the linker drops every member. `undefined symbol:
  # std::__stacktrace_impl::_S_current` is what that looks like.
  #
  # -flto is in this list because it is the ONE mode that tells the tarball's own
  # LLD 23 apart from the system LLD 21 fallback. LLD 21 links every case above
  # perfectly well and then fails bitcode with "Invalid summary version 14" --
  # which no Debug CI job here would ever notice, because LTO is Release/Dist only.
  "$llvm_prefix/bin/clang++" -std=c++23 $mode -fuse-ld=lld \
      -o "$probe/probe" "$probe/probe.cpp" -lstdc++exp >"$probe/log" 2>&1 && "$probe/probe" \
    || { echo "the pinned clang cannot build or run a C++23 $mode program:" >&2; cat "$probe/log" >&2; rm -rf "$probe"; exit 1; }
done
rm -rf "$probe"
echo "pinned clang verified: C++23 + std::stacktrace + asan/ubsan/tsan + LTO all link and run"

# ------------------------------------------------------------------- report
echo
echo "state:"
echo "  ci clang:  $llvm_prefix/bin/clang++ -> $("$llvm_prefix/bin/clang++" --version | head -1)"
echo "  ci lld:    $llvm_prefix/bin/ld.lld -> $(readlink -f "$llvm_prefix/bin/ld.lld") ($("$llvm_prefix/bin/ld.lld" --version))"
echo "  ci icu:    $(ls "$llvm_prefix"/lib/libicuuc.so.* 2>/dev/null | tr '\n' ' ' || echo '<none — lld is the system LLD>')"
echo "  ci rt:     $("$llvm_prefix/bin/clang++" -print-runtime-dir)"
echo "  fallback:  $(command -v clang++) -> $(clang++ --version | head -1)"
echo "  runtimes:  $(clang++ -print-runtime-dir 2>/dev/null || echo '<unknown>')"
echo "  timer:     $(systemctl list-timers dnf-automatic-install.timer --no-pager | sed -n 2p)"
echo "  condition: $(grep ExecCondition "$dropin")"
echo "  inhibitor: $(systemd-inhibit --list --no-legend 2>/dev/null | grep -c 'actions-runner' | sed 's/^0$/none held (no job executing)/; t; s/$/ job lock(s) held/')"
for dir in "${!unit_of[@]}"; do
  unit="${unit_of[$dir]}"
  cg="/sys/fs/cgroup/user.slice/user-${runner_uid}.slice/user@${runner_uid}.service/app.slice/${unit}"
  if [ -r "${cg}/memory.max" ]; then
    printf '  %-32s max=%s current=%sMiB peak=%sMiB oompolicy=%s\n' "$unit" "$(cat "${cg}/memory.max")" \
      "$(( $(cat "${cg}/memory.current") / 1048576 ))" "$(( $(cat "${cg}/memory.peak" 2>/dev/null || echo 0) / 1048576 ))" \
      "$(runner_systemctl show -p OOMPolicy --value "$unit" 2>/dev/null || echo '?')"
  else
    printf '  %-32s (not running)\n' "$unit"
  fi
done
echo "  gpu:       $(for dev in /sys/bus/pci/drivers/amdgpu/0000:*; do printf '%s control=%s status=%s ' "$(basename "$dev")" "$(cat "$dev/power/control")" "$(cat "$dev/power/runtime_status")"; done)"

# Deferred from step 3 so that a host without the GPU driver loaded still gets
# its compiler provisioned. Everything else is done; this is still a failure.
if [ "$gpu_missing" -ne 0 ]; then
  echo "FAILED: no amdgpu PCI device under /sys/bus/pci/drivers/amdgpu -- the runtime-PM pin could not be applied" >&2
  exit 1
fi
