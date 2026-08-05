#pragma once

// =============================================================================
// RHITypes.h — the API-neutral vocabulary of the render hardware interface.
//
// Issue #691 Phase 1, ADR 0011 (docs/adr/0011-rhi-neutral-resource-and-binding-model.md).
//
// Written declaration-only in Phase 1, so that the Phase 2 sweep of ~313 raw
// `glXxx()` call sites had a fixed target to convert *toward* instead of
// inventing a vocabulary one file at a time and discovering the disagreements at
// merge time. **That is history now** — `ResourceHandle` below is the live
// identity currency: minted by RHI::ResourceRegistry, carried by the
// Platform/OpenGL resource classes, RenderCommand's handle-taking siblings, the
// render graph, and the framebuffer attachment getters. `ViewHandle` /
// `HeapOffset` are still forward-looking and land in Phase 3 as a matched pair
// (ADR 0011 amendment (11)).
//
// Two hard rules, both enforced by RHIBoundaryRatchetTest:
//
//   1. Nothing in `OloEngine/Renderer/RHI/` may include a backend header
//      (`glad/gl.h`, `vulkan/vulkan.h`, …) or USE a backend type
//      (`GLenum`, `VkImage`, …). If you need one, you are writing backend code
//      and it belongs in `Platform/<Backend>/`. Naming them in a comment — as
//      this very line does — is fine and necessary; the test blanks comments
//      and string literals before checking, precisely so the rule can be
//      documented where it applies.
//   2. The enums below are *engine* enums. They are deliberately NOT bit-equal
//      to any backend's constants — a backend converts explicitly in its own
//      translation unit. Making them alias GL values "for free conversion" is
//      how `RendererAPI` ended up with `GLenum` in its virtuals in the first
//      place.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <spdlog/fmt/fmt.h>

#include <limits>
#include <string>

namespace OloEngine::RHI
{
    // -------------------------------------------------------------------------
    // Identity — "which GPU object is this?"
    //
    // Replaces the `u32 GetRendererID()` / `using RendererID = u32` currency at
    // the engine boundary. The {Index, Generation} shape deliberately mirrors
    // RGTextureHandle / RGBufferHandle / RGFramebufferHandle
    // (Renderer/ResourceHandle.h) so the render graph's existing handle
    // discipline generalises rather than acquiring a second scheme.
    //
    // The generation is not decoration. GL recycles object names, so today two
    // genuinely different objects can compare equal through
    // `Texture::operator==` (which compares GetRendererID()) when one was
    // deleted and another created. A generation distinguishes them; a raw u32
    // cannot. TransientPool's alias reporting depends on exactly that
    // distinction ("did these two plan entries get the same object, or a
    // recycled name?").
    // -------------------------------------------------------------------------
    // Tag-templated so the two identity levels below share one definition and
    // cannot drift, while staying MUTUALLY NON-CONVERTIBLE — distinct Tag types
    // make Handle<ResourceTag> and Handle<ViewTag> unrelated classes, which is
    // the property that stops a resource id being passed where a view id is
    // wanted. RHIBoundaryRatchetTest static_asserts both directions.
    //
    // `InvalidIndex` keeps its PascalCase spelling rather than the k-prefix used
    // for some file-scope constants elsewhere: the sibling handle types this
    // deliberately mirrors (RGTextureHandle / RGBufferHandle /
    // RGFramebufferHandle) all spell it `InvalidIndex`, and matching the types
    // §1.1 says we mirror beats matching a convention that is itself the
    // minority in Renderer/ (22 k-prefixed vs 291 PascalCase static constexpr).
    template<typename Tag>
    struct Handle
    {
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();

        u32 Index = InvalidIndex;
        u32 Generation = 0;

        // constexpr so the invariant can be asserted at compile time — see
        // RHIBoundaryRatchetTest's ResourceHandleCannotBeSpelledAsANativeId,
        // which needs it to pin that a parenthesized-aggregate-initialised
        // handle (C++20 P0960) carries Generation == 0 and is therefore inert.
        [[nodiscard]] constexpr auto IsValid() const -> bool
        {
            return Index != InvalidIndex && Generation > 0;
        }

