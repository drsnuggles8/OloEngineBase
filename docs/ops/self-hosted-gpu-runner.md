# Self-hosted AMD GPU runner

Provisioning notes for the runner behind
[`.github/workflows/gpu-conformance-amd.yml`](../../.github/workflows/gpu-conformance-amd.yml).

## Why this runner exists

It is **not** a cost or speed measure. OloEngineBase is a public repository, so
standard GitHub-hosted runners are free and unlimited, and queue wait is
negligible (median 7 s). Moving existing jobs here would save nothing — the CI
critical path is Windows (SonarCloud ~155 min, ASan/clang-cl ~151 min), which a
Linux box cannot take, and the three Linux sanitizer jobs already finish before
those.

The runner exists for the one thing GitHub cannot sell at any price: **a real
GPU**. Every test gated on `RenderPropertyFixture`'s GL-context check — 97 test
files — currently hits `GTEST_SKIP() << "no usable GL 4.6 context available"` in
CI. That is the visual, golden and perf third of the renderer pyramid running
nowhere but a developer's Windows box, in a repository whose own rules say
rendering changes must be verified against pixels.

`cross-vendor.yml` substitutes Mesa llvmpipe, but llvmpipe is a *third software
implementation*, not a second **vendor**. It cannot catch AMD GLSL-compiler or
driver divergence from the NVIDIA hardware the goldens are baselined on.

## Verified host capability

Confirmed working on the target box before any of this was written:

| | |
|---|---|
| GPU | AMD Radeon RX 5600 XT — Navi 10, PCI `1002:731F` |
| Driver | `amdgpu`, `/dev/dri/card0` + `/dev/dri/renderD128` |
| Userspace | Mesa 25.2.7 — `radeonsi_dri.so`, RADV (`radeon_icd`) |
| Context | **OpenGL 4.6 Core, GLSL 4.60** via `EGL_MESA_platform_surfaceless` |
| Display server | **none** — no X, no Wayland (`multi-user.target`) |
| CPU / RAM | Ryzen 7 3700X (8C/16T) / 31 GiB |
| Toolchain | cmake 4.4.0, ninja 1.13, clang 21.1.8, gcc 14.3.1, python 3.12 |

Headless offscreen render + readback was verified pixel-exact (FBO clear to
`0.25,0.5,0.75,1.0` → readback `64,128,191,255`, `glGetError() == 0`).

Note the software EGL device (`EGL_MESA_device_software`) is also enumerated on
this host and **fails** to produce a 4.6 core context — which is why the
workflow's preflight explicitly rejects `llvmpipe`/`softpipe`/`swrast` rather
than trusting that a context implies hardware.

## How the headless context works

The suite used to get its GL context only from GLFW, which on Linux needs an
X11 or Wayland display. This host has neither, so `glfwCreateWindow` failed and
every GPU test skipped — the exact condition the runner exists to eliminate.

`RenderPropertyTest.cpp` now selects a backend via **`OLO_TEST_GL_BACKEND`**:

| Value | Behaviour |
|---|---|
| unset (*Auto*) | GLFW first, then EGL if GLFW can't reach a display server |
| `glfw` | GLFW only — the unchanged local-developer path |
| `egl` | EGL only — what this workflow pins |

CI pins `egl` deliberately. Under *Auto* a broken display server would silently
switch backends, and two backends can produce subtly different pixels — a golden
baseline has to know which one produced it.

The EGL path tries `EGL_MESA_platform_surfaceless` first, then falls back to
enumerating EGL devices and taking the first backed by a DRM render node.
Skipping node-less devices is not cosmetic: Mesa also publishes an
`EGL_MESA_device_software` device, and binding it would hand us llvmpipe while
the run is labelled with a hardware vendor's golden set.

libEGL is discovered optionally in `OloEngine/tests/CMakeLists.txt`
(`OLO_TESTS_HAVE_EGL`). Where it is absent the GLFW-only path remains and those
tests skip exactly as before, so this cannot break a build.

Two implementation notes worth preserving:

- The EGL headers are included with `EGL_NO_X11` / `MESA_EGL_NO_X11_HEADERS`.
  Without them `<EGL/eglplatform.h>` drags in `<X11/Xlib.h>`, which defines
  `None` as a bare `0L` macro and mangles every `None` enumerator in the engine
  headers (`MeshComponent::Primitive::None` among them).
- The context is surfaceless — no default framebuffer. That is fine because
  every pixel the suite inspects is read back from an FBO.

Do **not** reach for Xvfb as an alternative: it is a software rasteriser, i.e.
llvmpipe with extra steps, and would produce "AMD" goldens that are nothing of
the sort.

### Guarding against a green-but-empty run

The job's own worst failure mode is that the EGL path breaks, every GPU test
calls `GTEST_SKIP()`, and the nightly passes having verified nothing. The
**"Assert GPU tests actually ran"** step parses the gtest XML and fails the run
if *any* test skipped for want of a context. It matches both gate sites, which
word their skip differently:

- `RendererAttachedTest.cpp` — *"no usable GL 4.6 context available"*
- `OLO_ENSURE_GPU_OR_SKIP()` — *"No GPU / GL 4.5+ context available…"*

