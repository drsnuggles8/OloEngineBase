# Vulkan: a CPU buffer write between two recorded draws is a semantic, not a memcpy

Postmortem of the #691 Phase 8 screenshot-parity failure: two of three sandbox
scenes rendered **skybox-only** under Vulkan — every sphere, cube and floor
plane missing — while the third scene rendered perfectly, the full Vulkan pass
suite stayed green, and the log carried zero errors, zero validation messages
and zero dropped-draw counters.

## The failure

GL executes `glNamedBufferSubData` and the draws around it **in command
order**: upload A, draw 1, upload B, draw 2 means draw 1 samples A and draw 2
samples B, even though they name the same buffer. A naive Vulkan port of that
buffer (one `VkBuffer`, one life-stable device address, mapped write-through
in `SetData`) silently replaces that contract with **last-write-wins**: the
CPU writes land immediately, the recorded draws execute at submit, so *every*
draw in the frame reads upload B.

`CommandDispatch::DrawMeshInstanced` is exactly that shape: it re-uploads the
one shared `ModelInstanceBuffer` (SSBO 15, `InstanceData[]`) before **every**
auto-batched instanced draw. Under Vulkan all batches rendered with the final
batch's instance data — typically the last tiny line-gizmo batch — so their
geometry collapsed off-screen / degenerate.

## Why it was scene-specific (the trap that misdirects the diagnosis)

- **MaterialSpheres / LightingTest**: grids of the *same* sphere/cube mesh —
  CommandBucket auto-batches them into instanced draws → all gone.
- **VehiclesTest**: 18 entities of mostly *unique* meshes — nothing batches,
  every draw is single-instance, and the per-draw **UBO** path
  (`VulkanUniformBuffer`) was already arena-versioned per write → perfect
  frame.

A backend bug that keys on *how the content is submitted* masquerades as a
*content* bug ("something wrong with these scenes"). The suite stayed green
because no tenant interleaved two uploads of one SSBO with draws in a single
recording.

## How it was localized (the method is the lesson)

1. **Pinned-pose screenshot gate** (same scene, same camera, both backends,
   read the PNGs side by side) turned "Vulkan looks right" — true only on the
   one scene being watched — into a table of per-scene verdicts.
2. **`olo_render_capture_target` on `SceneDepth`** separated "draws never
   landed" from "post chain ate them": depth was far-plane everywhere except
   the debug-line meshes → geometry pass, not post.
3. The **survivor pattern** (unique-mesh draws and debug-line meshes work,
   repeated-mesh draws vanish) named the batching path, and reading the
   dispatch showed the shared-buffer re-upload.

## The fix (ADR 0011 amendment (80))

`VulkanStorageBuffer::SetData` inside a recording bracket pushes a snapshot of
the written range into the frame arena; draws embed the snapshot address via
`GetRootDataAddress()` (the storage twin of the UBO's root-data seam), so each
recorded draw keeps the bytes that were current when it was recorded. Three
deliberate scope edges:

- **Compute keeps the persistent address** (`commandOrderedBufferReads=false`
  in `AssembleAndPushRootData`): compute SSBOs are GPU-write participants
  (cull survivors, atomically-bumped indirect seeds) whose writes must land in
  the buffer the indirect-draw resolve reads, and the GPU-cull path already
  pools per-dispatch buffers.
- **Outside a recording bracket no snapshot is taken** — load-time /
  between-frames uploads would burn arena space for an ordering nobody
  observes.
- **`VulkanVertexBuffer` (vertex pull) has the same latent archetype** if a
  pull stream is ever rewritten mid-frame between draws. Renderer2D uploads
  each stream once per frame today; recorded as a seam, not fixed.

Tenant: `VulkanPassSuite.InterleavedInstanceBufferUploadsKeepCommandOrderAcrossDraws`
(upload LEFT, draw, upload RIGHT, draw — both quads must land).

## The rule

When porting any GL-shaped facade to a deferred-execution backend, audit every
`SetData`/`Upload` call site for the pattern **"same buffer written more than
once per frame with draws recorded between the writes"**. Each such site needs
per-write versioning (arena snapshot, ring, or per-draw allocation) — a
persistent buffer with write-through *cannot* express it, and the failure is
silent, scene-shaped, and invisible to any tenant that doesn't interleave.

The GL-parity checklist that found this (screenshot gate → intermediate-target
capture → survivor-pattern reasoning) is reusable for any "backend X renders
scene Y wrong but scene Z right" report.
