#!/usr/bin/env bash
#
# Provision the Rocky host to run the Linux CI jobs (not just the GPU nightly),
# and register the extra runner instances they need. Issue #1009.
#
# Run as root, with a freshly minted registration token:
#
#   sudo bash scripts/setup-olo-ci-runners.sh \
#     "$(gh api -X POST repos/drsnuggles8/OloEngineBase/actions/runners/registration-token --jq .token)"
#
# Idempotent: safe to re-run. Re-running with a fresh token re-registers the
# runners in place (--replace).
#
# WHAT THIS IS NOT
# ----------------
# It does not touch the existing `olo-gpu-amd` runner, and the runners it adds do
# NOT carry the `gpu-amd` label. That separation is deliberate: the GPU nightly
# must never queue behind a CI job, and a CI job must never be scheduled onto the
# one runner that owns the GPU. Labels are the whole mechanism, so do not "tidy"
# them into one set.
#
# See docs/ops/self-hosted-gpu-runner.md.

set -euo pipefail

REG_TOKEN="${1:-}"
RUNNER_USER=gh-runner-olo
RUNNER_HOME="/home/${RUNNER_USER}"
RUNNER_TARBALL="${RUNNER_TARBALL:-/home/obueker/actions-runner-linux-x64-2.336.0.tar.gz}"
REPO_URL="https://github.com/drsnuggles8/OloEngineBase"
# Two, not more. See "Why two" below.
CI_RUNNER_COUNT="${CI_RUNNER_COUNT:-2}"

if [ "$(id -u)" -ne 0 ]; then
    echo "error: run as root (sudo bash $0 <token>)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. Packages
#
# The host was provisioned for gpu-conformance-amd, which builds with GCC and
# takes its shader toolchain from the LunarG SDK. The Linux CI jobs need three
# more things, and each has a specific reason:
#
#   clang / lld / compiler-rt   the sanitizer jobs are clang-only, and
#                               -fsanitize=address|undefined|thread needs the
#                               compiler-rt runtimes. Rocky 10 ships clang 21;
#                               the hosted jobs pin clang-19. That version skew
#                               is accepted deliberately (#1009) -- it is extra
#                               compiler coverage, not a defect -- but it does
#                               mean a diagnostic can appear here and not on the
#                               hosted fallback.
#   mesa-libEGL-devel           GLFW compiles its EGL backend unconditionally.
#   nasm                        FFmpeg's from-source build. Cheap to install and
#                               it removes a whole class of "why did this job
#                               fail only here" later.
#
# The shader libraries (shaderc / glslang / SPIRV-Tools / SPIRV-Cross) are NOT
# installed from dnf: Rocky does not package them, which is exactly why the
# Vulkan SDK is on this box. CMAKE_PREFIX_PATH="$VULKAN_SDK" resolves all four,
# the same way gpu-conformance-amd.yml already does it.
# ---------------------------------------------------------------------------
echo "== packages =="
dnf install -y \
    clang clang-tools-extra lld compiler-rt libomp libomp-devel \
    mesa-libEGL-devel libglvnd-devel \
    libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel \
    wayland-devel wayland-protocols-devel libxkbcommon-devel \
    nasm \
    autoconf autoconf-archive automake libtool pkgconf-pkg-config \
    curl zip unzip tar python3-jinja2