        [[nodiscard]] auto operator==(const Handle& other) const -> bool = default;
    };

    struct ResourceTag;
    struct ViewTag;

    using ResourceHandle = Handle<ResourceTag>;

    // Sentinel spelling for "no resource". Reads better than a bare `{}` at a
    // call site that used to pass a literal 0, and is the value every
    // `handle.IsValid()` guard is testing against.
    inline constexpr ResourceHandle NullResource{};

    // A 64-bit key identifying a handle uniquely, for hashing and for the sort
    // keys that used to pack a raw renderer ID. The (Generation << 32) | Index
    // packing deliberately matches what std::hash<RGTextureHandle> and friends
    // already do in Renderer/ResourceHandle.h — §1.1 says this type mirrors
    // those, and that has to include how it is keyed or the graph acquires a
    // second, subtly different scheme after all.
    //
    // For a *bucketing* key (a radix-sort field, a material hash) `Index` alone
    // is usually the better choice: it is dense from zero where a driver-assigned
    // name is arbitrary, and two live handles can never share one.
    template<typename Tag>
    [[nodiscard]] constexpr u64 HashKey(Handle<Tag> handle) noexcept
    {
        return (static_cast<u64>(handle.Generation) << 32u) | static_cast<u64>(handle.Index);
    }

    // -------------------------------------------------------------------------
    // View identity — "which *view* of which resource?"
    //
    // The second identity level, and the one that owns a heap slot. Under
    // VK_EXT_descriptor_heap the sampler heap is separate from the resource
    // heap, so a texture needs one resource-heap slot per VIEW — per
    // (resource, subresource range, format reinterpretation) — not one per
    // sampler. See ViewDesc in RHIResources.h.
    //
    // A separate type rather than reusing ResourceHandle, because the
    // relationship is genuinely one-to-many and the engine already proves it:
    // RendererAPI::CreateDepthArrayCompareOffView exists so one depth array can
    // be read both as sampler2DArrayShadow and as a plain sampler2DArray for the
    // PCSS blocker search. One resource, two views, two heap slots.
    // -------------------------------------------------------------------------
    using ViewHandle = Handle<ViewTag>;

    // -------------------------------------------------------------------------
    // Binding address — "what integer does the shader index the heap with?"
    //
    // This is a u32 wrapper ON PURPOSE and must stay layout-compatible with u32:
    // the value is written into a UBO/SSBO field and read by GLSL as an array
    // index, so it cannot be an opaque type. The wrapper buys type-safety at the
    // C++ boundary only, which is the whole point of having it.
    //
    // FETCH IT, DO NOT STORE IT. The only sanctioned way to obtain one is
    // `Heap::OffsetOf(ViewHandle)` (RHIResources.h), which validates the view's
    // generation first. A HeapOffset cached in a material or a pass member
    // outlives nothing in particular: a persistent view's offset is stable, but
    // a FrameTransient one is re-allocated every frame from the ring, and under
    // RenderGraph::WriteNewVersion aliasing the same physical object can hold
    // two different offsets within one frame. Fetching at the point of write
    // turns a stale offset into a cheap CPU-side assert instead of a wrong
    // sample that only the heap's poison mode would eventually reveal.
    // -------------------------------------------------------------------------
    struct HeapOffset
    {
        static constexpr u32 Invalid = std::numeric_limits<u32>::max();

        u32 Value = Invalid;

        [[nodiscard]] constexpr auto IsValid() const -> bool
        {
            return Value != Invalid;
        }

        [[nodiscard]] auto operator==(const HeapOffset& other) const -> bool = default;
    };
    static_assert(sizeof(HeapOffset) == sizeof(u32),
                  "HeapOffset is written into shader-visible buffers as a plain u32");

    // -------------------------------------------------------------------------
    // Native handle — "what do I pass to the driver?"
    //
    // Deliberately NOT reachable through a resource's ordinary interface. The
    // only accessor is spelled `GetNativeHandleForDebug` (see RHIResources.h),
    // because the naming is the enforcement mechanism: such a call sitting in a
    // render pass is a self-evident smell in review, where `GetRendererID()`
    // reads as ordinary and is how GL spread to 42 files.
    //
    // Legitimate consumers are the introspection tools in Renderer/Debug/ and
    // the MCP capture endpoints they back. RHIBoundaryRatchetTest baselines uses
    // outside Renderer/Debug/ and Platform/ at zero.
    // -------------------------------------------------------------------------
    enum class Backend : u8
    {
        None = 0,
        OpenGL,
        Vulkan,
    };

