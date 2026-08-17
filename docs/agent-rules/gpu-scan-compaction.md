# Every invocation must reach the scan, and test the order rather than the set

Issue #713 (GPU prefix-sum / parallel-scan primitive). Read this before converting a compaction
pass from `atomicAdd` to `GPUPrefixSum`, or before adding a work-group scan to any `.comp`.

The engine's compaction passes historically allocated output slots with a global `atomicAdd`:
`LightCulling.comp`, `Particle_Compact.comp`, `Fluid_Compact.comp`, `InstanceFrustumCull.comp`,
`VirtualClusterCull.comp`. `include/PrefixSum.glsl` + `GPUPrefixSum` replace that with a scan where
it is worth it.

§1–§3 are about the conversion and are not reliably caught by a test. §4–§6 are three separate ways
a *correct* scan still fails to reach the GPU, each of which cost a build round on #713 — and two of
them reproduce only on the real driver, so a green headless shader check does not clear them.

---

## 1. The compute-shader opening you have written a hundred times is a hang

Every other compute shader in this repo starts:

```glsl
uint idx = gl_GlobalInvocationID.x;
if (idx >= u_Count)
    return;
```

In a shader that calls `OloPrefixSumExclusive`, that is undefined behaviour. The scan contains
`barrier()`, and a barrier that only *some* invocations of a work group reach is UB — the tail work
group of any count that is not a multiple of 256 is exactly that case. The correct shape is to keep
every invocation alive through the scan and let the out-of-range ones contribute the additive
identity:

```glsl
uint idx   = gl_GlobalInvocationID.x;
uint value = (idx < u_Count) ? 1u : 0u;   // out of range contributes 0
uint slot  = OloPrefixSumExclusive(value, total);
if (idx < u_Count)
    /* ...use slot... */;
```

The same rule applies one level up: a **loop** around the scan must have a work-group-uniform trip
count, so bound it by `ceil(count / groupSize)` batches rather than by `i < count`.

**Why a test will not save you.** UB here does not mean "wrong answer" — it means the driver picks.
Observed outcomes for the same broken shader are a plain wrong scan, a correct scan (the tail group
happened to be full), and a GPU hang that on Windows surfaces as a TDR device reset rather than a
test failure. A green run is not evidence that the barrier contract is satisfied; only reading the
shader is. `PrefixSum.glsl`'s header block states the contract, and `PrefixSum_Scan.comp` carries a
NO EARLY RETURN comment at the exact line where the habit would reassert itself.

Corollary for the two shared-memory barriers *inside* the scan: they sit outside the `if`s on
purpose. Moving one in — which reads as a harmless optimisation, since the guarded lanes do no work
— reintroduces the same UB.

---

## 2. A compaction test that compares sets passes on the bug it is meant to catch

This is the trap that makes compaction work feel tested when it isn't.

`atomicAdd`-based compaction already produced the **correct set** of survivors every frame. What it
did not produce was a stable **order**: the slot a survivor received depended on which invocation
reached the counter first. So a test written the natural way —

```cpp
std::ranges::sort(actual);      // or a set/multiset comparison
EXPECT_EQ(actual, expected);
```

— passes identically before and after the conversion, and would keep passing if the scan were
replaced by an atomic tomorrow. It asserts nothing about the property the whole exercise exists to
create.

Assert the sequence instead. `GPUParticleCompactionTest` does it three ways, deliberately split so a
failure says *which* property broke:

- the compacted list equals the ascending list of survivor indices (wrong-but-stable order fails
  only this);
- repeated compaction of an unchanged input buffer is element-for-element identical
  (right-but-unstable order fails only this);
- the degenerate all-alive / all-dead masks, where an off-by-one in the dead-prefix identity
  `deadBefore == idx - aliveBefore` has nowhere to hide.

`GPUPrefixSumTest` adds the sizes that matter for the scan itself: a scan is trivially right on a
full block and wrong in exactly two places — the tail of a partial block and the seam where one
block's offset folds into the next — so the size list straddles 255/256/257 and 65535/65536/65537
rather than sampling round numbers. A power-of-two-only test passes on an implementation that
silently rounds its count up.

---

## 3. Converting a pass is a trade, and the contention half of the trade is a myth

**Measure before you justify a conversion with "contention".** The intuitive argument — one
`atomicAdd` per survivor on a single address must serialize, so a scan should win at scale — is
what the issue text says and what this PR originally claimed. It does not survive measurement.

`GPUPrefixSumPerfProbe` (DISABLED_, run it with `--gtest_also_run_disabled_tests`) times the
preserved pre-#713 atomic compaction against the scan over identical input, RTX 4090 / GL 4.6,
median of 31 GPU-timed samples:

