# Self-hosted Linux runners

Provisioning notes for the runners behind
[`.github/workflows/gpu-conformance-amd.yml`](../../.github/workflows/gpu-conformance-amd.yml)
and the Linux CI jobs in
[`asan.yml`](../../.github/workflows/asan.yml),
[`vulkan-off.yml`](../../.github/workflows/vulkan-off.yml) and
[`steam-stub.yml`](../../.github/workflows/steam-stub.yml).

**One box, two runner pools, and the labels are the whole separation:**

| Runner(s) | Labels | Serves |
|---|---|---|
| `olo-gpu-amd` | `self-hosted,Linux,X64,gpu-amd` | the nightly GPU conformance run, exclusively |
| `olo-ci-1`, `olo-ci-2` | `self-hosted,Linux,X64,olo-ci` | the Linux sanitizer / vulkan-off / steam-stub jobs |

Neither pool requests the other's label, so a CI job can never queue in front of
the nightly and the nightly can never starve CI. Do not "tidy" these into one
label set.

## Why this box exists

### For the GPU nightly — the original reason

The one thing GitHub cannot sell at any price: **a real GPU**.

Every test gated on `RenderPropertyFixture`'s GL-context check — 97 test
files — currently hits `GTEST_SKIP() << "no usable GL 4.6 context available"` in
CI. That is the visual, golden and perf third of the renderer pyramid running
nowhere but a developer's Windows box, in a repository whose own rules say
rendering changes must be verified against pixels.

`cross-vendor.yml` substitutes Mesa llvmpipe, but llvmpipe is a *third software
implementation*, not a second **vendor**. It cannot catch AMD GLSL-compiler or
driver divergence from the NVIDIA hardware the goldens are baselined on.

### For the Linux CI jobs — added by #1009

**This section previously said the opposite**, and the correction is the point:

> ~~It is not a cost or speed measure… Moving existing jobs here would save
> nothing — the CI critical path is Windows, and the three Linux sanitizer jobs
> already finish before those.~~

Measured over 250 runs in a 36-hour window (`gh run list --json`, medians):

| Workflow | trigger | n | median |
|---|---|---|---|
| Windows | pull_request | 27 | **159.5 min** |
| Sanitizers | pull_request | 24 | **154.4 min** |

Five minutes apart is **co-critical**, not "already finish before those". The
claim was written from an earlier, faster state of the suite and was never
re-measured.

It is still true that hosted runners are free for a public repo, so this is not a
cost measure. What the box buys is what an Actions cache structurally cannot:

- **Caches that simply persist.** No upload, no download, no 10 GB repository cap,
  and — the one that actually bit — no post-job save step for a cancellation to
  skip. See [ci-cache-that-looks-alive.md](../agent-rules/ci-cache-that-looks-alive.md).
- **Capacity off the hosted pool**, which the Windows jobs then have to themselves.

**Be honest about what it does NOT buy:** the box is Linux, so it cannot take
Windows, and Windows is the 159-minute ceiling. Moving Sanitizers here removes a
co-critical path; it does not move the wall clock on its own.

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

`RenderPropertyTest.cpp` now selects a backend via **`--olo-gl-backend`**:

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
  glslang-devel spirv-tools \
  python3-jinja2 \
  autoconf autoconf-archive automake libtool pkgconf-pkg-config \
  curl zip unzip tar