    struct NativeHandle
    {
        u64 Value = 0;
        Backend Owner = Backend::None;
    };

    // -------------------------------------------------------------------------
    // Formats.
    //
    // Superset of the engine's existing `ImageFormat` (Renderer/Texture.h) and
    // `RGResourceFormat` (Renderer/ResourceHandle.h), which Phase 2 collapses
    // into this one. Kept separate for now so this header stays declaration-only
    // and no existing serialised enum value moves.
    //
    // NOTE for Phase 2: `ImageFormat`'s integer values are persisted in scene
    // YAML and asset packs, so its members may only ever be appended. Mapping
    // ImageFormat -> RHI::Format must therefore be an explicit switch, never a
    // static_cast.
    // -------------------------------------------------------------------------
    enum class Format : u16
    {
        Unknown = 0,

        // 8-bit
        R8UNorm,
        R8UInt,
        RG8UNorm,
        RGB8UNorm,
        RGBA8UNorm,
        RGBA8SRGB,

        // 16-bit
        R16UInt,
        RG16UInt,
        RG16Float,
        RGBA16Float,

        // 32-bit
        R32Float,
        R32Int,
        R32UInt,
        RG32Float,
        RGB32Float,
        RGBA32Float,

        // Depth / stencil
        D24UNormS8UInt,
        D32Float,

        // Block-compressed (issue #440)
        BC5UNorm,
        BC6HUFloat,
        BC7UNorm,
        BC7SRGB,
    };

    [[nodiscard]] constexpr bool IsCompressed(Format format) noexcept
    {
        return format == Format::BC5UNorm || format == Format::BC6HUFloat ||
               format == Format::BC7UNorm || format == Format::BC7SRGB;
    }

    [[nodiscard]] constexpr bool IsDepthStencil(Format format) noexcept
    {
        return format == Format::D24UNormS8UInt || format == Format::D32Float;
    }

    // -------------------------------------------------------------------------
    // Pipeline state.
    //
    // These replace the `GLenum` parameters currently declared in RendererAPI's
    // own virtual interface: SetBlendFunc(GLenum, GLenum), SetDepthFunc(GLenum),
    // SetStencilFunc/SetStencilOp(GLenum...), SetCullFace(GLenum),
    // SetPolygonMode(GLenum, GLenum), SetBlendEquation(GLenum),
    // BindImageTexture(..., GLenum access, GLenum format).
    //
    // That leakage is the single highest-leverage thing to fix, and it must go
    // FIRST in Phase 2 (ADR 0011 §1.7): 40 files outside Platform/OpenGL/
    // include <glad/gl.h> while making zero GL calls, purely to name the GL_*
    // constants these signatures demand. RendererAPI.h itself includes
    // <glad/gl.h>, so until these are gone, removing a per-file include
    // proves nothing — the symbols still arrive transitively.
    // -------------------------------------------------------------------------
    enum class CompareOp : u8
    {
        Never = 0,
        Less,
        Equal,
        LessOrEqual,
        Greater,
        NotEqual,
        GreaterOrEqual,
        Always,
    };

    enum class BlendFactor : u8
    {
        Zero = 0,
        One,
        SrcColor,
        OneMinusSrcColor,
        DstColor,
        OneMinusDstColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstAlpha,
        OneMinusDstAlpha,
        ConstantColor,
        OneMinusConstantColor,
        SrcAlphaSaturate,
    };

    enum class BlendOp : u8
    {
        Add = 0,
        Subtract,
        ReverseSubtract,
        Min,
        Max,
    };

    enum class StencilOp : u8
    {
        Keep = 0,
        Zero,
        Replace,
        IncrementClamp,
        DecrementClamp,
        Invert,
        IncrementWrap,
        DecrementWrap,
    };

    enum class CullMode : u8
    {
        None = 0,
        Front,
        Back,
        FrontAndBack,
    };