| particles | alive % | atomic (ms) | scan (ms) | ratio |
|---|---|---|---|---|
| 1 000 | any | 0.0051 | 0.0225 | 4.4× |
| 10 000 | any | 0.0051 | 0.0215–0.0225 | 4.2–4.4× |
| 100 000 | 10 / 50 / 95 | 0.0082 / 0.0092 / 0.0082 | 0.0348 / 0.0338 / 0.0338 | 3.7–4.3× |
| 1 000 000 | 10 / 50 / 95 | 0.0584 / 0.0604 / 0.0543 | 0.1219 / 0.1219 / 0.1229 | 2.0–2.3× |

**The scan never wins** on this configuration — the ratio narrows from 4.4× to 2.0× as fixed dispatch
overhead stops dominating, but it does not invert anywhere in the range.

**On contention, be careful what this does and does not show.** Read honestly, the probe is evidence
*against* serialization being the cost here, but it is not a controlled contention experiment:

- Every atomic-path invocation does exactly one `atomicAdd` at every alive fraction — sweeping
  10 → 95 % changes only how those increments *split* between `aliveCount` and `deadCount`, not how
  many there are. So the sweep contrasts "mostly one address" (95 %) against "two addresses"
  (50 %), and the 95 % case came out no slower (0.0543 vs 0.0604 ms at 1 M). That is a *narrow*
  result, not a general statement about atomics.
- The count sweep is the stronger signal: 1 000 → 1 000 000 particles is 1000× the atomics for 11×
  the time (0.0051 → 0.0584 ms). Serialization on a single address cannot produce sublinear scaling
  like that, so on this device the atomic path is bandwidth-bound rather than serialization-bound.

What that supports is a scoped claim: **on an RTX 4090 / GL 4.6, for this shader at these sizes,
atomic-slot allocation showed no contention penalty worth converting away from.** It is not a claim
about GPUs generally, and it is not an isolated measurement of same-address contention — that would
need a variant with the atomics removed as a control. If you need the general claim, build that
control first.

What is left is the half that is real: **determinism**, plus an absolute cost small enough to pay for
it (+0.026 ms at the engine's default 100 k particles; +0.065 ms at 1 M — under 1 % of a 60 fps
frame either way). Convert on the ordering guarantee, priced against that absolute number. Do not
convert on a contention argument you have not measured.

| Pass | Verdict |
|---|---|
| `Particle_Compact.comp` | **Converted — on ordering alone.** The alive-index list *is* the draw order of every transparent particle (`ParticleBatchRenderer` draws instance `i` as `aliveIndices[i]`), so it was re-rolling the blend order every frame from an identical particle buffer. Costs 2–4× on a pass measured at 0.03–0.12 ms; the contention argument originally given for it did not survive the probe above. |
| `LightCulling.comp` | Left. The work-group scan would replace the shared-memory allocation for free (no extra dispatch, no extra buffer), but the **global** allocation is already only one atomic per work group, and making the per-cluster offsets deterministic needs a second full cull pass over a hot Forward+ path. A partial conversion buys a deterministic list *within* a cluster while the cluster's base offset stays scheduler-dependent — worth doing, not worth doing alone. |
| `InstanceFrustumCull.comp` | Left — and the probe above weakens the case further. It was the strongest *contention* candidate (one atomic per surviving instance at 10k+), but same-address atomic contention turned out not to be measurable, so a conversion here would be paying 2–4× for an ordering guarantee nothing currently depends on. |
| `VirtualClusterCull.comp` | Left. Multi-phase, indirect-dispatched, and its `atomicAdd` targets are per-instance segments rather than one global counter — so it was the weakest contention case even before the probe showed contention is not the cost. |
| `Fluid_Compact.comp` | Left. Nothing downstream depends on the order. |

The generalisable rule: **convert where the ordering guarantee or the contention relief is worth the
extra dispatches, and say which.** Converting a pass whose ordering nobody depends on adds
dispatches and risk for nothing.

---

## 4. `#ifdef GL_KHR_shader_subgroup_arithmetic` is not a support test

The natural way to write an optional subgroup fast path is to let the header detect it:

```glsl
#ifdef GL_KHR_shader_subgroup_arithmetic     // <-- always true under glslang
    ... subgroupExclusiveAdd(value) ...
#else
    ... shared-memory fallback ...
#endif
```

GLSL predefines an extension's macro when that extension is **enabled**, so this reads correctly.
**glslang predefines it either way** — the macro advertises what the *compiler* knows, not what the
*shader* has enabled. So the fast path is selected in every shader, including the ones that never
wrote an `#extension` line, and they fail outright:

```
error: 'subgroupExclusiveAdd' : required extension not requested: GL_KHR_shader_subgroup_arithmetic
error: 'subgroup op' : requires SPIR-V 1.3
```

Both of the engine's routes reject it — `--target-env=vulkan1.2` on the first line and
`--target-env=opengl4.5` on both, since OpenGL 4.5's SPIR-V 1.0 cannot represent subgroup ops at
all. Use an **explicit** opt-in define (`OLO_PREFIX_SUM_USE_SUBGROUP`) alongside the `#extension`
lines, defaulting to the portable path, so a shader that forgets the directives cannot land on the
fast path by accident.

