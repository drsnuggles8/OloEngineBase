#pragma once

#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <glm/glm.hpp>
#include <span>
#include <string_view>

namespace OloEngine
{
    namespace RHI
    {
        // Defined in RHI/RHIResources.h — forward-declared so the explicit-
        // sampler BindTexture overload (whose default body ignores it) does
        // not pull that header into every RendererAPI includer (#691).
        struct SamplerDesc;
    } // namespace RHI

    struct Viewport
    {
        u32 x = 0;
        u32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };

    class RendererAPI
    {
      public:
        enum class API
        {
            None = 0,
            OpenGL = 1,
            // Vulkan bring-up (#691): selectable via `--rhi=vulkan`, which routes
            // window + context creation to Platform/Vulkan. The renderer proper does NOT
            // run under it yet — every factory below the context switches to a loud
            // "unsupported backend" assert, and Application skips Renderer::Init.
            // The member exists even when OLO_WITH_VULKAN=0 so selection code can parse
            // the flag and report "not compiled in" instead of "unknown backend".
            Vulkan = 2
        };

        enum class RendererType
        {
            None = 0,
            Renderer3D
        };

        // Renderer-agnostic texture target types (converted to GL enums by the backend)
        enum class TextureTargetType : u8
        {
            Texture2D = 0,
            TextureCubeMap,
            // Added by the call-site sweep (issue #691): the per-sample
            // MSAA paths copy *multisample* G-Buffer attachments, and a
            // multisample image cannot be copied as if it were a plain 2D one —
            // glCopyImageSubData requires matching targets and Vulkan requires
            // matching VkImageCreateInfo::samples. Without this member those
            // call sites had to keep a raw GL_TEXTURE_2D_MULTISAMPLE.
            Texture2DMultisample,
            // Terrain VT cache tiles (issue #715): the staging->cache
            // copies address individual array layers through
            // CopyImageSubDataFull's srcZ/dstZ, which needs the array target
            // on both operands. Append only — never renumber.
            Texture2DArray
        };

      public:
        virtual ~RendererAPI() = default;

        virtual void Init() = 0;

        // Release GPU-side renderer state WHILE THE CONTEXT/DEVICE IS STILL LIVE.
        //
        // Not pure, because a backend with no such constraint has nothing to do
        // here — but not backend-specific either, which is why it is a virtual
        // rather than a `dynamic_cast` at the call site: `RenderCommand.h` is
        // included across most of the engine, so reaching for the concrete
        // OpenGL type there would drag `glad/gl.h` in behind it and undo exactly
        // what this issue's include ratchet measures (#691).
        //
        // The destructor is too late for this. It runs from the static destructor
        // of `RenderCommand::s_RendererAPI` — at atexit, after the window and its
        // context are gone — and any GL/Vulkan call made from there executes
        // against a dead handle. Vulkan will want the same hook to destroy its
        // descriptor pools before `vkDestroyDevice`.
        virtual void ShutdownGpuResources() {}

        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) = 0;
        virtual void SetClearColor(const glm::vec4& color) = 0;
        virtual void Clear() = 0;
        virtual void ClearDepthOnly() = 0;
        virtual void ClearColorAndDepth() = 0;
        virtual Viewport GetViewport() const = 0;