    enum class FrontFace : u8
    {
        CounterClockwise = 0,
        Clockwise,
    };

    enum class PolygonMode : u8
    {
        Fill = 0,
        Line,
        Point,
    };

    enum class PrimitiveTopology : u8
    {
        TriangleList = 0,
        TriangleStrip,
        LineList,
        LineStrip,
        PointList,
        PatchList, ///< tessellation; patch control-point count lives in PipelineDesc
    };

    // -------------------------------------------------------------------------
    // Sampler state — added in Phase 2 (ADR 0011 amendment, see §1.7).
    //
    // Phase 1 modelled sampler state as two bools on SamplerDesc
    // (`LinearFilter` / `ClampToEdge`), which was enough to describe the
    // *typical* texture but not enough to replace the call sites Phase 2 had to
    // sweep: RendererAPI::SetTextureParameter(id, GLenum pname, GLint value)
    // took an open-ended GL enum space, and SSAORenderPass's noise texture uses
    // the combination the bools cannot express — Nearest filtering with Repeat
    // wrapping.
    //
    // Mirroring GL's pname space with an RHI::TextureParameterName would have
    // re-exported GL under a new spelling. Instead the call sites decompose into
    // two intent-named setters (SetTextureFilter / SetTextureWrap) over these
    // two enums, which is a complete description of every use in the engine:
    // min/mag filter and wrap S/T/R, and every call site sets one wrap value for
    // all axes.
    // -------------------------------------------------------------------------
    enum class Filter : u8
    {
        Nearest = 0,
        Linear,
    };

    enum class AddressMode : u8
    {
        Repeat = 0,
        MirroredRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    // The colour sampled outside a ClampToBorder texture. ENUMERATED, not a
    // float4, because that is the portable subset: Vulkan's VkBorderColor admits
    // only these three float variants without
    // VK_EXT_custom_border_color, so a float4 here would be a field the neutral
    // model cannot honour on the backend it exists to reach.
    //
    // It has to exist at all because a bindless descriptor carries its own
    // sampler object, and a sampler object's border defaults to TRANSPARENT
    // BLACK while the engine's depth arrays set an OPAQUE WHITE one
    // (OpenGLTexture2DArray) so that lookups outside a shadow cascade read as
    // "lit" rather than "fully shadowed". Minting a shadow descriptor without
    // this reproduces the texture's wrap mode but not its border, and the error
    // shows up only at cascade edges — a plausible frame, not an obvious one.
    enum class BorderColor : u8
    {
        TransparentBlack = 0, ///< (0,0,0,0) — the GL/VK default
        OpaqueBlack,          ///< (0,0,0,1)
        OpaqueWhite,          ///< (1,1,1,1) — "outside the shadow map is lit"
    };

    // Index buffer element width. Added in Phase 2 so the POD draw commands in
    // Commands/RenderCommand.h can describe their index buffer without a
    // GLenum field; the direct Vulkan counterpart is VkIndexType.
    enum class IndexType : u8
    {
        UInt16 = 0,
        UInt32,
    };

    // -------------------------------------------------------------------------
    // Access — the unified barrier lattice.
    //
    // ADR 0011 §1.5. RenderGraph::ResourceTransition currently types its
    // transition as `RGWriteUsage FromUsage` -> `RGReadUsage ToUsage`, which
    // structurally CANNOT express a write->write transition. The barrier planner
    // does emit WAW barriers, but BuildResourceTransitions then finds no read
    // declaration on the consumer and silently falls back to
    // `ToUsage = RGReadUsage::ShaderSample`.
    //
    // On GL that is harmless — the flags come from PlannedBarrier::Flags, which
    // the planner derived correctly from the *write* usage, so the bogus ToUsage
    // is never read. On Vulkan, a backend deriving (dstStageMask, dstAccessMask,
    // newLayout) from ToUsage would lower a storage-image WAW into
    // SHADER_READ_ONLY_OPTIMAL with a read-only access mask: wrong layout, wrong
    // access, and silent.
    //
    // This single enum covers reads, writes and the initial no-access state, so
    // a transition record can express any pair. RGReadUsage / RGWriteUsage stay
    // as the *builder's* declaration vocabulary (passes do genuinely declare
    // reads and writes separately) — it is the *transition record* that unifies.
    //
    // There is deliberately NO image-layout member anywhere in the RHI. Layout
    // is a pure function of Access (ShaderSampleRead -> SHADER_READ_ONLY_OPTIMAL,
    // ColorAttachmentWrite -> COLOR_ATTACHMENT_OPTIMAL, StorageWrite -> GENERAL,
    // TransferWrite -> TRANSFER_DST_OPTIMAL, ...), so the Vulkan backend derives
    // it. Hoisting layout up here would leak Vulkan into the render graph for no
    // gain.
    // -------------------------------------------------------------------------
    enum class Access : u8
    {
        // The resource has no defined contents. First use of a transient this
        // frame, and the state a Vulkan backend uses to discard rather than
        // preserve. Today `ProducerPass == "external"` defaults FromUsage to
        // RenderTarget instead, which is wrong for a genuine first use — see
        // ADR 0011 §1.5.
        Undefined = 0,

