# Write a per-subset shader step as "subset 0, then subset 1 behind a branch", never as a data-dependent loop over indexed arrays

In a shader that Mesa's AMD compiler has to build, a step that runs once per subset is written
with constant array indices:

```glsl
Refit(masks[0], g_Fit0[0], g_Fit1[0]);
if (subsets > 1)
    Refit(masks[1], g_Fit0[1], g_Fit1[1]);
```

and not as a loop whose trip count is data-dependent and whose body indexes arrays by the loop
variable:

```glsl
for (int s = 0; s < subsets; ++s)          // subsets is 1 or 2, decided by the block mode
    Refit(masks[s], g_Fit0[s], g_Fit1[s]);  // inout bound to a dynamically indexed element
```

The two are the same program. NVIDIA's driver and Mesa's llvmpipe compile either in about a
second. Mesa's AMD path (radeonsi on the CI box's RX 5600 XT, RADV on the null device, with the
ACO and the LLVM backend alike) took over 200 s and 14 GB on the loop form and was OOM-killed.
`BC6HEncodeCommon.glsl` had three such loops (quantize, score, refit) plus a two-iteration
`FitEndpoints` loop written the same way; all four are now the branch form, and the comment above
`EvaluateCandidate` says so in the file itself.

## What stayed green

Every PR check. The shader compiled in 1 s on the NVIDIA dev box and in 1.3 s on llvmpipe, the
GPU-gated tests skip on the self-hosted sanitizer jobs (`--olo-gl-backend=none`), and the only
run that ever handed the shader to Mesa's AMD compiler was the nightly `gpu-sanitizers-amd`
workflow. There, from the night the encoder landed (2026-09-05, #624) until this fix, every
`BC6HGpuEncoder` case that touches the shader — including a 13×7-pixel one — either timed out at
600 s (runs 34006887409, 34076957588, 34180634926) or died as `Subprocess killed` after 5 to 10
minutes (34303910937, 34430144106). The kills were the kernel's OOM killer at the runner cgroup's
14 GiB, on a process that reported 7 to 14.5 GB anonymous RSS; two of them took the whole
`user@1004` slice, and every Linux runner with it. The one case that passed
(`AnUndrainedQueueGivesUpOnceInsteadOfHanging`) never loads the shader.

## The bisection, on the dev box

No AMD hardware is needed. `RADV_FORCE_FAMILY=navi10` makes Mesa's Vulkan ICD enumerate a
**null device** for that chip, and building a compute pipeline on it runs the whole compiler.
In WSL (Ubuntu 24.04, Mesa 25.2.8, the same major as the box's 25.2.7) with a 100-line
`vkCreateComputePipelines` probe under `/usr/bin/time -f 'wall=%es maxrss=%MkB'` and
`ulimit -v` set to the box's cap, glslc'd from the include-resolved GLSL:

| Variant | RADV navi10 |
|---|---|
| shipped shader (unsigned) | 207 s, 14.3 GB, killed at the cap (ACO and LLVM identical) |
| one-subset pass only | 167 s, killed |
| one-subset pass, refine loop removed | **0.09 s, 97 MB** |
| refine loop kept, `RefitEndpoints` call removed | 0.15 s |
| `RefitEndpoints` reduced to `e0.x += mask` | killed |
| the refit's `for (s < subsets)` changed to `s` = 0, `if (subsets > 1)` s = 1 | 0.16 s (one-subset pass); full shader still killed |
| the same for quantize and score as well (the shipped fix) | **1.0 s, 278 MB**; signed variant 1.3 s |
| llvmpipe, GL path, shipped shader | 1.3 s, 200 MB |

Twenty variants in all; every one that kept a data-dependent subset loop over indexed arrays was
killed, every one without it compiled in about a second. Making the loop bounds opaque (no
unrolling) did not help, and neither did unrolling the inner `c` loop by hand, removing the
weight-table lookups, or removing the dynamic vector component reads. Which NIR pass grows is
not pinned: Ubuntu's driver carries no symbols. The fix does not depend on knowing.

Reproducing the GL front end (what radeonsi is fed) is the same probe against EGL with
`LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460`; llvmpipe
compiled every variant, so the front end is not where it grows.

## The guards

- **`ShaderCompileBudgetTest`** (`OloEngine/tests/Rendering/`) builds every
  `assets/shaders/compute/*.comp` on the RADV navi10 null device in-process, through the same
  shaderc options as `VulkanComputeShader`, with a budget of 30 s and 1 GiB per shader, and
  asserts the two BC6H shaders are in the measured set. It runs wherever the RADV ICD is
  installed — the self-hosted box, and the hosted Ubuntu runners now that `setup-linux-build`
  installs `mesa-vulkan-drivers` — and skips loudly elsewhere (Windows has no null device).
  Fine on NVIDIA is not evidence for this class; this test is.
- **The per-process memory ceiling** (`OloEngine/tests/MemoryCeiling.h`,
  `--olo-rss-ceiling-mb`, default 6144 = 14 GiB cgroup ÷ `ctest --parallel 2` with headroom) stops
  a test process past its resident-set ceiling with exit code 77 and the running test's name in
  the output. An OOM kill has neither, and can take the runner down. Resident set, not virtual
  size: a sanitizer build reserves terabytes it never touches, so `RLIMIT_AS` cannot express
  this, and Linux does not enforce `RLIMIT_RSS`.

## Reading a CI OOM kill

The victim's test name is not in the kernel log. Match the kill timestamps (the box logs local
time, UTC+2) against the jobs whose `Run tests` step spans them, then read that job's ctest
output for `Subprocess killed` and `***Timeout`. The nightly's GPU-enabled jobs run on the
`olo-ci` runners too, so a kill during a PR's sanitizer window can belong to the nightly.