        virtual void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount) = 0;
        virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount) = 0;
        virtual void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 instanceCount) = 0;
        virtual void DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount) = 0;
        virtual void DrawIndexedPatches(const Ref<VertexArray>& vertexArray, u32 indexCount, u32 patchVertices) = 0;

        // Raw-vertex-array overloads for POD shadow casters (no Ref<VertexArray>
        // available). Identity forms since issue #691; the u32
        // siblings are gone as of item 4.
        virtual void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount) = 0;
        virtual void DrawIndexedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex) = 0;
        // Instanced raw variant for batched shadow casters that share VAO + submesh range.
        virtual void DrawIndexedInstancedRaw(RHI::ResourceHandle vertexArray, u32 indexCount, u32 baseIndex,
                                             u32 instanceCount) = 0;
        virtual void DrawIndexedPatchesRaw(RHI::ResourceHandle vertexArray, u32 indexCount,
                                           u32 patchVertices) = 0;

        virtual void SetLineWidth(f32 width) = 0;

        virtual void EnableCulling() = 0;
        virtual void DisableCulling() = 0;
        virtual void FrontCull() = 0;
        virtual void BackCull() = 0;
        // CullMode::None is not a face — culling-off is DisableCulling().
        virtual void SetCullFace(RHI::CullMode face) = 0;
        virtual void SetDepthMask(bool value) = 0;
        virtual void SetDepthTest(bool value) = 0;
        virtual void SetDepthFunc(RHI::CompareOp func) = 0;
        virtual void SetBlendState(bool value) = 0;
        virtual void SetBlendFunc(RHI::BlendFactor sfactor, RHI::BlendFactor dfactor) = 0;
        virtual void SetBlendEquation(RHI::BlendOp mode) = 0;

        virtual void EnableStencilTest() = 0;
        virtual void DisableStencilTest() = 0;
        virtual bool IsStencilTestEnabled() const = 0;
        virtual void SetStencilFunc(RHI::CompareOp func, i32 ref, u32 mask) = 0;
        virtual void SetStencilOp(RHI::StencilOp sfail, RHI::StencilOp dpfail, RHI::StencilOp dppass) = 0;
        virtual void SetStencilMask(u32 mask) = 0;
        virtual void ClearStencil() = 0;

        // No face parameter, deliberately. Core-profile glPolygonMode accepts
        // only GL_FRONT_AND_BACK (anything else is GL_INVALID_ENUM) and every
        // call site in the engine passed it; Vulkan's polygonMode has no face
        // either. Carrying one would re-export a wart GL itself deprecated.
        virtual void SetPolygonMode(RHI::PolygonMode mode) = 0;

        virtual void EnableScissorTest() = 0;
        virtual void DisableScissorTest() = 0;
        virtual void SetScissorBox(i32 x, i32 y, u32 width, u32 height) = 0;

        // Indirect draw calls (GPU-driven rendering)
        virtual void DrawElementsIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer) = 0;
        virtual void DrawArraysIndirect(const Ref<VertexArray>& vertexArray, RHI::ResourceHandle indirectBuffer) = 0;
        // Raw-VAO variant used by the GPU-frustum-cull path which only has a
        // RendererID (the dispatcher's BindVAOIfNeeded() cache populates it).
        // Draws from the ALREADY-BOUND vertex array (issue #691).
        // Replaces the DrawElementsIndirectRaw(vaoID, ...) pair: its only caller
        // had just run BindVAOIfNeeded, so re-binding inside the draw was both
        // redundant and a bind behind the redundant-bind cache's back. Mirrors
        // the existing DrawBound* family, whose comment gives the same reason.
        // `topology` because the GPU-driven terrain path (issue #714) draws
        // PatchList through the same entry point the instance cull draws
        // Triangles through. It is an explicit parameter rather than a default
        // argument: a default on a virtual is resolved from the STATIC type, so
        // an override declaring a different one would silently disagree.
        virtual void DrawBoundElementsIndirect(RHI::ResourceHandle indirectBuffer,
                                               RHI::PrimitiveTopology topology) = 0;
        // Multi-draw indirect with a GPU-sourced draw count (core GL 4.6, issue #629):
        // reads DrawElementsIndirectCommand records from indirectBufferID starting at
        // indirectOffsetBytes and the u32 draw count from parameterBufferID at
        // parameterOffsetBytes; maxDrawCount caps the count, strideBytes is the
        // command record stride.
        virtual void MultiDrawElementsIndirectCountRaw(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indirectBuffer,
                                                       u32 indirectOffsetBytes,
                                                       RHI::ResourceHandle parameterBuffer, u32 parameterOffsetBytes,
                                                       u32 maxDrawCount, u32 strideBytes) = 0;

        // Compute shader dispatch
        virtual void DispatchCompute(u32 groupsX, u32 groupsY, u32 groupsZ) = 0;

        // GPU-sourced dispatch dimensions: reads a uvec3 group count from
        // `argsBuffer` at `offsetBytes` (which must be 4-byte aligned on both
        // backends). Added for the terrain LOD descent (issue #714), where each
        // level's group count is written by the previous level's kernel and must
        // never round-trip through the CPU. A zero group count is legal and does
        // nothing — that is how a level with no survivors costs nothing.
        virtual void DispatchComputeIndirect(RHI::ResourceHandle argsBuffer, u32 offsetBytes) = 0;

        // Task/mesh-pipeline dispatch (issue #813): launches the
        // currently-bound task/mesh GRAPHICS pipeline with a
        // groupsX x groupsY x groupsZ grid of task workgroups. Requires the
        // bound shader to carry task+mesh stages and the backend to answer
        // SupportsMeshShaders() — callers gate on that capability and take a
        // classic vertex-pipeline path when it says no; a call that reaches
        // an unsupported backend anyway is a LOUD dropped draw, never a
        // silent one. A zero group count in any dimension is a legal no-op.
        // Deliberately NOT named "DrawMesh": that name is the mesh-ASSET draw
        // path in Renderer/Commands/RenderCommand.h.
        virtual void DrawMeshTasks(u32 groupsX, u32 groupsY, u32 groupsZ) = 0;
        virtual void MemoryBarrier(MemoryBarrierFlags flags) = 0;

        // The render graph's pre-pass barrier batch, carrying BOTH barrier
        // currencies (ADR 0011 §1.5). `flags` is the GL lowering —
        // the glMemoryBarrier bitmask the planner derives; `barriers` is the
        // neutral truth — the handle-resolved per-resource transitions for
        // the same batch. The GL backend executes `flags` and ignores
        // `barriers`; an explicit-barrier backend (Vulkan) lowers each
        // RHI::Barrier to a VkImageMemoryBarrier2 / VkBufferMemoryBarrier2
        // in one vkCmdPipelineBarrier2 and ignores `flags`. Exactly one of
        // the two is authoritative per backend — neither backend may consult
        // both.
        virtual void IssueBarrierBatch(MemoryBarrierFlags flags, std::span<const RHI::Barrier> barriers) = 0;

        // New methods for render graph
        virtual void BindDefaultFramebuffer() = 0;
        virtual void BlitFramebufferToDefault(RHI::ResourceHandle srcFramebuffer, u32 width, u32 height) = 0;

        // ---------------------------------------------------------------------
        // The bind family (issue #691; u32 siblings deleted in
        // item 4).
        //
        // The migration ran in two steps because the conversion is asymmetric:
        // `handle -> native` is a registry lookup, but `native -> handle` is not
        // recoverable, so a facade taking only handles could not serve a caller
        // still holding a u32. Both spellings coexisted until the last such
        // caller migrated; then the u32 forms went and `facade_native_id_params`
        // reached zero (ADR 0011 step-3 amendments).
        //
        // Every implementation resolves through Utils::ResolveNative INSIDE
        // Platform/<Backend>/. A resolving helper in Renderer/ would be simpler
        // and would breach the boundary this phase exists to close —
        // RHIBoundaryRatchetTest's backend_resolve_hatch is what keeps it honest.
        //
        // RHI::NullResource unbinds, exactly as a native 0 used to.
        // ---------------------------------------------------------------------
        virtual void BindTexture(u32 slot, RHI::ResourceHandle texture) = 0;
        // The EXPLICIT-SAMPLER form (#691). On GL the slot path
        // samples with the texture OBJECT's parameters and always has — the
        // default body preserves exactly that, so GL and every mock see one
        // entry point. The Vulkan backend overrides it: its "object state"
        // lives in a registry and its samplers come from the sampler heap, so
        // an explicit desc (the ShadowDepthSampler compare state, SSAO's
        // Nearest/Repeat noise) must reach the backend or it silently samples
        // with the inherit state — the heap-off half of the §4f contract.
        virtual void BindTexture(u32 slot, RHI::ResourceHandle texture, const RHI::SamplerDesc& sampler);
        virtual void BindImageTexture(u32 unit, RHI::ResourceHandle texture, u32 mipLevel, bool layered,
                                      u32 layer, RHI::Access access, RHI::Format format) = 0;

        virtual void SetPolygonOffset(f32 factor, f32 units) = 0;
        virtual void EnableMultisampling() = 0;
        virtual void DisableMultisampling() = 0;
        virtual void SetColorMask(bool red, bool green, bool blue, bool alpha) = 0;
        virtual void SetColorMaskForAttachment(u32 attachment, bool red, bool green, bool blue, bool alpha) = 0;

        // Per-attachment blend control (needed for mixed integer/float framebuffer attachments)
        virtual void SetBlendStateForAttachment(u32 attachment, bool enabled) = 0;
        // Per-attachment blend function (needed for weighted-blended OIT — accum/revealage differ)
        virtual void SetBlendFuncForAttachment(u32 attachment, RHI::BlendFactor src, RHI::BlendFactor dst) = 0;

        // GPU-side image copy (used for staging textures to avoid read-write
        // hazards). BOTH operands take handles together, deliberately: every
        // caller is "framebuffer attachment -> persistent texture", so a mixed
        // handle/native overload pair would only exist to serve a half-migrated
        // chain, which is the state this migration is meant to make
        // unrepresentable.
        virtual void CopyImageSubData(RHI::ResourceHandle src, TextureTargetType srcTarget,
                                      RHI::ResourceHandle dst, TextureTargetType dstTarget,
                                      u32 width, u32 height) = 0;
        // Full image copy with source/dest offsets (needed for cubemap face copies).
        // srcZ/dstZ address a cubemap face or a 2D-array layer; x/y offsets are
        // fixed at 0 on both operands.
        //
        // width/height are SOURCE-image texels, and no backend may scale them.
        // That is load-bearing for a mixed compressed/uncompressed pair (the
        // terrain VT tile stage, issue #715): GL's 128-bit view class
        // makes one RGBA32UInt texel one 16-byte BC7 block, so an
        // RGBA32UInt -> BC7 copy passes the source's texel (= block) dimensions
        // and covers 4x width/height in destination texels. A copy that needs
        // x/y offsets goes through CopyImageSubDataRegion below.
        virtual void CopyImageSubDataFull(RHI::ResourceHandle src, TextureTargetType srcTarget, i32 srcLevel, i32 srcZ,
                                          RHI::ResourceHandle dst, TextureTargetType dstTarget, i32 dstLevel, i32 dstZ,
                                          u32 width, u32 height) = 0;
        // The offset-taking sibling of CopyImageSubDataFull — full (x, y, z)
        // addressing on both operands, parameter order mirroring the copy's
        // native spelling. Added for the terrain VT tile stage (issue #715
        // slice 4): each compressed tile lands at a per-tile (dstX, dstY)
        // inside a BC7 cache layer, read from a per-slot (srcX, 0) in the
        // RGBA32UInt staging array.
        //
        // Same block-copy contract as above: width/height are SOURCE-image
        // texels, and no backend may scale them or the offsets. srcX/srcY are
        // source texels (= blocks when the source stands in for compressed
        // payload); when the DEST is block-compressed, dstX/dstY are DEST
        // texels and must be multiples of the 4-texel block edge.
        virtual void CopyImageSubDataRegion(RHI::ResourceHandle src, TextureTargetType srcTarget, i32 srcLevel,
                                            i32 srcX, i32 srcY, i32 srcZ,
                                            RHI::ResourceHandle dst, TextureTargetType dstTarget, i32 dstLevel,
                                            i32 dstX, i32 dstY, i32 dstZ,
                                            u32 width, u32 height) = 0;
        // Copy from currently-bound READ framebuffer to a named texture
        virtual void CopyFramebufferToTexture(RHI::ResourceHandle texture, u32 width, u32 height) = 0;

        // Restrict which color attachments are written to
        virtual void SetDrawBuffers(std::span<const u32> attachments) = 0;
        // Restore all color attachments for drawing (convenience for post-pass cleanup)
        virtual void RestoreAllDrawBuffers(u32 colorAttachmentCount) = 0;

        // Create a TEXTURE_2D_ARRAY *view* aliasing the storage of an existing
        // immutable depth array, but with hardware depth comparison DISABLED, so
        // it can be sampled as a plain sampler2DArray to read raw depth (needed by
        // the PCSS blocker search, which the comparison sampler2DArrayShadow can't
        // provide). Source must be DEPTH_COMPONENT32F immutable storage. Returns
        // RHI::NullResource if the platform lacks texture-view support.
        //
        // (issue #691 — the command-layer bind cache). The view
        // is a DISTINCT GPU object from the array it aliases, so it gets its own
        // identity: ShadowMap holds both, and binding the wrong one is a silent
        // PCSS bug rather than a loud one.
        [[nodiscard]] virtual RHI::ResourceHandle CreateDepthArrayCompareOffViewHandle(RHI::ResourceHandle srcTexture,
                                                                                       u32 numLayers) = 0;
        // Replaces SetTextureParameter(id, GLenum pname, GLint value). `pname`
        // was an open-ended GL enum space, and mirroring it with an
        // RHI::TextureParameterName would have re-exported GL under a new name.
        // Every call site in the engine sets exactly min/mag filter and wrap
        // S/T/R, so two intent-named setters cover all of them; SetTextureWrap
        // applies one mode to all three axes because no call site ever used
        // different modes per axis (WRAP_R is inert on a 2D target).
        virtual void SetTextureFilter(RHI::ResourceHandle texture, RHI::Filter minFilter, RHI::Filter magFilter) = 0;
        virtual void SetTextureWrap(RHI::ResourceHandle texture, RHI::AddressMode wrap) = 0;
        // `sourceFormat` describes the layout of `data` — the CPU-side buffer —
        // NOT the texture's storage format. GL converts on upload, and the
        // engine relies on that: SSAO's noise texture is RG16Float storage fed
        // from RG32Float host data.
        virtual void UploadTextureSubImage2D(RHI::ResourceHandle texture, u32 width, u32 height,
                                             RHI::Format sourceFormat, const void* data) = 0;

        // Occlusion / conditional rendering
        virtual void BeginConditionalRender(RHI::ResourceHandle query) = 0;
        virtual void EndConditionalRender() = 0;

        // =====================================================================
        // Call-site sweep additions (issue #691) — the operations the sweep
        // found the facade had never abstracted at all.
        //
        // Step 1 converted the vocabulary of the 74 virtuals that already
        // existed. This block is the other half of the finding: 84 distinct GL
        // entry points appear across the 313 swept call sites and ~60% of them
        // had NO facade equivalent, so passes reached past it. See ADR 0011's
        // "Amendments from the call-site sweep" for the category table and the
        // reasoning behind each shape.
        // =====================================================================

        // --- Buffer binding points -------------------------------------------
        // The single biggest gap (26 call sites). A 0 id unbinds the point.
        // An invalid handle (RHI::NullResource) unbinds the point — the same
        // "0 unbinds" contract the deleted u32 forms had, now spelled so it
        // cannot be confused with a real object.
        virtual void BindUniformBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) = 0;
        virtual void BindStorageBuffer(u32 bindingPoint, RHI::ResourceHandle buffer) = 0;

        // --- Program / VAO / framebuffer binding ------------------------------
        // The POD command dispatcher holds a program identity by design (it
        // resolves materials at build time and has no Ref<Shader> on hand), so
        // Shader::Bind() cannot serve it. RHI::NullResource unbinds.
        virtual void BindShaderProgram(RHI::ResourceHandle program) = 0;
        virtual void BindVertexArrayRaw(RHI::ResourceHandle vertexArray) = 0;
        // RHI::NullResource selects the default framebuffer — same as
        // BindDefaultFramebuffer().
        virtual void BindFramebuffer(RHI::ResourceHandle framebuffer) = 0;

        // --- Draws from already-bound geometry --------------------------------
        // Distinct from the DrawIndexedRaw(vaoID, ...) family above, which binds
        // its own VAO: CommandDispatch keeps a redundant-bind cache, so a draw
        // that re-binds would defeat it. This is also the NATIVE Vulkan shape
        // (vkCmdBindIndexBuffer then vkCmdDrawIndexed) — the combined
        // bind-and-draw form is the less portable of the two. Topology and index
        // width are explicit rather than hard-coded to triangles / u32.
        virtual void DrawBoundIndexed(RHI::PrimitiveTopology topology, u32 indexCount,
                                      RHI::IndexType indexType, u32 baseIndex) = 0;
        virtual void DrawBoundIndexedInstanced(RHI::PrimitiveTopology topology, u32 indexCount,
                                               RHI::IndexType indexType, u32 baseIndex,
                                               u32 instanceCount) = 0;
        virtual void DrawBoundArrays(RHI::PrimitiveTopology topology, u32 firstVertex, u32 vertexCount) = 0;
        // Split out rather than folded into a patch-draw variant: the
        // tessellation call sites set the count once and then draw many times.
        virtual void SetPatchVertexCount(u32 patchVertices) = 0;

        // --- Pipeline state the facade was missing -----------------------------
        virtual void SetFrontFace(RHI::FrontFace face) = 0;
        virtual void SetBlendFuncSeparate(RHI::BlendFactor srcRGB, RHI::BlendFactor dstRGB,
                                          RHI::BlendFactor srcAlpha, RHI::BlendFactor dstAlpha) = 0;
        virtual void SetClearDepth(f32 depth) = 0;

        // --- Named framebuffers ------------------------------------------------
        // SetDrawBuffers/RestoreAllDrawBuffers above act on the CURRENTLY BOUND
        // framebuffer; every swept call site names a specific one through DSA.
        //
        // `attachmentIndices[i]` is the attachment written by draw slot i, or
        // RHI::NoAttachment for "slot i writes nowhere" — which is not an index
        // and which both backends need (GL_NONE / VK_ATTACHMENT_UNUSED).
        // DecalRenderPass depends on it to steer a decal into exactly one
        // G-Buffer attachment.
        virtual void AttachFramebufferColorTexture(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                                   RHI::ResourceHandle texture, u32 mipLevel) = 0;
        virtual void AttachFramebufferDepthTexture(RHI::ResourceHandle framebuffer, RHI::ResourceHandle texture,
                                                   u32 mipLevel) = 0;
        [[nodiscard("Store this!")]] virtual bool IsFramebufferComplete(RHI::ResourceHandle framebuffer) = 0;
        virtual void SetFramebufferDrawAttachments(RHI::ResourceHandle framebuffer,
                                                   std::span<const u32> attachmentIndices) = 0;
        // The identity list { 0, 1, ... count-1 } — "draw to every colour
        // attachment this framebuffer has". Nine call sites were open-coding the
        // same std::array + fill loop + span; that is the named-framebuffer
        // counterpart of RestoreAllDrawBuffers(u32) above, which already existed
        // for the BOUND framebuffer. Restoring a narrower list than the target
        // actually has silently drops later shader outputs (PBR_MultiLight's
        // motion vector at location 3, breaking TAA), which is exactly the kind
        // of off-by-one an open-coded loop invites.
        virtual void RestoreAllFramebufferDrawAttachments(RHI::ResourceHandle framebuffer,
                                                          u32 colorAttachmentCount) = 0;
        virtual void SetFramebufferReadAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex) = 0;
        virtual void ClearFramebufferColorAttachment(RHI::ResourceHandle framebuffer, u32 attachmentIndex,
                                                     const glm::vec4& color) = 0;
        virtual void ClearFramebufferDepth(RHI::ResourceHandle framebuffer, f32 depth) = 0;
        // RHI::NullResource on either side names the DEFAULT framebuffer, which
        // is how the backbuffer blit is spelled.
        virtual void BlitFramebuffer(RHI::ResourceHandle srcFramebuffer, RHI::ResourceHandle dstFramebuffer,
                                     i32 srcX0, i32 srcY0, i32 srcX1, i32 srcY1,
                                     i32 dstX0, i32 dstY0, i32 dstX1, i32 dstY1,
                                     RHI::BlitAspect aspect, RHI::Filter filter) = 0;

        // --- Raw buffer lifecycle ----------------------------------------------
        // UniformBuffer / StorageBuffer wrap *their own* buffers; VirtualMeshRegistry
        // hand-rolls a vertex/index arena plus a persistent-mapped upload ring and
        // needs the primitives directly.
        // Mutable storage — re-callable to resize.
        virtual void AllocateBufferStorage(RHI::ResourceHandle buffer, u64 sizeBytes, RHI::MemoryResidency residency) = 0;
        // Immutable storage + a persistent, coherent WRITE mapping in one step:
        // the only mapping mode the engine uses, so splitting it would invite a
        // storage/mapping flag mismatch that GL only reports at map time.
        // Returns the CPU pointer, or nullptr if the mapping failed.
        virtual void* AllocatePersistentUploadStorage(RHI::ResourceHandle buffer, u64 sizeBytes) = 0;
        virtual void UnmapBuffer(RHI::ResourceHandle buffer) = 0;
        virtual void UploadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, const void* data) = 0;
        virtual void ReadBufferSubData(RHI::ResourceHandle buffer, u64 offsetBytes, u64 sizeBytes, void* dest) = 0;
        virtual void CopyBufferSubData(RHI::ResourceHandle srcBuffer, RHI::ResourceHandle dstBuffer,
                                       u64 srcOffsetBytes, u64 dstOffsetBytes, u64 sizeBytes) = 0;
        virtual void ClearBufferUInt(RHI::ResourceHandle buffer, u32 value) = 0;
        virtual void ClearBufferFloat(RHI::ResourceHandle buffer, f32 value) = 0;

        // ---------------------------------------------------------------------
        // The resource creators (issue #691 added them beside
        // u32-returning siblings; item 4 deleted those siblings).
        //
        // These are the migration ROOT: the ids that passes used to hold came
        // from here, and ImportTexture -> ResolveTexture -> every pass sits
        // downstream.
        //
        // Still spelled `...Handle` even though the u32 forms are gone. The move
        // called the suffix temporary, expecting to take the plain name back
        // here; on arriving, renaming ~40 call sites bought no semantic gain and
        // `CreateTexture2DHandle` reads as "create a texture, get its identity",
        // which is what it does. Revisit when ViewHandle lands and
        // the create family is reshaped anyway.
        //
        // Each Delete* sibling must BOTH destroy the object and retire its
        // identity — skip the second and a handle to a destroyed object keeps
        // resolving to a name the driver may reissue.
        // ---------------------------------------------------------------------
        [[nodiscard]] virtual RHI::ResourceHandle CreateTexture2DHandle(u32 width, u32 height, RHI::Format internalFormat) = 0;
        [[nodiscard]] virtual RHI::ResourceHandle CreateTextureCubemapHandle(u32 width, u32 height, RHI::Format internalFormat) = 0;
        [[nodiscard]] virtual RHI::ResourceHandle CreateFramebufferHandle() = 0;
        [[nodiscard]] virtual RHI::ResourceHandle CreateBufferHandle() = 0;
        [[nodiscard]] virtual RHI::ResourceHandle CreateVertexArrayHandle() = 0;
        virtual void DeleteTexture(RHI::ResourceHandle texture) = 0;
        virtual void DeleteFramebuffer(RHI::ResourceHandle framebuffer) = 0;
        virtual void DeleteBuffer(RHI::ResourceHandle buffer) = 0;
        virtual void DeleteVertexArray(RHI::ResourceHandle vertexArray) = 0;
        virtual void SetVertexArrayIndexBuffer(RHI::ResourceHandle vertexArray, RHI::ResourceHandle indexBuffer) = 0;

        // --- Texture clear / upload / readback -----------------------------------
        // Two clears rather than one type-punned value pointer, mirroring
        // VkClearColorValue's float/uint union members. `mipLevel` clears one
        // level of every layer/face, matching glClearTexImage.
        virtual void ClearTextureFloat(RHI::ResourceHandle texture, u32 mipLevel, const glm::vec4& color) = 0;
        virtual void ClearTextureUInt(RHI::ResourceHandle texture, u32 mipLevel, u32 value) = 0;
        // Offset overloads of the whole-image UploadTextureSubImage2D above.
        // `sourceFormat` is the HOST buffer's layout, not the texture's storage
        // format — see ADR 0011 amendment (4).
        virtual void UploadTextureSubImage2D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset,
                                             u32 width, u32 height,
                                             RHI::Format sourceFormat, const void* data) = 0;
        virtual void UploadTextureSubImage3D(RHI::ResourceHandle texture, i32 xOffset, i32 yOffset, i32 zOffset,
                                             u32 width, u32 height, u32 depth,
                                             RHI::Format sourceFormat, const void* data) = 0;
        // Readbacks return success rather than leaving the caller to ask the
        // backend for an error: GL's error model is a global sticky flag and
        // Vulkan's is a per-call result, so exposing either would force the
        // other backend to fake it. ThumbnailCapture's glGetError() disappears
        // with no replacement (ADR 0011 amendment (7)).
        // A stale handle resolves to 0, and a readback of texture 0 fails —
        // so this reports false rather than silently handing back an
        // uninitialised buffer. LightProbeBaker depends on that: its
        // coefficients are PERSISTED, so a bad read must abandon the bake, not
        // write wrong lighting to disk.
        [[nodiscard("Store this!")]] virtual bool ReadTextureImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                                   RHI::Format destFormat,
                                                                   sizet destSizeBytes, void* dest) = 0;
        [[nodiscard("Store this!")]] virtual bool ReadTextureSubImage(RHI::ResourceHandle texture, u32 mipLevel,
                                                                      i32 x, i32 y, i32 z,
                                                                      u32 width, u32 height, u32 depth,
                                                                      RHI::Format destFormat,
                                                                      sizet destSizeBytes, void* dest) = 0;
        virtual void GetTextureDimensions(RHI::ResourceHandle texture, u32 mipLevel, u32& outWidth, u32& outHeight) = 0;
        // The storage format of one mip, decoded into the neutral diagnostic
        // vocabulary (#810). False when the handle does not resolve, the mip
        // has no storage, or the backend cannot decode the format — in which
        // case `out` is left untouched and the caller must refuse rather than
        // read with a guessed layout. This is what lets olo_render_probe_pixel
        // and olo_render_target_stats work on both backends: their old GL arm
        // asked glGetTextureLevelParameteriv(GL_TEXTURE_INTERNAL_FORMAT)
        // directly, which is the one thing a Vulkan session cannot do.
        [[nodiscard("Store this!")]] virtual bool QueryTextureFormat(RHI::ResourceHandle texture, u32 mipLevel,
                                                                     RHI::TextureFormatInfo& out) = 0;
        // Orders a texture's use as a render target against a subsequent sample
        // of it in the same pass. Vulkan expresses this as a pipeline barrier.
        virtual void TextureBarrier() = 0;

        // --- Queries --------------------------------------------------------------
        //
        // Queries carry an IDENTITY like every other GPU object (issue #691 step
        // 3, item 4). `RHI::ResourceKind::Query` already existed in the registry
        // when the mint landed; this is what makes that enumerator reachable.
        //
        // The recycled-name hazard is concrete here, not theoretical:
        // OcclusionQueryPool hands a query issued in frame N to
        // BeginConditionalRender in frame N+1. Re-Initialize() in between (a
        // maxQueries change) frees the names, and GL is then free to reissue one
        // of them to an unrelated query — under which a draw would be gated on
        // someone else's occlusion result, silently. A retired handle resolves
        // to 0 instead, and BeginConditionalRender(0) is a no-op.
        //
        // CreateQueries does NOT hand its out-span to the driver. §4's `.data()`
        // trap is exactly this shape: a span whose element type changed from a
        // 4-byte name to an 8-byte handle still compiles when passed to
        // glCreateQueries, and GL would write names over half of each handle.
        // The backend creates into its own native array and writes handles back.
        virtual void CreateQueries(RHI::QueryType type, std::span<RHI::ResourceHandle> outQueries) = 0;
        // Destroys the objects AND retires their identities — both halves, per
        // §4's "Delete* needs two changes".
        virtual void DeleteQueries(std::span<const RHI::ResourceHandle> queries) = 0;
        virtual void BeginQuery(RHI::QueryType type, RHI::ResourceHandle query) = 0;
        virtual void EndQuery(RHI::QueryType type) = 0;
        // Stamps the GPU clock into a QueryType::Timestamp query at this point
        // in the command stream — the write-shaped member of the query family
        // (GL glQueryCounter; Vulkan vkCmdWriteTimestamp into the query's pool
        // slot), never bracketed by Begin/EndQuery. GetQueryResultU64 on a
        // timestamp query returns NANOSECONDS on both backends: the Vulkan arm
        // owes the timestampPeriod scaling so GPUPassTimerPool's subtraction
        // math is backend-blind (#691).
        virtual void WriteTimestamp(RHI::ResourceHandle query) = 0;
        [[nodiscard("Store this!")]] virtual bool IsQueryResultAvailable(RHI::ResourceHandle query) = 0;
        [[nodiscard("Store this!")]] virtual u32 GetQueryResultU32(RHI::ResourceHandle query) = 0;
        [[nodiscard("Store this!")]] virtual u64 GetQueryResultU64(RHI::ResourceHandle query) = 0;

        // --- Fences ---------------------------------------------------------------
        // An opaque u64 rather than a handle type: GLsync is a pointer and
        // VkFence a 64-bit handle, and FrameResourceManager stores one per
        // in-flight frame. 0 means "no fence" / creation failed.
        [[nodiscard("Store this!")]] virtual u64 CreateFence() = 0;
        [[nodiscard("Store this!")]] virtual RHI::FenceStatus ClientWaitFence(u64 fence, u64 timeoutNanoseconds) = 0;
        [[nodiscard("Store this!")]] virtual bool IsFenceSignaled(u64 fence) = 0;
        virtual void DestroyFence(u64 fence) = 0;

        // --- Debug markers ----------------------------------------------------------
        virtual void PushDebugGroup(u32 id, std::string_view label) = 0;
        virtual void PopDebugGroup() = 0;

        // --- Device ------------------------------------------------------------------
        // Full CPU/GPU sync. Expensive by construction — the two callers are an
        // IBL precompute and a virtual-geometry ring-buffer wrap.
        virtual void WaitForDeviceIdle() = 0;

        // MSAA capability caps. Three separate queries because GL reports them
        // separately and they genuinely differ on some drivers: a format may
        // support more colour samples than depth samples, and GBuffer must pick
        // a count both attachments can carry.
        [[nodiscard("Store this!")]] virtual u32 GetMaxFramebufferSamples() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetMaxColorTextureSamples() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetMaxDepthTextureSamples() const = 0;

        // Name-keyed default-block uniform. THIS IS THE ONE VIRTUAL A VULKAN
        // BACKEND CANNOT IMPLEMENT FAITHFULLY — SPIR-V has push constants and
        // UBO members, not a queryable default uniform block. It has exactly one
        // call site (CommandDispatch::DrawInfiniteGrid's u_GridScale) and the pipeline layer
        // must fold that into a UBO and delete this. Recorded deliberately in
        // ADR 0011 amendment (9) rather than left to surprise a later bring-up.
        virtual void SetProgramUniformFloat(RHI::ResourceHandle program, std::string_view name, f32 value) = 0;

        // GPU capability queries

        // True when the backend can service resource creation and draws *right
        // now*. This is not "has Renderer::Init run" — it asks whether the
        // underlying device is usable in this process at all.
        //
        // It exists because asset code legitimately runs without one: headless
        // harnesses (Functional tests, asset preprocessors) load fonts and
        // meshes for their CPU-side data and never render them. Those paths must
        // build the data and defer the GPU upload rather than crash, so they
        // need to ask the question before calling Texture2D::Create.
        //
        // Replaces a `glad_glCreateTextures != nullptr` probe in
        // SlugFontProcessor — reaching into the GL loader's symbol table is a
        // real need expressed unportably, and it is invisible to the boundary
        // ratchet's `gl[A-Z]` scan because the character after `gl` is `a`
        // (issue #691).
        [[nodiscard("Store this!")]] virtual bool IsDeviceAvailable() const = 0;

        [[nodiscard("Store this!")]] virtual u32 GetMaxUniformBlockSize() const = 0;
        // True when the driver exposes 64-bit shader integers AND 64-bit shader
        // atomics (GL_ARB_gpu_shader_int64 + GL_NV_shader_atomic_int64), which
        // lets the virtualized-geometry software rasterizer resolve its
        // visibility buffer with a single atomicMin on a packed uint64_t instead
        // of the portable two-pass 2x32 scheme (issue #629). Cached at Init.
        [[nodiscard("Store this!")]] virtual bool SupportsInt64ShaderAtomics() const = 0;
        // True when the backend can run task/mesh-shader graphics pipelines
        // right now — the capability gate for DrawMeshTasks (issue #813).
        // Vulkan answers from the logical device's ENABLED VK_EXT_mesh_shader
        // features (taskShader AND meshShader, the SupportsInt64ShaderAtomics
        // enabled-not-just-supported rule); OpenGL always answers false —
        // GL_NV_mesh_shader is deliberately out of scope. The decision is
        // refuse-or-degrade, made explicitly by the caller, never silent.
        [[nodiscard("Store this!")]] virtual bool SupportsMeshShaders() const = 0;

        [[nodiscard("Store this!")]] static API GetAPI()
        {
            return s_API;
        }

        // Backend selection is a RUNTIME switch (ADR 0011 §2): parsed from `--rhi=` /
        // the config fallback in Application's constructor. HARD ordering contract:
        // this must run BEFORE Window::Create — WindowsWindow::Init reads GetAPI()
        // ahead of glfwCreateWindow (client-API hint), so a late set selects the
        // wrong window kind. Note RenderCommand::s_RendererAPI is ALSO created at
        // static init from the default value; device bring-up tolerates that because the
        // Vulkan path never routes through RenderCommand (Renderer::Init is skipped
        // entirely), but the execution layer must re-create it after selection. Never call this
        // after a window or context exists.
        static void SetAPI(API api)
        {
            s_API = api;
        }

        static Scope<RendererAPI> Create();

      private:
        static API s_API;
    };

} // namespace OloEngine