        IndirectArgsRead,
        IndexRead,
        VertexAttributeRead,
        UniformRead,

        ShaderSampleRead, ///< sampled through a sampler (texture fetch)
        StorageRead,      ///< imageLoad / SSBO load
        StorageWrite,     ///< imageStore / SSBO store
        StorageReadWrite, ///< both in the same pass

        ColorAttachmentRead,
        ColorAttachmentWrite,
        DepthStencilAttachmentRead,
        DepthStencilAttachmentWrite,
        InputAttachmentRead,

        TransferRead,
        TransferWrite,

        // Clear is intentionally split. RGWriteUsage::Clear conflates
        // clear-as-a-load-op (free, folded into the render pass) with an
        // explicit vkCmdClearColorImage (a transfer-queue write needing a
        // TRANSFER_DST transition). Phase 5 must distinguish them.
        ClearAsLoadOp,
        ClearAsTransfer,

        Present,
    };

    [[nodiscard]] constexpr bool IsWriteAccess(Access access) noexcept
    {
        switch (access)
        {
            case Access::StorageWrite:
            case Access::StorageReadWrite:
            case Access::ColorAttachmentWrite:
            case Access::DepthStencilAttachmentWrite:
            case Access::TransferWrite:
            case Access::ClearAsLoadOp:
            case Access::ClearAsTransfer:
                return true;
            default:
                return false;
        }
    }

    enum class QueueType : u8
    {
        Graphics = 0,
        Compute,
        Transfer,
    };

    // -------------------------------------------------------------------------
    // Added in Phase 2 step 2 (the call-site sweep) — ADR 0011 amendment (10).
    //
    // Step 1 converted the facade's existing vocabulary; step 2 discovered the
    // facade was also INCOMPLETE. 84 distinct GL entry points appear at the 313
    // swept call sites and roughly 60% of them had no RendererAPI equivalent at
    // all — whole categories (buffer lifecycle, named-framebuffer state,
    // queries, fences) that every pass reached past the facade to perform. The
    // enums below are the neutral vocabulary those ~60 new virtuals needed.
    // -------------------------------------------------------------------------

    // The two query kinds the engine actually issues: OcclusionQueryPool's
    // visibility test and PrecipitationSystem's GPU timer. Deliberately NOT a
    // mirror of GL's query-target space — that would re-export GL under a new
    // spelling, the mistake amendment (3) called out for SetTextureParameter.
    enum class QueryType : u8
    {
        OcclusionAnySamples = 0, ///< GL_ANY_SAMPLES_PASSED / VK_QUERY_TYPE_OCCLUSION
        TimeElapsed,             ///< GL_TIME_ELAPSED / a VK_QUERY_TYPE_TIMESTAMP pair
    };

    // The four outcomes of a client-side fence wait. Mirrors glClientWaitSync's
    // return set; a Vulkan backend folds VK_SUCCESS into ConditionSatisfied and
    // VK_TIMEOUT into TimeoutExpired. AlreadySignaled is kept distinct from
    // ConditionSatisfied because the caller uses it to skip a flush.
    enum class FenceStatus : u8
    {
        AlreadySignaled = 0,
        ConditionSatisfied,
        TimeoutExpired,
        Failed,
    };