```

**The last two rows are required since the vcpkg manifest migration (#773/#781)**,
and they are separate requirements that fail at different stages:

- **autotools** — ports with an autotools Linux build (libsodium is the one in
  this manifest) need `autoconf`/`automake`/`libtoolize`/`autoconf-archive`/
  `pkg-config` from the system. Fails during the port build.
- **`curl zip unzip tar`** — vcpkg's *own* `scripts/bootstrap.sh` requires these
  before it will build `vcpkg-tool`, so a miss here fails earlier than any port,
  with vcpkg's own "Could not find zip" help text rather than our message. This
  host had neither `zip` nor `unzip` on a fresh Rocky 10 install. **Install all
  four together**: bootstrap.sh reports only the *first* missing one, so fixing
  the name it prints just surfaces the next on the following run.

The `setup-vcpkg` composite action *verifies* both sets on a self-hosted runner
and fails with a provisioning message naming this section — it only auto-installs
on GitHub-hosted images, where passwordless sudo + apt-get exist. (An earlier
version ran `sudo apt-get` unconditionally, which died on this Rocky host with
"sudo: a terminal is required to read the password" and kept the nightly red for
days before the suite ever built.)

Both gaps share one root cause worth remembering when adding any new build
dependency: **GitHub-hosted images ship a large implicit toolchain**, so a
requirement that is invisible on `ubuntu-latest` is a hard provisioning step
here. This box is the only place such a gap can surface, and it surfaces as a
red nightly nobody reads.

**`python3-jinja2` from dnf, not `pip install --user`.** glad2's code generation
imports jinja2, and `--user` installs it into the *installing* user's home —
invisible to `gh-runner-olo` for the same `0700` reason as everything else. That
mistake cost a run: all three preflights passed and the build died four minutes
in on `ModuleNotFoundError: No module named 'jinja2'`. The toolchain preflight
now checks importable modules as well as binaries.

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

> **Version note (#691):** ADR 0010's *tooling* floor is SDK
> **1.4.357.0** — the first SDK whose GPU-assisted validation understands
> `VK_EXT_descriptor_heap`. The 1.4.350.1 install above predates that: the
> engine builds and the device-gated tenants run (the capability gate reads
> *driver* extensions, not the SDK), but GPU-AV diagnostics for heap traffic
> are unavailable until the SDK is upgraded. When touching this host, prefer
> installing 1.4.357.0+ alongside and repointing `actions-runner/.env`.

The workflow preflights this and fails with a "provision the runner" message
rather than a CMake stack trace several minutes into configure.

### 1c. Caches outside the workspace

The workflow wipes the workspace and `actions/checkout` cleans it, so anything
under it is gone every run — including `OloEngine/vendor/`, where CPM and
FetchContent place vendor sources. Without caches sited elsewhere, every nightly
re-downloads and rebuilds the entire vendor set.

The GPU nightly points its two at the runner user's home:

```
CPM_SOURCE_CACHE=/home/gh-runner-olo/.cache/cpm
CCACHE_DIR=/home/gh-runner-olo/.cache/ccache
```

The CI jobs added by #1009 use a second set under `.cache/olo/`, created by
`scripts/setup-olo-ci-runners.sh`:

| Path | Written by | Notes |
|---|---|---|
| `~/.cache/olo/ccache` | ccache, **shared by every job** | bounded at 30 GiB by the setup script |
| `~/.cache/olo/vcpkg-binary-cache` | vcpkg (`files` provider), shared | `setup-vcpkg` evicts oldest-first only when it exceeds 20 GiB |
| `~/.cache/olo/vcpkg-<runner-name>` | the vcpkg clone itself, **per runner** | |
| `~/.cache/olo/cpm` | CPM / FetchContent, shared | |

Two of those distinctions are load-bearing, and both come from the same fact:
**two runner instances share one Unix account.**

- **ccache, not sccache.** sccache is a per-*user* daemon, and the first client to
  start it fixes the server's environment — so a second concurrent job's
  `SCCACHE_DIR` is silently ignored, which is the exact genre of failure this
  work exists to remove. ccache is a process per compiler invocation with an
  on-disk format built for concurrent writers. One shared directory is right:
  ccache hashes the full command line, so `-fsanitize=address` and
  `-fsanitize=thread` objects cannot collide, and a single large LRU beats
  several fixed-size ones. The hosted arm keeps sccache, where a job owns the VM.
- **The vcpkg clone is per runner.** `setup-vcpkg` does not treat it as read-only
  — it `git checkout --force --detach`es to the manifest baseline and runs
  `bootstrap-vcpkg.sh` in it. Two jobs sharing one clone would race on the index
  lock, on `downloads/`, and (silently, which is worse) on the version database,
  which one job can swap out from under another's install. `RUNNER_NAME` is
  unique per instance and stable across runs, so each keeps its own warmth.

The binary *cache* is shared deliberately — the `files` provider writes to a temp
path and renames, and sharing is the whole point of it.

**Why local disk rather than `actions/cache`, in one line:** an entry on local
disk has no post-job save step for a cancellation to skip, no ref scoping, and no
10 GB cap — the three defects in
[ci-cache-that-looks-alive.md](../agent-rules/ci-cache-that-looks-alive.md). The
`actions/cache` restore and save steps in those workflows are therefore
`runner.environment == 'github-hosted'`-gated: on this box a restore would
overwrite the live cache with an older snapshot of itself.

`rm -rf ~/.cache/olo` is a safe full reset; the next run is simply cold.

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

### 6. The CI runner pool (#1009)

The Linux sanitizer, vulkan-off and steam-stub jobs run here too. They need more
packages than the GPU nightly (which builds with GCC and takes its shader
toolchain from the LunarG SDK) and their own runner instances. Both are in one
idempotent script:

```bash
sudo bash scripts/setup-olo-ci-runners.sh   "$(gh api -X POST repos/drsnuggles8/OloEngineBase/actions/runners/registration-token --jq .token)"
```

It installs `clang`/`lld`/`compiler-rt`, `mesa-libEGL-devel`, `nasm` and the
X/Wayland `-devel` set; creates the `~/.cache/olo/*` directories from §1c; and
registers `olo-ci-1` and `olo-ci-2` with labels `self-hosted,linux,x64,olo-ci`
under user systemd, exactly like the GPU runner.

**The compiler is clang 21, not clang-19.** Every hosted Linux job pins clang-19
from apt.llvm.org because ubuntu-24.04's default libstdc++ lacks
`std::forward_like`. Rocky 10 has no clang-19 package and does not need one — its
libstdc++ is far newer. The version skew against the hosted fallback is accepted
deliberately: a second compiler version is coverage. The consequence to remember
when debugging: **a diagnostic can appear on one runner kind and not the other.**
If a self-hosted job fails and its hosted fallback passes, check the compiler
version before concluding the box is broken.

**Why two CI runners and not three.** 31 GiB total. A sanitizer build is the
heaviest thing that lands here — clang++ takes ~3 GB per instrumented TU, the
jobs are pinned to `--parallel 2`, and the link is the real spike (which is why
they use lld). Budget ~9 GiB per concurrent job, reserve ~4 GiB for the OS, and
27 GiB leaves three concurrent slots — one of which `olo-gpu-amd` already holds.
The box also hosts `gh-runner-1/2/3` for a private repository, so "normally idle"
is not "always idle". `CI_RUNNER_COUNT=1` is the lever if memory pressure ever
shows up; raising it above 2 wants a measurement first.

Note the Linux sanitizer jobs use the **Unix Makefiles** generator, so the root
`CMakeLists.txt` link job pool (`OLO_LINK_JOBS`) does not apply — it is
Ninja-only. Their link concurrency is bounded only by `--parallel 2`, i.e. up to
2 jobs × 2 links on this box. That is the pessimistic worst case behind the
9 GiB figure above.

## GitHub-side settings

`gpu-conformance-amd.yml` is structurally safe: it has no `pull_request` or
`pull_request_target` trigger, so a fork PR can never schedule onto this box.

**The CI jobs added by #1009 DO have a `pull_request` trigger**, so for them the
boundary is an expression in `runs-on`:

```yaml
runs-on: ${{ (github.event_name != 'pull_request'
              || github.event.pull_request.head.repo.full_name == github.repository)
             && fromJSON('["self-hosted","linux","x64","olo-ci"]') || 'ubuntu-24.04' }}
```

A fork PR takes the `ubuntu-24.04` arm, which is a throwaway VM — fork code never
reaches this machine.

`vars.OLO_LINUX_SELF_HOSTED` is the kill switch, and it is deliberately the FIRST
term so it can only ever move a job back to a hosted runner, never onto the box:

```bash
# on
gh api -X POST repos/drsnuggles8/OloEngineBase/actions/variables   -f name=OLO_LINUX_SELF_HOSTED -f value=true
# off (or just delete the variable)
gh api -X PATCH repos/drsnuggles8/OloEngineBase/actions/variables/OLO_LINUX_SELF_HOSTED   -f name=OLO_LINUX_SELF_HOSTED -f value=false
```

It exists for two reasons. A `runs-on` naming a label set nobody serves does not
fail, it **queues** — for up to 24 hours — so the workflows had to be able to
land before the runners existed. And the blast radius here is the entire CI
system: turning the box off should be one API call by whoever is awake, not a
revert commit that itself needs CI to merge. Both halves of the condition are load-bearing: `push` and
`schedule` events carry no `pull_request` object at all, so the right-hand
comparison is empty there, and an expression written with only that half would
silently push every nightly onto hosted runners.

Three rules that follow, and none of them is optional:

- **Never introduce a `pull_request_target` that checks out the PR head.** That
  single pattern defeats the condition entirely, because
  `pull_request_target` runs with the base repository's permissions.
- **Approval-based gating is NOT the mechanism.** Approving a fork PR run is
  precisely what *executes* that fork's code. It stays on as belt and braces:
  **Settings → Actions → General → Fork pull request workflows** → "Require
  approval for all external contributors" (the default, first-time contributors
  only, is not sufficient for a public repo with a self-hosted runner).
- Keep the runners **repository-scoped**, not org-scoped, so no other repository
  can target the `gpu-amd` or `olo-ci` labels. (This account is a User, not an
  org, so runner groups are unavailable anyway — do not go looking for them.)

Never add a secret to these jobs. They need none.

## Bootstrapping the AMD golden baseline

`GoldenImageTests.cpp` uses `--olo-golden-vendor=<name>` to select a per-vendor
golden directory, so AMD output cannot clobber the NVIDIA baselines.

1. Land the EGL harness change; run the workflow via **Run workflow**.
2. The first run will fail on golden comparisons — expected, there is no
   `assets/tests/golden/amd/` yet.
3. Download the `gpu_amd_golden_diffs` artifact and **look at the PNGs**. Do not
   promote them unseen; a genuine AMD-vs-NVIDIA divergence and a broken headless
   context both present as "all goldens differ".
4. Once the images are confirmed correct, re-run with `--olo-golden-rebase`
   and commit the resulting `assets/tests/golden/amd/*.png`.

Perf baselines follow the same pattern with `--olo-perf-rebase`. Because
the box is dedicated rather than a shared hosted VM, these numbers are actually
stable enough to be worth trending — unlike hosted-runner perf data.

## Troubleshooting

| Symptom | Cause |
|---|---|
| A Linux CI job queues forever | no `olo-ci` runner is online — `gh api repos/drsnuggles8/OloEngineBase/actions/runners`. There is no automatic hosted fallback for a same-repo PR: the `runs-on` expression chose this box before scheduling |
| `clang: command not found` on an `olo-ci` job | the CI provisioning was never run — `sudo bash scripts/setup-olo-ci-runners.sh` (§6) |
| A job fails here and its hosted rerun passes | check the compiler first. This box is clang 21, the hosted arm is clang-19 (§6) |
| Cold builds despite the persistent caches | `~/.cache/olo` is owned by the wrong user, or the job took the hosted arm. The setup step logs the resolved cache dir |
| Preflight: `no /dev/dri/renderD128` | `amdgpu` not loaded, or the runner user lost `render` group |
| Preflight: `SOFTWARE RENDERER` | Mesa fell back to llvmpipe — check `MESA_LOADER_DRIVER_OVERRIDE`, driver install, and that the *software* EGL device wasn't selected |
| All GPU tests still skip | The EGL harness fallback is missing, or `--olo-gl-backend=egl` is not reaching the process |
| Failures that look like missing shaders | Job ran from the repo root instead of `OloEditor/` |
| Stale generated `.inl` failures | Workspace not wiped; the workflow's first step does this, but a manually-run build may not have |
