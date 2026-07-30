#!/usr/bin/env bash
#
# Register the self-hosted GitHub Actions runner for OloEngineBase.
# Run as root:   sudo bash setup-olo-runner.sh "<registration-token>"
#
# Idempotent: safe to re-run. See docs/ops/self-hosted-gpu-runner.md.
set -euo pipefail

# Host-specific paths; override via the environment if your box differs.
RUNNER_USER=${RUNNER_USER:-gh-runner-olo}
RUNNER_HOME=/home/$RUNNER_USER
RUNNER_DIR=$RUNNER_HOME/actions-runner
TARBALL=${TARBALL:-/home/obueker/actions-runner-linux-x64-2.336.0.tar.gz}
REPO_URL=${REPO_URL:-https://github.com/drsnuggles8/OloEngineBase}
RUNNER_NAME=${RUNNER_NAME:-olo-gpu-amd}
LABELS=${LABELS:-self-hosted,linux,x64,gpu-amd}
SDK_SRC=${SDK_SRC:-/home/obueker/vulkan-sdk/1.4.350.1}
SDK_DST=${SDK_DST:-/opt/vulkan-sdk/1.4.350.1}
UNIT=${UNIT:-actions-runner-olo}

TOKEN="${1:-}"
[ -n "$TOKEN" ] || { echo "usage: sudo bash $0 <registration-token>" >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 2; }
[ -f "$TARBALL" ] || { echo "runner tarball not found: $TARBALL" >&2; exit 1; }

say() { printf '\n=== %s ===\n' "$*"; }

# --------------------------------------------------------------------------
# 1. Dedicated unprivileged user, isolated from gh-runner-1/2/3.
#    Those serve a PRIVATE repo; this one serves a PUBLIC repo, which is a
#    different trust level. No shared home, no shared cache, and deliberately
#    NOT in the wheel group.
# --------------------------------------------------------------------------
say "user $RUNNER_USER"
if id -u "$RUNNER_USER" >/dev/null 2>&1; then
    echo "already exists"
else
    useradd -m -s /bin/bash "$RUNNER_USER"
    echo "created"
fi
# GPU access. /dev/dri/renderD128 is currently mode 0666 so this is
# belt-and-braces, but distro defaults change.
usermod -aG render,video "$RUNNER_USER"
chmod 700 "$RUNNER_HOME"
id "$RUNNER_USER"

# --------------------------------------------------------------------------
# 2. Vulkan SDK -> /opt.
#    The engine needs shaderc/glslang/SPIRV-Tools/SPIRV-Cross, not just the
#    loader, and CMake FATAL_ERRORs on the first one missing. The SDK currently
#    sits under /home/obueker, which is mode 0700 — the runner user cannot even
#    traverse into it, so a copy in /opt is required, not just convenient.
#    Copied (not moved) so the existing build-linux tree keeps working.
# --------------------------------------------------------------------------
say "Vulkan SDK -> $SDK_DST"
if [ -d "$SDK_DST/x86_64" ]; then
    echo "already present"
else
    [ -d "$SDK_SRC" ] || { echo "source SDK missing: $SDK_SRC" >&2; exit 1; }
    mkdir -p /opt/vulkan-sdk
    cp -a "$SDK_SRC" /opt/vulkan-sdk/
    echo "copied ($(du -sh "$SDK_DST" | cut -f1))"
fi
chown -R root:root /opt/vulkan-sdk
chmod -R a+rX /opt/vulkan-sdk
# Prove the runner user can actually read what the build needs.
sudo -u "$RUNNER_USER" test -r "$SDK_DST/x86_64/include/vulkan/vulkan.h" \
    || { echo "runner cannot read Vulkan headers" >&2; exit 1; }
sudo -u "$RUNNER_USER" test -r "$SDK_DST/x86_64/lib/libshaderc_shared.so" \
    || { echo "runner cannot read shaderc" >&2; exit 1; }
echo "readable by $RUNNER_USER: OK"

# --------------------------------------------------------------------------
# 3. Unpack the runner.
# --------------------------------------------------------------------------
say "runner package"
if [ -x "$RUNNER_DIR/config.sh" ]; then
    echo "already unpacked"
else
    install -o "$RUNNER_USER" -g "$RUNNER_USER" -m 700 -d "$RUNNER_DIR"
    sudo -u "$RUNNER_USER" tar xzf "$TARBALL" -C "$RUNNER_DIR"
    echo "unpacked $(basename "$TARBALL")"
fi

# --------------------------------------------------------------------------
# 4. Register with GitHub.
#    NOT --ephemeral. Ephemeral runners deregister after one job and must be
#    re-registered with a fresh token, which means storing a long-lived PAT on
#    the box — trading one risk for another. The value of ephemerality is low
#    here because the workflow has no pull_request trigger (so only repo-owned
#    code ever runs) and it wipes the workspace as its first step.
# --------------------------------------------------------------------------
say "registering $RUNNER_NAME"
if [ -f "$RUNNER_DIR/.runner" ]; then
    echo "already configured; use ./config.sh remove to re-register"
else
    sudo -u "$RUNNER_USER" -- "$RUNNER_DIR/config.sh" \
        --url "$REPO_URL" \
        --token "$TOKEN" \
        --name "$RUNNER_NAME" \
        --labels "$LABELS" \
        --work _work \
        --unattended --replace
fi

# --------------------------------------------------------------------------
# 5. Job environment. The workflow preflights VULKAN_SDK and fails with a
#    "provision the runner" message if it is missing.
# --------------------------------------------------------------------------
say "runner .env"
printf 'VULKAN_SDK=%s/x86_64\n' "$SDK_DST" > "$RUNNER_DIR/.env"
chown "$RUNNER_USER:$RUNNER_USER" "$RUNNER_DIR/.env"
cat "$RUNNER_DIR/.env"

# Caches must live OUTSIDE the workspace: the workflow wipes it and checkout
# cleans it, so vendor sources under OloEngine/vendor/ are gone every run.
sudo -u "$RUNNER_USER" mkdir -p "$RUNNER_HOME/.cache/cpm" "$RUNNER_HOME/.cache/ccache"

# --------------------------------------------------------------------------
# 6. Start at boot via user-level systemd + linger, matching the pattern the
#    existing gh-runner-1/2/3 use on this host.
# --------------------------------------------------------------------------
say "service"
loginctl enable-linger "$RUNNER_USER"
RUID=$(id -u "$RUNNER_USER")
for _ in $(seq 1 20); do [ -d "/run/user/$RUID" ] && break; sleep 0.5; done
[ -d "/run/user/$RUID" ] || { echo "user manager for $RUNNER_USER never came up" >&2; exit 1; }

install -o "$RUNNER_USER" -g "$RUNNER_USER" -d "$RUNNER_HOME/.config/systemd/user"
cat > "$RUNNER_HOME/.config/systemd/user/$UNIT.service" <<EOF
[Unit]
Description=GitHub Actions runner (OloEngineBase, AMD GPU)
After=network-online.target

[Service]
ExecStart=$RUNNER_DIR/run.sh
WorkingDirectory=$RUNNER_DIR
Restart=always
RestartSec=10
KillMode=process
KillSignal=SIGTERM
TimeoutStopSec=5min

[Install]
WantedBy=default.target
EOF
chown "$RUNNER_USER:$RUNNER_USER" "$RUNNER_HOME/.config/systemd/user/$UNIT.service"

run_uctl() { sudo -u "$RUNNER_USER" XDG_RUNTIME_DIR="/run/user/$RUID" systemctl --user "$@"; }
run_uctl daemon-reload
run_uctl enable --now "$UNIT.service"
sleep 3
run_uctl --no-pager --lines=15 status "$UNIT.service" || true

say "done"
echo "Verify from GitHub:  gh api repos/drsnuggles8/OloEngineBase/actions/runners --jq '.runners[]|{name,status,busy}'"