Two follow-on rules:

- **`: require`, not `: enable`, once the path is explicit.** `: enable` is only useful when the code
  can detect the outcome and fall back — which is exactly what does not work here. With an explicit
  opt-in, a driver lacking the extension must fail loudly at compile time; the test then treats that
  compile failure as "no subgroup support on this driver, skip that half" and says so.
- **Verify shader edits with `glslc` before spending a build.** A full Debug build here is 30+
  minutes behind a cross-worktree mutex; this bug was found in seconds with
  `glslc -fshader-stage=compute --target-env=opengl4.5 -I. <file> -o /dev/null`, run against both
  target envs. Do that for any change to a `.comp` or a shader `include/`.
- **…but glslc is not the last word — two failures below reproduce only on the real driver.**

## 5. `layout(local_size_*) in;` must precede the `#include`

A shader that includes a header using `gl_NumSubgroups` / `gl_SubgroupID` must declare its work-group
size *first*. The driver derives those builtins from `gl_WorkGroupSize`, and NVIDIA's GLSL compiler
rejects a use that precedes the layout qualifier declaring it:

```
0(234) : error C7594: OpenGL requires declaring a layout qualifier for work group size
                      before using the builtin constant gl_WorkGroupSize
```

**glslang and `glslc` accept either order**, so this passes every headless check and fails only on
hardware. Put the layout line above the `#include` in every consumer — it costs nothing and the
failure cannot then arise on either compiler. `PrefixSum.glsl`'s contract 2 states it; all three
consumers carry a comment at the line, because the natural authoring order (defines, include, then
layout next to `main`) is the broken one.

## 6. A failed compute-shader compile is a modal dialog, not a return value

`OpenGLComputeShader` raises `OLO_CORE_ASSERT(..., "Compute shader compilation failure!")` when a
compile fails. In a Debug build that is a **message box**, so in an unattended run — CI, an agent
session, anything without someone to click OK — the process does not fail. It **blocks forever**.
The signature is a test binary sitting at exactly 0% CPU with a window titled `OloEngine Assert`:

```powershell
Get-Process OloEngine-Tests | Select-Object CPU        # flat across two samples => blocked
# EnumWindows for the pid; class #32770 is a Windows dialog
```

The consequence for test design: **you cannot write a test that tolerates a shader failing to
compile.** A `Ref<ComputeShader>` that comes back null or invalid is unreachable in Debug — the
assert fires first. If a shader is optional (hardware-dependent, like the subgroup probe here), ask
the driver *before* creating it — `glGetStringi(GL_EXTENSIONS, …)` for `GL_KHR_shader_subgroup` —
and skip the whole path when the answer is no. `GPUPrefixSumTest` does exactly that.

## 7. Smaller things that cost time

- **`#extension` cannot live in the include.** GLSL requires every `#extension` directive to precede
  all non-preprocessor tokens, so `PrefixSum.glsl` cannot enable the subgroup extensions itself
  without dictating where every consumer puts its `#include` — the same constraint
  [BindlessHeap.glsl](../../OloEditor/assets/shaders/include/BindlessHeap.glsl) documents. That is
  *why* the opt-in above has to be a separate define rather than something the header can arrange.
- **Don't assume a 32-wide subgroup.** The Timberdoodle reference scans its per-subgroup totals with
  a second subgroup op, which is correct only while `numSubgroups <= subgroupSize`. `gl_SubgroupSize`
  is 8 on some Intel parts and is allowed to vary per dispatch, so this implementation scans those
  totals in shared memory instead.
- **Scanning in place is safe, and looks like it isn't.** Each invocation reads and writes exactly
  its own index, so no invocation observes another's element and there is nothing to order. That is
  what lets the recursion over block totals avoid a ping-pong buffer per level — reviewers reliably
  flag it, so the reasoning is in the code at both the shader and the binding-constant.
- **A new top SSBO slot has a second home.** `ShaderReflectionBindingTest`'s
  `kHighestKnownSSBOBinding` is a hand-maintained ceiling — there is no per-name validator for
  SSBOs, so "is this slot declared in C++?" is approximated by "is it <= the top one". Adding
  `SSBO_PREFIX_SUM_*` (54–56) above the previous top (53) failed that test until it was re-pointed.
  It is deliberately not the numerically largest constant: `SSBO_VERTEX_PULL` (57) and
  `SSBO_BONE_PULL` (63) live above it but only inside `#ifdef OLO_VULKAN` branches the harness never
  compiles, so they never reach reflection.
- **`Ref<T>` propagates const**, so a `const Ref<StorageBuffer>&` parameter cannot call a non-const
  buffer method. The empty-scan case originally called `ClearData()` through one; routing `count == 0`
  through the normal single-work-group dispatch (every lane out of range, total 0) removed both the
  const problem and the special case.