# ---------------------------------------------------------------------------
# 2. Persistent caches, owned by the runner user
#
# This is the point of moving the jobs here. On a hosted runner every cache is a
# round trip through the Actions cache service: an upload, a download, a slice of
# the repo's single 10 GB cap, and -- the part that actually bit -- a post-job
# save step that a cancelled run skips. On local disk none of that exists. The
# archive is written when the port is built and it is simply still there.
#
# Everything lives under $RUNNER_HOME/.cache/olo so one `rm -rf` resets the lot,
# and NOTHING is shared with gh-runner-1/2/3, which serve a private repository at
# a different trust level (see docs/ops/self-hosted-gpu-runner.md §2).
# ---------------------------------------------------------------------------
echo "== caches =="
# THE PARENTS ARE CREATED AND CHOWNED EXPLICITLY, and that is not tidiness.
# `install -d -o user a/b/c` applies -o/-g/-m to the FINAL component only; the
# intermediate directories it creates along the way are left owned by the
# invoking user, i.e. root, mode 0755. The runner user can then traverse
# ~/.cache/olo but cannot create anything in it -- so the three directories below
# work and anything the workflow makes later does not. Measured, not theorised:
# the first self-hosted run died with
#
#   fatal: could not create work tree dir
#   '/home/gh-runner-olo/.cache/olo/vcpkg-olo-ci-2': Permission denied
#
# on the per-runner vcpkg clone, four minutes in, with three sibling directories
# sitting there correctly owned. The chown is separate from the install so
# re-running this script REPAIRS a tree left behind by the buggy version rather
# than skipping it as already-present.
for d in "" "/olo"; do
    install -o "$RUNNER_USER" -g "$RUNNER_USER" -m 755 -d "${RUNNER_HOME}/.cache${d}"
    chown "${RUNNER_USER}:${RUNNER_USER}" "${RUNNER_HOME}/.cache${d}"
done
for d in ccache vcpkg-binary-cache cpm; do
    install -o "$RUNNER_USER" -g "$RUNNER_USER" -m 755 -d "${RUNNER_HOME}/.cache/olo/${d}"
    chown -R "${RUNNER_USER}:${RUNNER_USER}" "${RUNNER_HOME}/.cache/olo/${d}"
done
# ONE ccache directory shared by every job, not one per sanitizer: ccache hashes
# the full compile command line, so -fsanitize=address and -fsanitize=thread
# objects cannot collide, and a single large LRU beats several fixed-size ones.
#
# ccache and not sccache, and that is the load-bearing choice once the box has
# more than one runner: sccache is a per-USER DAEMON whose environment is fixed by
# whichever client happens to start it, so a second concurrent job's SCCACHE_DIR
# is silently ignored. ccache is a process per compiler invocation, with an
# on-disk format built for concurrent writers. See
# .github/actions/setup-linux-build.
#
# 30 GiB against 233 GiB free is generous and still bounded, so a runaway cannot
# fill /home.
sudo -u "$RUNNER_USER" env CCACHE_DIR="${RUNNER_HOME}/.cache/olo/ccache" \
    ccache --max-size=30G >/dev/null
echo "  ${RUNNER_HOME}/.cache/olo/{ccache,vcpkg-binary-cache,cpm}"
echo "  (each runner also keeps its own vcpkg clone at .cache/olo/vcpkg-<runner-name>)"

# ---------------------------------------------------------------------------
# 3. Extra runner instances
#
# WHY TWO, and why not more. The box has 31 GiB and 8C/16T, and it is NOT idle:
# it also hosts gh-runner-1/2/3 for a private repository.
#
#   * A Linux sanitizer build is the heaviest thing that will land here. clang++
#     under instrumentation takes ~3 GB per TU, the jobs are pinned to
#     --parallel 2, and the link is the real spike (which is why they use lld and
#     OLO_LINK_JOBS). Budget ~9 GiB per concurrent job, worst case.
#   * Reserve ~4 GiB for the OS and page cache. That leaves ~27 GiB, i.e. three
#     concurrent jobs at the ceiling.
#   * One of those three slots is already spoken for by olo-gpu-amd. So: two.
#
# Three CI runners would fit only if nothing else on the box ever ran at the same
# time, and the whole reason gpu-conformance-amd.yml caps itself at
# CMAKE_BUILD_PARALLEL_LEVEL=6 is that "normally idle" is not "always idle".
# Every uncertainty here answers with the smaller number: a wrong low guess costs
# a queue wait, a wrong high one costs an OOM that kills somebody's job.
#
# Labels: `olo-ci`, deliberately WITHOUT `gpu-amd`. The GPU nightly requests
# gpu-amd and so can only ever land on olo-gpu-amd; the CI jobs request olo-ci
# and can only ever land here. Neither can starve the other. That is the fix for
# "a single runner serialises every job behind the nightly" in #1009.
# ---------------------------------------------------------------------------
echo "== runners =="
if [ -z "$REG_TOKEN" ]; then
    echo "no registration token given -- packages and caches are done, runners skipped." >&2
    echo "re-run with:  sudo bash $0 \"\$(gh api -X POST repos/drsnuggles8/OloEngineBase/actions/runners/registration-token --jq .token)\"" >&2
    exit 0
