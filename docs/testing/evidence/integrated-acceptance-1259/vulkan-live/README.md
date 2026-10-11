# Live Vulkan acceptance data (#1259)

Raw output of `scripts/perf/acceptance-live.py` on the Release editor (`--rhi=vulkan`), RTX 4090,
2026-10-11. Read with [integrated-acceptance-1259.md](../../../../analysis/integrated-acceptance-1259.md).

| File | Session | What it is |
|---|---|---|
| `live-vk.json` | 1 | Forward, Forward+, Deferred (and a hybrid cell that traced nothing: no light requested the tier yet), three passes, 120 frames per camera |
| `live-vk-attrib.json` | 1 | Deferred and hybrid attribution: CPU scopes, groom budget stats, raw series, RT scene stats |
| `live-vk-hybrid.json` | 2 | Deferred and hybrid after every shadowed light requests the tier; three passes with attribution. Its Deferred rows carry the producer-gate bug (`Vegetation::Queue` 24–71 ms) |
| `live-vk-gatefix.json` | 3 | Deferred and hybrid on the build with the producer-gate fix |
| `live-vk-vvl.json` | 4 | every cell and camera under the Khronos validation layer (frame times are not comparable) |
| `reopen.json` | 2 | private bytes and working set every 5 s while the scene reloads |
