# Validate feedback against physical attachments, not version names

Resolve a pass's original texture accesses through version aliases and views before
checking same-pass feedback. A `WriteNewVersion` changes dependency identity but
keeps the source's physical storage. Sampling `Normals` while drawing into
`SceneColor@Water` still conflicts when Normals names that framebuffer's RT2.
Issue #1554 closes the validator gap exposed by water in #1332.

Keep dependency expansion separate from physical validation. A framebuffer write
conservatively covers its attachments, but a read of one attachment must not become
a physical read of every sibling. `RGBuilder::Read(view)` extends the parent's
lifetime; dependency expansion retains the parent writer even for views created
in a consumer's Setup. The validator
uses the original setup accesses and maps them to parent storage, colour index or
depth aspect, and mip/layer/cube-face range. A multisample resolve view names its
single-sample backing, not the source image. Declaration ranges must cover the
entire read/write intersection.

An attachment load, blend, or depth-test read (`RenderTargetRead`) is the ordinary
attachment RMW contract. It does not authorize shader sampling. A pass that copies
scene colour before drawing declares `ReadTransferSourceBeforeWrite`; that permits
only the ordered transfer and leaves a later shader sample diagnosable. The pass's
Execute must perform the copy before binding/drawing the destination.

## Depth and diffusion policy

Use an independent snapshot while the destination framebuffer retains writable
depth/stencil state. Disabling depth writes alone does not change the Vulkan
attachment layout: the current backend binds
`VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`. Vulkan's read-only depth
exception requires the matching attachment and sampling layout, which the current
backend does not provide. See the [Vulkan render-pass contract](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)
and [image layouts](https://docs.vulkan.org/spec/latest/chapters/resources.html).

The production choices are:

| Pass | Sampling source | Copy point |
|---|---|---|
| Water | Existing depth and normal snapshots | Existing pre-decal depth and pre-water normals |
| FluidIntermediates | SceneDepthAttachment | None: its splat/thickness targets are separate images |
| FluidComposite | FluidSceneDepthSnapshot | Immediately before fluid composition, after water |
| SkinDiffusion and SSS | DiffusionDepthSnapshot and DiffusionHandoffSnapshot | Immediately before the diffusion band, after the geometry/modifier chain |

The diffusion hand-off is copied too, so neither additive pass needs a framebuffer
feedback exemption. Skin and snow share the unchanged pre-diffusion inputs. These
are transient graph resources; ordinary reads order the snapshots before the
writers, extend storage lifetimes, and cull unused copy nodes. Scene depth and
normal exports remain attachment views, preserving downstream version publication.

Preserve sampling state as well as texels. A bare transient `Texture2D` defaults
to repeat wrapping, whereas framebuffer attachments use linear filtering and
clamp-to-edge. Allocate screen-space snapshots as views of independent,
single-sample graph-owned framebuffers so both backends inherit the attachment
sampling contract. Do not change texture parameters after minting a resident
OpenGL bindless handle. The visual fixtures compare the production snapshot's
filter and wrap parameters with its source.

`RenderGraphAttachmentFeedback.*` pins aliases, disjoint attachments and ranges,
resolve backing, transfer-only declarations, snapshots, and both backend policies.
The skin, snow, and fluid visual fixtures validate their real enabled graph and
capture native, dynamic bilinear, and requested FSR2 frames with requested 1x/4x
MSAA. Forward paths retain a single-sample target; Deferred MSAA forces FSR2's
spatial fallback. Deferred dynamic scaling retains the explicit #1537 capture
rejection. Vulkan FSR2 requests use the existing backend fallback.

## Diagnostics must handle the snapshot storage contract

Inspect a framebuffer's attachment specification before requesting colour
attachment zero. A depth-only snapshot backing has no colour slot: probing it
and then falling back to depth already accesses an empty array. The live
`olo_render_validate` sweep exposed this as a Release OpenGL crash; the shared
MCP resolver now selects depth directly and has headless selection contracts.

Report the active framebuffer viewport as the render size. Dynamic resolution
keeps the allocation at display size, so `GetSpecification().Width/Height`
describes storage, while `GetActiveViewportWidth/Height` describes the region
actually drawn. The live 0.75-scale checks caught this distinction in
`olo_perf_snapshot`: 640x360 storage contains a 480x270 rendered region.