If you add a third GPU gate with new wording, add its phrase there too.

## Host provisioning

### 1. Packages

Most of the toolchain is already present. Mirroring `asan.yml`'s Linux
dependency set, on Rocky 10:

**Everything the build needs must be installed system-wide** (`/usr`, `/opt`) —
never in a person's home directory. `/home/obueker` is mode `0700`, so anything
under it is invisible to `gh-runner-olo`. This bit three separate times during
bring-up: the Vulkan SDK, the runner tarball, and finally `cmake`/`ninja`, which
were pip installs in `/home/obueker/.local/bin` and produced a bare `cmake:
command not found`. The workflow now preflights the toolchain so the error names
the cause.

```bash
sudo dnf install -y \
  cmake ninja-build ccache gcc gcc-c++ \
  vulkan-loader-devel vulkan-headers \
  mesa-libGL-devel mesa-libEGL-devel libglvnd-devel \
  libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel \
  wayland-devel wayland-protocols-devel libxkbcommon-devel \
  glslang-devel spirv-tools
python3 -m pip install --user jinja2   # OloHeaderTool codegen
```

The X11/Wayland `-devel` packages are needed to *build* GLFW even though no
display server runs — GLFW compiles its backends unconditionally.

### 1b. Vulkan SDK (required — distro packages are not enough)

`OloEngine/CMakeLists.txt` raises `FATAL_ERROR` at configure time on the first
missing shader library, and it needs **shaderc, glslang, SPIRV-Tools and
SPIRV-Cross**, not just the loader. Rocky ships `vulkan-loader` but none of
those, so configure fails with:

```
Required shader library not found: SHADERC_LIB.
```

Install the LunarG SDK **somewhere the runner user can actually reach** — on this
host that means `/opt/vulkan-sdk/<version>/`, root-owned and world-readable. It
must *not* live in a person's home directory: `/home/obueker` is mode `0700`, so
the runner user cannot traverse into it even though the SDK's own subdirectories
are world-readable. Point CMake at it with a single prefix, which resolves Vulkan
*and* all the shader libraries at once:

```
-DCMAKE_PREFIX_PATH="$VULKAN_SDK"
```

Export `VULKAN_SDK` for the runner service — add it to `actions-runner/.env`, so
every job inherits it:

```
VULKAN_SDK=/opt/vulkan-sdk/1.4.350.1/x86_64
```

The workflow preflights this and fails with a "provision the runner" message
rather than a CMake stack trace several minutes into configure.

### 1c. Caches outside the workspace

The workflow wipes the workspace and `actions/checkout` cleans it, so anything
under it is gone every run — including `OloEngine/vendor/`, where CPM and
FetchContent place vendor sources. Without caches sited elsewhere, every nightly
re-downloads and rebuilds the entire vendor set. The workflow points both at the
runner user's home:

```
CPM_SOURCE_CACHE=/home/gh-runner-olo/.cache/cpm
CCACHE_DIR=/home/gh-runner-olo/.cache/ccache
```

### 1d. Why the interchange formats are switched off

Beyond cost, `OLO_WITH_USD=ON` **cannot complete as a non-root user**: OpenUSD's
`pxr/cmake_install.cmake` installs `pxrConfig.cmake` to an absolute path,
ignoring `cmake --install --prefix`, and dies with

```
file INSTALL cannot copy file ".../pxrConfig.cmake"
to "/usr/local/pxrConfig.cmake": Permission denied
```

The workflow passes `-DOLO_WITH_USD=OFF -DOLO_WITH_ALEMBIC=OFF
-DOLO_WITH_MATERIALX=OFF`, which is correct on its own merits (none of them
affect GPU rendering conformance) and sidesteps this too.

### 1e. Gotcha when building locally alongside another tree

CPM/FetchContent subbuilds live in the **shared source tree**
(`OloEngine/vendor/`), not the build directory, and are stamped with the
generator that created them. A second build tree using a *different* generator
fails at configure with:

```
Error: generator : Ninja
Does not match the generator used previously: Ninja Multi-Config
```

Use the same generator as any existing tree, or wipe `OloEngine/vendor/`. CI is
unaffected — it always starts from a wiped workspace.

### 2. Dedicated, isolated user

The host already runs `gh-runner-1/2/3` for a **private** repository. This
runner serves a **public** one, which is a materially different trust level: a
compromise here must not reach those runners or their secrets.

```bash
sudo useradd -m -s /bin/bash gh-runner-olo
sudo usermod -aG render,video gh-runner-olo    # GPU access
```

Do not add it to `wheel`. Do not share a home directory, work directory, ccache
directory, or any credential with `gh-runner-1/2/3`.

`/dev/dri/renderD128` is currently mode `0666` on this host, so the `render`
group is belt-and-braces rather than strictly required — but set it anyway, in
case the permissive mode is a distribution default that changes.

### 3. Register the runner

A tarball is staged at `/home/obueker/actions-runner-linux-x64-2.336.0.tar.gz`.

