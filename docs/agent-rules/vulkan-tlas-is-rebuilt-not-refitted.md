# The Vulkan TLAS is rebuilt every frame, never refitted

Read before changing how `VulkanRayTracingBackend::RecordTlasBuild` builds, or before chasing a
device fault that names an address just past an acceleration structure.

## Rules

1. **Record a TLAS as a full build (`VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR`). Do not bring
   back `MODE_UPDATE` for it.** A request to refit keeps its storage and is rebuilt into it; the
   caller is told `TlasBuildReason::InPlaceRebuild`, which the scene counts as a build.
   `RayTracingDevice.ARefitRequestIsRecordedAsAFullBuildIntoTheSameStorage` pins both. BLAS refits
   for deformed surfaces are a different path and are unchanged.
2. **A device fault whose address is the first page after a live buffer is a read past that
   buffer, and it is silent wherever the next page is mapped.** Suspect it when a fault appears or
   disappears with unrelated changes: anything that moves an allocation hides or exposes it.
3. **To test an over-read, give the suspect buffer a dedicated allocation**
   (`VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`, temporarily). Nothing follows it then, so the
   fault no longer depends on its neighbours, and an A/B of the suspected reader is one run each.
   Padding the buffer proves nothing: it also moves it.
4. **Use the levers before reading code.** `OLO_VULKAN_NO_RAY_TRACING=1` took this fault away in one
   run and `OLO_VK_TRACE_BUFFERS=1` showed the address belonged to no vertex or index buffer; both
   are in `DebugLevers.inl`.

## What happened (2026-11-18)

Merging `master` into the Drift boat branch, the Vulkan editor lost its device a few frames after
opening `Drift.olo`: `DEVICE FAULT REPORT`, `READ of invalid address 0x16880000`, last checkpoint
`ShadowPass`. OpenGL was fine, the packaged Vulkan game was fine, and the scene with the old boat
was fine. Neither side of the merge had changed the ray-tracing backend.

The address was the end of a 32 MiB memory block. The TLAS, re-created that frame because a BLAS
compaction had finished, had been placed in the block's last 4 KiB (`0x1687f000`, 3840 bytes), and
the next two frames updated it in place. Whether it landed there depended on every allocation
before it, which is why the fault followed the number of meshes in the scene (six instances fine,
seven not), survived or vanished with a log line added, and had not shown before the merge.

With the TLAS storage in a dedicated allocation the result stopped depending on layout:

| TLAS recording | Drift, new boat, 8 instances | master's Drift plus cubes, 6 to 13 instances |
|---|---|---|
| in-place update (`MODE_UPDATE`, src == dst) | fault one page past the storage within 5 frames, 4 runs of 4 | no fault in about 3,000 frames a run |
| full build into fresh storage every frame | no fault, 2 runs of about 2,300 frames | not run |
| full build into the same storage | no fault, 3 runs of 2,460 to 3,440 frames | not run |

The update matched the size query in flags, geometry and primitive count, the instance order and
BLAS references did not change between the build and the updates, and one of two runs on a single
queue (`OLO_VK_ASYNC_COMPUTE=0`) still faulted, so it is not a reader on the other queue. What in
that instance set makes the update read past the structure is not known; the driver was NVIDIA's
on an RTX 4090. The engine now does what both vendors' guidance says to do for a TLAS anyway:
rebuild it.