    // Which aspect(s) of a framebuffer a blit moves. Colour and depth are never
    // combined at any call site in the engine (an MRT resolve must select one
    // read/draw attachment pair at a time), so this is a plain enum rather than
    // a flag set.
    enum class BlitAspect : u8
    {
        Color = 0,
        Depth,
        Stencil,
        DepthStencil,
    };

    // Where a buffer's memory lives, expressed as intent rather than as a heap
    // index. The GL backend maps these onto buffer-storage usage hints; a Vulkan
    // backend maps them onto VMA usage hints. Naming them by intent is what keeps
    // the choice reviewable — "this buffer is written once per frame by the CPU"
    // is a fact about the engine, "VK_MEMORY_PROPERTY_HOST_COHERENT_BIT" is not.
    //
    // MOVED here from RHIResources.h in Phase 2 step 2. Phase 1 had already
    // designed exactly this and put it next to BufferDesc, where nothing outside
    // the (then declaration-only) resource header could reach it; the sweep
    // started to reinvent it as a "BufferUsage" access-pattern enum and only the
    // resulting NAME COLLISION with RHIResources.h's bind-flag BufferUsage
    // surfaced the duplication. Recorded because the near-miss is the lesson:
    // when a phase leaves a declaration-only header, later phases must read it
    // for vocabulary they are about to invent, not just for the types they
    // consume. RendererAPI.h includes only RHITypes.h, which is why it lives
    // here now rather than being reachable only alongside BufferDesc.
    enum class MemoryResidency : u8
    {
        DeviceLocal = 0, ///< GPU-only; GPU writes and reads (compute output, copy target)
        HostToDevice,    ///< CPU writes each frame, GPU reads (per-frame UBOs, upload arenas)
        DeviceToHost,    ///< GPU writes, CPU reads back (readback staging, query results)
    };

    // "This draw slot writes nowhere" in a framebuffer draw-attachment list.
    //
    // Not expressible as an attachment index, and BOTH backends need it:
    // GL spells it GL_NONE inside glNamedFramebufferDrawBuffers, Vulkan spells
    // it VK_ATTACHMENT_UNUSED inside VkSubpassDescription::pColorAttachments.
    // DecalRenderPass depends on it to steer one decal into exactly one
    // G-Buffer attachment while leaving the others untouched.
    inline constexpr u32 NoAttachment = std::numeric_limits<u32>::max();
} // namespace OloEngine::RHI

// -----------------------------------------------------------------------------
// Logging support, added in Phase 2 step 3.
//
// The ~100 diagnostic sites that used to print a bare renderer ID
// ("aoTex={}, depthTex={}") keep working, and say more than they did: a handle
// prints as `#<index>:<generation>`, so a log line now distinguishes "the same
// object" from "the same recycled slot" — the exact distinction the bare u32
// could not express and TransientPool's alias reporting needs.
//
// No new dependency: RHITypes.h already includes Core/Base.h, which includes
// Core/Log.h, which includes fmt. Nothing backend-specific enters this header.
// -----------------------------------------------------------------------------
// Hashing, so a handle can key a map exactly where a renderer ID used to.
// Mirrors std::hash<RGTextureHandle> and friends (Renderer/ResourceHandle.h),
// which pack the same way — see RHI::HashKey above for why that matters.
template<typename Tag>
struct std::hash<OloEngine::RHI::Handle<Tag>>
{
    [[nodiscard]] std::size_t operator()(const OloEngine::RHI::Handle<Tag>& handle) const noexcept
    {
        return std::hash<u64>{}(OloEngine::RHI::HashKey(handle));
    }
};

template<typename Tag>
struct fmt::formatter<OloEngine::RHI::Handle<Tag>> : formatter<std::string>
{
    template<typename FormatContext>
    auto format(const OloEngine::RHI::Handle<Tag>& handle, FormatContext& ctx) const
    {
        if (!handle.IsValid())
            return formatter<std::string>::format("<null>", ctx);

        return formatter<std::string>::format(fmt::format("#{}:{}", handle.Index, handle.Generation), ctx);
    }
};