**Extract it as root, not as the runner user.** `/home/obueker` is mode `0700`,
so `gh-runner-olo` cannot traverse into it and `tar` fails with `Cannot open:
Permission denied` before reading a byte — the same reason the Vulkan SDK has to
be relocated to `/opt` above. Extract, hand ownership over, then register as the
runner user:

```bash
RUNNER_DIR=/home/gh-runner-olo/actions-runner
sudo install -o gh-runner-olo -g gh-runner-olo -m 700 -d "$RUNNER_DIR"
sudo tar xzf /home/obueker/actions-runner-linux-x64-2.336.0.tar.gz -C "$RUNNER_DIR"
sudo chown -R gh-runner-olo:gh-runner-olo "$RUNNER_DIR"

sudo -u gh-runner-olo -- "$RUNNER_DIR/config.sh" \
  --url https://github.com/drsnuggles8/OloEngineBase \
  --token <REGISTRATION_TOKEN> \
  --name olo-gpu-amd \
  --labels self-hosted,linux,x64,gpu-amd \
  --work _work \
  --unattended --replace
```

`config.sh` prints a few `ldd: ./bin/lib*.so: No such file or directory` lines
from its dependency probe (it uses relative paths from the wrong directory).
They are cosmetic — registration and `Runner.Listener` both work regardless.

Mint the token with `gh api -X POST
repos/drsnuggles8/OloEngineBase/actions/runners/registration-token --jq .token`
(or **Settings → Actions → Runners → New self-hosted runner**). It expires after
an hour.

**Not `--ephemeral`, deliberately.** An ephemeral runner deregisters after one
job and must be re-registered with a fresh token, which in practice means
storing a long-lived PAT on the box — trading one risk for another. The value of
ephemerality is low here because the workflow has no `pull_request` trigger, so
only repo-owned code ever runs, and it wipes the workspace as its first step.
Revisit this if the runner is ever pointed at a workflow that untrusted code can
reach.

### 4. Service

The host's existing runners start via **user-level systemd with lingering**
(`/var/lib/systemd/linger/`), so match that rather than installing a system unit
with `svc.sh`:

```bash
sudo loginctl enable-linger gh-runner-olo
# ~/.config/systemd/user/actions-runner-olo.service -> ExecStart=<dir>/run.sh
sudo -u gh-runner-olo XDG_RUNTIME_DIR=/run/user/$(id -u gh-runner-olo) \
    systemctl --user enable --now actions-runner-olo.service
```

### 5. One-shot setup script

All of the above — user, SDK relocation, unpack, register, `.env`, caches,
linger, unit — is scripted and idempotent. Run it as root with a freshly minted
token:

```bash
sudo bash scripts/setup-olo-runner.sh \
  "$(gh api -X POST repos/drsnuggles8/OloEngineBase/actions/runners/registration-token --jq .token)"
```

Verify afterwards:

```bash
gh api repos/drsnuggles8/OloEngineBase/actions/runners \
  --jq '.runners[]|{name,status,busy,labels:[.labels[].name]}'
```

## GitHub-side settings

The workflow is structurally safe — it has no `pull_request` or
`pull_request_target` trigger, so a fork PR can never schedule onto this box.
These are defence in depth:

- **Settings → Actions → General → Fork pull request workflows**: set approval to
  **"Require approval for all external contributors"**. The default is
  first-time contributors only, which is not sufficient for a public repo with a
  self-hosted runner.
- Keep the runner **repository-scoped**, not org-scoped, so no other repository
  can target the `gpu-amd` label.
- Never add a secret to this job. It needs none.

## Bootstrapping the AMD golden baseline

`GoldenImageTests.cpp` reads `OLOENGINE_GOLDEN_VENDOR` to select a per-vendor
golden directory, so AMD output cannot clobber the NVIDIA baselines.

1. Land the EGL harness change; run the workflow via **Run workflow**.
2. The first run will fail on golden comparisons — expected, there is no
   `assets/tests/golden/amd/` yet.
3. Download the `gpu_amd_golden_diffs` artifact and **look at the PNGs**. Do not
   promote them unseen; a genuine AMD-vs-NVIDIA divergence and a broken headless
   context both present as "all goldens differ".
4. Once the images are confirmed correct, re-run with `OLOENGINE_GOLDEN_REBASE=1`
   and commit the resulting `assets/tests/golden/amd/*.png`.

Perf baselines follow the same pattern with `OLOENGINE_PERF_REBASE=1`. Because
the box is dedicated rather than a shared hosted VM, these numbers are actually
stable enough to be worth trending — unlike hosted-runner perf data.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Preflight: `no /dev/dri/renderD128` | `amdgpu` not loaded, or the runner user lost `render` group |
| Preflight: `SOFTWARE RENDERER` | Mesa fell back to llvmpipe — check `MESA_LOADER_DRIVER_OVERRIDE`, driver install, and that the *software* EGL device wasn't selected |
| All GPU tests still skip | The EGL harness fallback is missing, or `OLO_TEST_GL_BACKEND=egl` is not reaching the process |
| Failures that look like missing shaders | Job ran from the repo root instead of `OloEditor/` |
| Stale generated `.inl` failures | Workspace not wiped; the workflow's first step does this, but a manually-run build may not have |
