# Every invocation must reach the scan, and test the order rather than the set

Issue #713 (GPU prefix-sum primitive). Read this before converting a compaction pass from `atomicAdd` to `GPUPrefixSum`, or before adding a work-group scan to any `.comp`.

## Rules

1. **Keep every invocation alive through a scan; never `return` early before `OloPrefixSumExclusive`.** The scan contains `barrier()`, and a barrier only some invocations reach is undefined behaviour (wrong scan, correct scan, or a TDR). Out-of-range lanes contribute the additive identity (`value = (idx < u_Count) ? 1u : 0u`) and guard only the use of `slot`. [Details](gpu-scan-compaction-details.md#1-the-compute-shader-opening-you-have-written-a-hundred-times-is-a-hang)
2. **Give any loop around a scan a work-group-uniform trip count.** Bound it by `ceil(count / groupSize)` batches, not `i < count`. The two shared-memory barriers inside the scan stay outside the `if`s. [Details](gpu-scan-compaction-details.md#1-the-compute-shader-opening-you-have-written-a-hundred-times-is-a-hang)
3. **A green test does not clear the barrier contract; read the shader.** `PrefixSum.glsl`'s header states the contract. [Details](gpu-scan-compaction-details.md#1-the-compute-shader-opening-you-have-written-a-hundred-times-is-a-hang)
4. **Assert the compacted sequence, never a sorted or set comparison.** `atomicAdd` already produced the right set; only the order changed. Follow `GPUParticleCompactionTest`: ascending survivor indices, repeat-run identity, and all-alive/all-dead masks. [Details](gpu-scan-compaction-details.md#2-a-compaction-test-that-compares-sets-passes-on-the-bug-it-is-meant-to-catch)
5. **Size scan tests to straddle block boundaries.** Use 255/256/257 and 65535/65536/65537, not round or power-of-two counts. [Details](gpu-scan-compaction-details.md#2-a-compaction-test-that-compares-sets-passes-on-the-bug-it-is-meant-to-catch)
6. **Convert a pass for its ordering guarantee, and say so; do not justify it with contention.** `GPUPrefixSumPerfProbe` (DISABLED_, run with `--gtest_also_run_disabled_tests`) showed the scan 2.0-4.4x slower than atomics on RTX 4090 / GL 4.6, with no measurable contention penalty. Converted: `Particle_Compact.comp` (alive list is the draw order); left: `LightCulling.comp`, `InstanceFrustumCull.comp`, `VirtualClusterCull.comp`, `Fluid_Compact.comp`. [Details](gpu-scan-compaction-details.md#3-converting-a-pass-is-a-trade-and-the-contention-half-of-the-trade-is-a-myth)
7. **Never use `#ifdef GL_KHR_shader_subgroup_arithmetic` as a support test.** glslang predefines it regardless; use the explicit opt-in define `OLO_PREFIX_SUM_USE_SUBGROUP` next to the `#extension` lines, defaulting to the portable path. [Details](gpu-scan-compaction-details.md#4-ifdef-gl_khr_shader_subgroup_arithmetic-is-not-a-support-test)
8. **Use `#extension ... : require`, not `: enable`, for the explicit subgroup path.** A driver lacking it must fail loudly at compile time. [Details](gpu-scan-compaction-details.md#4-ifdef-gl_khr_shader_subgroup_arithmetic-is-not-a-support-test)
9. **Run `glslc` on every `.comp` or shader `include/` change against both target envs, with no `-I`.** `glslc -fshader-stage=compute --target-env=opengl4.5 <file> -o /dev/null` (and `vulkan1.2`); an `-I` lets a wrong include path pass (#978). glslc is not the last word on rules 10 and 11. [Details](gpu-scan-compaction-details.md#4-ifdef-gl_khr_shader_subgroup_arithmetic-is-not-a-support-test)
10. **Put `layout(local_size_*) in;` above the `#include` of any header using `gl_NumSubgroups`/`gl_SubgroupID`.** NVIDIA rejects the reverse order (C7594) while glslang and glslc accept both. [Details](gpu-scan-compaction-details.md#5-layoutlocal_size_-in-must-precede-the-include)
11. **Never write a test that tolerates a compute shader failing to compile.** In Debug `OLO_CORE_ASSERT("Compute shader compilation failure!")` is a modal dialog that blocks an unattended run at 0% CPU forever. Ask the driver first (`glGetStringi(GL_EXTENSIONS, …)` for `GL_KHR_shader_subgroup`) and skip the path, as `GPUPrefixSumTest` does. [Details](gpu-scan-compaction-details.md#6-a-failed-compute-shader-compile-is-a-modal-dialog-not-a-return-value)
12. **Do not put `#extension` in an include.** It must precede all non-preprocessor tokens, so the consumer has to enable it (same constraint as `BindlessHeap.glsl`). [Details](gpu-scan-compaction-details.md#7-smaller-things-that-cost-time)
13. **Do not assume a 32-wide subgroup.** `gl_SubgroupSize` is 8 on some Intel parts and may vary per dispatch, so per-subgroup totals are scanned in shared memory. [Details](gpu-scan-compaction-details.md#7-smaller-things-that-cost-time)
14. **Raise `kHighestKnownSSBOBinding` in `ShaderReflectionBindingTest` when adding a top SSBO slot.** `SSBO_PREFIX_SUM_*` (54-56) failed it until re-pointed; `SSBO_VERTEX_PULL` (57) and `SSBO_BONE_PULL` (63) are Vulkan-only and never reach reflection. [Details](gpu-scan-compaction-details.md#7-smaller-things-that-cost-time)
15. **Remember `Ref<T>` propagates const.** A `const Ref<StorageBuffer>&` cannot call non-const buffer methods; route `count == 0` through the normal dispatch instead of `ClearData()`. In-place scanning is safe (each invocation touches only its own index). [Details](gpu-scan-compaction-details.md#7-smaller-things-that-cost-time)
16. **Pin the tile-size to `kMaxElements` relation with a `static_assert`.** A histogram-and-scatter pass scans `bins x ceil(maxCount / tile)` entries (splat radix sort, #1043), so a smaller tile can exceed the 16.7 M ceiling. [Details](gpu-scan-compaction-details.md#7-smaller-things-that-cost-time)
17. **Test a radix sort with ties and with ties spanning many tiles.** Use `MatchesTheCpuReference{WhenEveryDepthIsIdentical,WhenTiesSpanManyRadixTiles}`; a single-tile tie cloud cannot see a cross-tile rank error. [Details](gpu-scan-compaction-details.md#7-smaller-things-that-cost-time)
18. **Prove the algorithm on the CPU before spending a build.** A ~150-line Python transcription diffed against `sorted()` costs minutes against an hour of lock queue. [Details](gpu-scan-compaction-details.md#7-smaller-things-that-cost-time)

## Commands and checks

```
glslc -fshader-stage=compute --target-env=opengl4.5 <file> -o /dev/null
```

Run it against both `opengl4.5` and `vulkan1.2` target envs, with no `-I`.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[gpu-scan-compaction-details.md](gpu-scan-compaction-details.md).