fi

if [ ! -f "$RUNNER_TARBALL" ]; then
    echo "error: runner tarball not found at $RUNNER_TARBALL" >&2
    echo "       set RUNNER_TARBALL=/path/to/actions-runner-linux-x64-*.tar.gz" >&2
    exit 1
fi

for i in $(seq 1 "$CI_RUNNER_COUNT"); do
    name="olo-ci-${i}"
    dir="${RUNNER_HOME}/actions-runner-ci-${i}"
    unit="actions-runner-ci-${i}.service"

    echo "-- ${name} (${dir})"

    # Extract as ROOT then hand ownership over. /home/obueker is mode 0700, so
    # gh-runner-olo cannot traverse into it and `tar` would fail with "Cannot
    # open: Permission denied" before reading a byte. Same trap as the Vulkan SDK
    # and the original runner install -- see the ops doc.
    install -o "$RUNNER_USER" -g "$RUNNER_USER" -m 700 -d "$dir"
    if [ ! -x "${dir}/config.sh" ]; then
        tar xzf "$RUNNER_TARBALL" -C "$dir"
        chown -R "${RUNNER_USER}:${RUNNER_USER}" "$dir"
    fi

    # VULKAN_SDK has to be in each runner's own .env: the workflow reads it to
    # find shaderc/glslang/SPIRV-Cross, and a runner process inherits nothing
    # from an interactive login. Copy whatever olo-gpu-amd already uses so the
    # two cannot drift.
    src_env="${RUNNER_HOME}/actions-runner/.env"
    if [ -f "$src_env" ]; then
        install -o "$RUNNER_USER" -g "$RUNNER_USER" -m 600 "$src_env" "${dir}/.env"
    else
        echo "warning: ${src_env} not found; VULKAN_SDK may be unset for ${name}" >&2
    fi

    sudo -u "$RUNNER_USER" -- "${dir}/config.sh" \
        --url "$REPO_URL" \
        --token "$REG_TOKEN" \
        --name "$name" \
        --labels self-hosted,linux,x64,olo-ci \
        --work _work \
        --unattended --replace

    # User-level systemd with lingering, matching the host's existing runners
    # rather than installing a system unit with svc.sh.
    install -o "$RUNNER_USER" -g "$RUNNER_USER" -m 755 -d "${RUNNER_HOME}/.config/systemd/user"
    cat > "${RUNNER_HOME}/.config/systemd/user/${unit}" <<UNIT
[Unit]
Description=GitHub Actions runner ${name} (OloEngineBase CI)
After=network-online.target

[Service]
ExecStart=${dir}/run.sh
WorkingDirectory=${dir}
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
UNIT
    chown "${RUNNER_USER}:${RUNNER_USER}" "${RUNNER_HOME}/.config/systemd/user/${unit}"
done

loginctl enable-linger "$RUNNER_USER"
uid=$(id -u "$RUNNER_USER")
for i in $(seq 1 "$CI_RUNNER_COUNT"); do
    sudo -u "$RUNNER_USER" XDG_RUNTIME_DIR="/run/user/${uid}" \
        systemctl --user daemon-reload
    sudo -u "$RUNNER_USER" XDG_RUNTIME_DIR="/run/user/${uid}" \
        systemctl --user enable --now "actions-runner-ci-${i}.service"
done

echo
echo "done. verify with:"
echo "  gh api repos/drsnuggles8/OloEngineBase/actions/runners --jq '.runners[]|{name,status,busy,labels:[.labels[].name]}'"
