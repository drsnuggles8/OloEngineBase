# A write-once GL buffer is immutable storage

Create a large buffer that is filled once and never refilled with
`VertexBuffer::CreateImmutable` / `IndexBuffer::CreateImmutable`, not `Create(data, size)`.
On the GL backend `Create(data, size)` is mutable storage (`glNamedBufferData`), and the
NVIDIA driver keeps a resident system-memory copy of every mutable buffer. The buffer then
costs its size twice: once in VRAM and once in the process's working set.

## What it cost

The Bernese coat's groom rest stream is 1.5 GB of ribbon vertices (4 corners x 64 B per
segment, 6.1 M segments). Built through `Create(data, size)`, it added 1538 MB to the working
set at upload and kept it there. `BreedShowcaseEvidenceTest` crossed the test binary's
6 GB per-process ceiling (`MEMORY CEILING EXCEEDED`, exit 77, peak 7.4 GB), and the editor
carried the same copy (#1608). With immutable storage the upload added 20 MB.

## The rules

- **Flags 0 means flags 0.** Immutable storage with `GL_DYNAMIC_STORAGE_BIT` brought the
  resident copy back on the same stream (+1518 MB). A buffer that `SetData` refills must
  keep `Create(size)`; it is not a candidate.
- **`SetData` on an immutable vertex buffer is refused loudly** (assert plus log) instead of
  failing as a silent `GL_INVALID_OPERATION`. A coat built with `CreateImmutable` and then
  refilled would hold its first pose.
- **Read a GL buffer back with `glGetNamedBufferSubData`, never `glMapBuffer`.** A map of
  immutable storage without `GL_MAP_READ_BIT` fails; the resource inspector previewed every
  such buffer as nothing until it switched.
- **The renderer memory report does not show this.** It books VRAM backing. To attribute
  working set, log `FPlatformMemory::GetStats().UsedPhysical` around the suspect creation
  and sample the process from outside (`WorkingSet64` every 500 ms, tagged with the running
  gtest case). That pairing found this in one run.
- **Stream a coat-sized buffer; never hold it whole.** Immutable storage removes the copy that
  stays, not the one being built: the Samoyed's 2 GB rest stream, built whole on the CPU, still
  spiked the working set by 2.6 GB while it uploaded. `BuildGroomStrandRestMeshStreamed` hands the
  vertices to `ImmutableVertexBufferUpload` in 16 MiB chunks and records what the pass used to read
  back off them (`GroomRestStreamMeasures`); the packed-path case fell from 5.77 to 4.28 GB. A new
  reader of the rest stream reads the measures, or the walk records what it needs.
- **Vulkan is unaffected.** Its buffers are device-local (VMA prefers `DEVICE_LOCAL`), so
  `CreateImmutable` there is `Create`.

Pinned by `ImmutableBufferStorageTest` (storage flags, data, the mutable control, the
inspector read) and `GroomGpuDeformation.AStreamedRestBuild*` (the streamed stream and measures
equal the whole build's, bit for bit); the groom wiring by the breed suite passing at the default
ceiling.
