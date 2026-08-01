#pragma once

// =============================================================================
// RHIResources.h — API-neutral descriptions of textures, buffers, views,
// pipelines, barriers, and the descriptor heap.
//
// Issue #691 Phase 1, ADR 0011 (docs/adr/0011-rhi-neutral-resource-and-binding-model.md).
//
// **Not yet load-bearing: only RHIResourceRegistry.cpp includes these so far.**
// (Phase 2 step 3 minted RHI::ResourceHandle, but that lives in RHITypes.h — the
// resource *descriptions* below are still waiting on their consumers.) Same two
// rules as RHITypes.h: no backend headers, no backend types, and these are engine
// enums that a backend converts explicitly.
//
// The descriptions below are heap+offset shaped from day one because the
// binding model is heap-bindless-only (ADR 0010) — there is no classic
// descriptor-set path to migrate away from later.
// =============================================================================

#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <string>
#include <vector>

namespace OloEngine::RHI
{
    // =========================================================================
    // Resource descriptions
    // =========================================================================

    enum class TextureDimension : u8
    {
        Texture2D = 0,
        Texture2DArray,
        Texture3D,
        TextureCube,
        TextureCubeArray,
    };

    // What a texture will be used for. Vulkan requires this up front
    // (VkImageUsageFlags is immutable after creation) whereas GL infers it, so
    // the neutral description must carry it or the Vulkan backend has to
    // over-request every usage on every image — which costs both memory layout
    // quality and validation-layer signal.
    enum class TextureUsage : u32
    {
        None = 0,
        Sampled = OloBit32(0),
        Storage = OloBit32(1), ///< imageLoad / imageStore
        ColorAttachment = OloBit32(2),
        DepthStencilAttachment = OloBit32(3),
        TransferSrc = OloBit32(4),
        TransferDst = OloBit32(5),
    };

    [[nodiscard]] constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept
    {
        return static_cast<TextureUsage>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    [[nodiscard]] constexpr bool HasFlag(TextureUsage value, TextureUsage flag) noexcept
    {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0u;
    }

    struct TextureDesc
    {
        TextureDimension Dimension = TextureDimension::Texture2D;
        // Named PixelFormat, not Format: a member named `Format` inside this
        // struct would hide the enum of the same name, so the default member
        // initializer `Format::RGBA8UNorm` would fail to compile. The existing
        // RGResourceDesc dodges this the same way (RGResourceFormat Format).
        Format PixelFormat = Format::RGBA8UNorm;
        TextureUsage Usage = TextureUsage::Sampled;

        u32 Width = 1;
        u32 Height = 1;
        u32 DepthOrArrayLayers = 1;
        u32 MipLevels = 1;
        u32 Samples = 1;

        // Pixels replaced every frame from the CPU (streamed video). The GL
        // backend already routes these through a double-buffered PBO ring; the
        // Vulkan backend would use a host-visible staging ring. Backend-specific
        // mechanism, neutral intent — which is why it belongs here rather than
        // being inferred from usage flags.
        bool Streaming = false;

        std::string DebugName;
    };

    enum class BufferUsage : u32
    {
        None = 0,
        Vertex = OloBit32(0),
        Index = OloBit32(1),
        Uniform = OloBit32(2),
        Storage = OloBit32(3),
        Indirect = OloBit32(4),
        TransferSrc = OloBit32(5),
        TransferDst = OloBit32(6),
    };

    [[nodiscard]] constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept
    {
        return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    [[nodiscard]] constexpr bool HasFlag(BufferUsage value, BufferUsage flag) noexcept
    {
        return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0u;
    }

    // MemoryResidency MOVED to RHITypes.h in Phase 2 step 2. It turned out to be
    // vocabulary rather than resource description: RendererAPI::AllocateBufferStorage
    // needs it, and RendererAPI.h includes only RHITypes.h. See the note there.

    struct BufferDesc
    {
        u64 SizeBytes = 0;
        BufferUsage Usage = BufferUsage::Storage;
        MemoryResidency Residency = MemoryResidency::DeviceLocal;
        std::string DebugName;
    };

    // =========================================================================
    // Views and the descriptor heap
    //
    // ADR 0011 §1.2: the heap slot is owned by the VIEW, not the resource. Under
    // VK_EXT_descriptor_heap the sampler heap is separate from the resource
    // heap, so a texture needs one resource-heap slot per
    // (resource, subresource range, format reinterpretation) — not one per
    // sampler.
    //
    // The engine already needs exactly this: RendererAPI::CreateDepthArrayCompareOffView
    // exists because one depth array must be reachable both as a
    // sampler2DArrayShadow and as a plain sampler2DArray for the PCSS blocker
    // search. That is a format/compare-mode reinterpretation of one image — a
    // second view, and under this model a second heap slot.
    // =========================================================================

    // Which plane(s) of a resource an access refers to. A depth-stencil format
    // has two, and Vulkan can transition them independently
    // (DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL and its mirror), so a range
    // without an aspect is not a complete subresource identifier. The engine's
    // existing RGSubresourceRange omits this; it is added here because
    // CreateDepthArrayCompareOffView already produces a depth-only read of a
    // resource that is elsewhere used as a depth-stencil attachment.
    enum class TextureAspect : u8
    {
        Color = 0,
        Depth,
        Stencil,
        DepthStencil,
    };

    struct SubresourceRange
    {
        static constexpr u32 AllRemaining = ~0u;

        TextureAspect Aspect = TextureAspect::Color;
        u32 BaseMip = 0;
        u32 MipCount = AllRemaining;
        u32 BaseLayer = 0;
        u32 LayerCount = AllRemaining;

        [[nodiscard]] auto operator==(const SubresourceRange& other) const -> bool = default;
    };

    // The *description* of a view — the key a view cache is keyed on. The
    // identity handed back is RHI::ViewHandle (RHITypes.h).
    //
    // Note this does NOT imply one VkImageView per view. Under descriptor-heap
    // Vulkan, view API objects largely disappear outside colour/depth
    // attachments — DXVK reports replacing VkImageView with a plain descriptor
    // blob and managing it the same way as before. The engine-side *concept* of
    // "a particular way of viewing a resource" survives regardless, and now has
    // to be owned by us precisely because the API no longer owns it. That is
    // what ViewHandle is for; how a backend realises it (VkImageView, a
    // descriptor blob, a GL texture view) is its own business.
    struct ViewDesc
    {
        ResourceHandle Resource;
        SubresourceRange Range;
        // Unknown = inherit the resource's format. A different value is a
        // deliberate reinterpretation (e.g. reading a depth texture as R32Float).
        Format FormatOverride = Format::Unknown;
        // Depth textures only: false gives a raw-depth read where the resource's
        // own view would give a hardware comparison result. Models
        // CreateDepthArrayCompareOffView neutrally.
        bool DepthCompare = true;

        [[nodiscard]] auto operator==(const ViewDesc& other) const -> bool = default;
    };

    // Slot lifetime follows the resource's lifetime class. The engine already
    // has exactly two and they are already separated in code — do not invent a
    // third (ADR 0011 §1.2).
    enum class HeapSlotLifetime : u8
    {
        // Allocated at view creation, freed at view destruction. The offset is
        // stable for the object's life, so it can be baked once into material
        // data and never touched. That stability IS the performance argument for
        // bindless.
        Persistent = 0,

        // Allocated from a per-frame ring that resets in
        // TransientPool::ReleaseAll(). Required, not merely convenient:
        // TransientPool hands the SAME physical object to two logical resources
        // with disjoint lifetimes, and RenderGraph::WriteNewVersion is a rename
        // of one physical resource. With persistent slots an aliased pair would
        // need one offset rewritten mid-frame — the exact stale-read archetype
        // docs/agent-rules/render-graph-transient-aliasing.md warns about, and
        // one that LIFO pool reuse hides in steady state. With a per-frame ring,
        // each acquisition gets its own offset and a version rename gets a
        // second offset onto the same object, which makes the alias visible in
        // the heap rather than invisible.
        FrameTransient,
    };

    // Allocation is EXPLICIT and observable, never implicit-on-first-use — same
    // reasoning as above. `PoisonOnFree` overwrites a released slot with a
    // descriptor for a known-bad resource so a use-after-free renders as a
    // deterministic, obviously-wrong result instead of whatever the previous
    // tenant left behind. Mirrors OLO_RG_POISON_TRANSIENTS, which turned a
    // stochastic aliasing artifact into a one-screenshot signal.
    struct HeapDesc
    {
        // Slot counts, not byte sizes. The heap is addressed as a flat array
        // with ONE uniform stride — the largest descriptor size the backend
        // needs — so a slot index converts to a byte offset by multiplication
        // and HeapOffset can stay a plain shader-side array index. Uniform
        // stride wastes a little memory on small descriptor types and is the
        // standard trade: DXVK describes full bindless under descriptor-heap as
        // "use the size of the largest descriptor type that you need as an
        // array stride, index into the heap in your shader, allocate memory,
        // bind heap, done."
        u32 ResourceSlotCapacity = 0;
        u32 SamplerSlotCapacity = 0;
        u32 FrameTransientRingSlots = 0; ///< per frame-in-flight
        bool PoisonOnFree = false;
    };

    // The ONLY sanctioned way to turn a view into a shader-side index.
    //
    // Validates the handle's generation, then returns the offset. Fetch at the
    // point you write it into a buffer; never cache the HeapOffset itself (see
    // RHITypes.h). This is cheap CPU-side insurance against an expensive class
    // of bug: a malformed or stale descriptor under descriptor-heap does not
    // merely sample the wrong resource — DXVK's experience with the equivalent
    // descriptor-buffer model is that "if you screw up, you will likely hang
    // your GPU and have all sorts of fun trying to debug that." Catching it here
    // costs a generation compare.
    [[nodiscard]] HeapOffset OffsetOf(ViewHandle view);

    // Sampler-heap slots are allocated separately, because the sampler heap IS
    // separate under VK_EXT_descriptor_heap. This is a genuine modelling change
    // rather than plumbing: on OpenGL today, filter/wrap state lives ON the
    // texture object (glTextureParameteri), so the engine has no concept of a
    // shareable sampler. Under a split heap the same sampler state used by 500
    // textures should occupy ONE sampler slot, which means Phase 3/4 has to
    // introduce sampler deduplication that has no GL counterpart to port from.
    //
    // Immutable/embedded samplers are deliberately NOT modelled. Descriptor-heap
    // makes them markedly more complicated (for drivers too), the engine does
    // not use them, and adding them speculatively would buy nothing — but any
    // middleware that wants them becomes a sampler-heap management problem, so
    // this is a Phase 4 device-bring-up checklist item, not a silent omission.
    // Phase 2 widened the filter/wrap members from bools to RHI::Filter /
    // RHI::AddressMode. The bools could not express the combinations the sweep
    // actually had to replace — SSAORenderPass's noise texture is Nearest+Repeat,
    // and CreateDepthArrayCompareOffView is Nearest+ClampToBorder — so a
    // two-bool sampler would have forced those call sites to keep a GL escape
    // hatch. See RHITypes.h's Filter/AddressMode note.
    struct SamplerDesc
    {
        // `Compare`, not `CompareOp` — a member sharing the enum's name hides it
        // and breaks the default member initializer (same trap as
        // TextureDesc::PixelFormat above).
        CompareOp Compare = CompareOp::Never; ///< Never = comparison disabled
        Filter MinFilter = Filter::Linear;
        Filter MagFilter = Filter::Linear;
        bool LinearMipFilter = true;
        AddressMode AddressU = AddressMode::ClampToEdge;
        AddressMode AddressV = AddressMode::ClampToEdge;
        AddressMode AddressW = AddressMode::ClampToEdge;
        f32 MaxAnisotropy = 1.0f;

        [[nodiscard]] auto operator==(const SamplerDesc& other) const -> bool = default;
    };

    // =========================================================================
    // Pipeline state
    //
    // Everything a Vulkan VkPipeline bakes in. On GL these are separate dynamic
    // calls; collecting them into one description is what lets the two backends
    // share a pass's intent. It is also the reason shader hot-reload has a
    // different invalidation granularity per backend (ADR 0011 §3(d)): one
    // shader is the source of N pipelines, one per permutation of the state
    // below, so a reload must invalidate all of them via a
    // shader -> {pipeline} reverse index.
    // =========================================================================

    struct BlendState
    {
        bool Enabled = false;
        BlendFactor SrcColor = BlendFactor::SrcAlpha;
        BlendFactor DstColor = BlendFactor::OneMinusSrcAlpha;
        BlendOp ColorOp = BlendOp::Add;
        BlendFactor SrcAlpha = BlendFactor::One;
        BlendFactor DstAlpha = BlendFactor::OneMinusSrcAlpha;
        BlendOp AlphaOp = BlendOp::Add;
        bool WriteR = true;
        bool WriteG = true;
        bool WriteB = true;
        bool WriteA = true;
    };

    struct DepthStencilState
    {
        bool DepthTestEnabled = true;
        bool DepthWriteEnabled = true;
        CompareOp DepthCompare = CompareOp::Less;

        bool StencilTestEnabled = false;
        CompareOp StencilCompare = CompareOp::Always;
        StencilOp StencilFail = StencilOp::Keep;
        StencilOp DepthFail = StencilOp::Keep;
        StencilOp DepthPass = StencilOp::Keep;
        u32 StencilReference = 0;
        u32 StencilReadMask = 0xFFu;
        u32 StencilWriteMask = 0xFFu;
    };

    struct RasterState
    {
        CullMode Cull = CullMode::Back;
        FrontFace Winding = FrontFace::CounterClockwise;
        PolygonMode Fill = PolygonMode::Fill;
        f32 DepthBiasConstant = 0.0f;
        f32 DepthBiasSlope = 0.0f;
        f32 LineWidth = 1.0f;
    };

    struct PipelineDesc
    {
        // Identifies the shader whose SPIR-V feeds this pipeline. Kept as an
        // engine-side handle rather than a Ref<Shader> so the reverse index that
        // hot-reload walks does not create ownership cycles.
        ResourceHandle ShaderHandle;

        PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
        u32 PatchControlPoints = 0; ///< PrimitiveTopology::PatchList only

        RasterState Raster;
        DepthStencilState DepthStencil;

        // One entry per colour attachment, in attachment order. Per-attachment
        // blend is not optional: weighted-blended OIT needs different blend
        // functions on accum (RT0) and revealage (RT1) of the same framebuffer,
        // and the deferred G-Buffer mixes integer and float attachments.
        std::vector<BlendState> ColorBlend;

        // Attachment formats are part of pipeline identity on Vulkan. They are
        // NOT part of it on GL, which is exactly why a shader reload invalidates
        // one program on GL and N pipelines on Vulkan.
        std::vector<Format> ColorAttachmentFormats;
        Format DepthStencilAttachmentFormat = Format::Unknown;
        u32 Samples = 1;

        std::string DebugName;
    };

    // =========================================================================
    // Barriers
    //
    // ADR 0011 §1.5. This is what RenderGraph::ResourceTransition should carry
    // once Phase 5 unifies its RGWriteUsage -> RGReadUsage pair, which today
    // structurally cannot express a write -> write transition.
    //
    // There is no image-layout member, deliberately: layout is a pure function
    // of Access, so the Vulkan backend derives it. See RHITypes.h.
    // =========================================================================

    struct Barrier
    {
        ResourceHandle Resource;
        SubresourceRange Range;
        Access Before = Access::Undefined;
        Access After = Access::Undefined;

        // Set when producer and consumer sit on different queues. On GL 4.6
        // (one command stream) this is informational; on Vulkan it drives
        // queue-family ownership transfer and semaphore waits.
        bool IsCrossQueue = false;
        QueueType SourceQueue = QueueType::Graphics;
        QueueType DestQueue = QueueType::Graphics;
    };

    // =========================================================================
    // The debug escape hatch.
    //
    // The ONLY sanctioned way to obtain a backend-native handle outside
    // Platform/. Named to be conspicuous: a call to this in a render pass is a
    // self-evident smell in review, where `GetRendererID()` reads as ordinary —
    // which is how GL reached 42 files outside the backend.
    //
    // Legitimate callers are the introspection tools in Renderer/Debug/ and the
    // MCP capture endpoints they back (olo_render_capture_target,
    // olo_render_transient_plan). Those tools are Phase 8 relocation work, not
    // permanent exemptions: CLAUDE.md's rendering-verification rule is enforced
    // through them, so a Vulkan backend they cannot see is a Vulkan backend
    // Phase 7 cannot verify.
    //
    // RHIBoundaryRatchetTest baselines uses outside Renderer/Debug/ and
    // Platform/ at zero.
    // =========================================================================
    [[nodiscard]] NativeHandle GetNativeHandleForDebug(ResourceHandle handle);
} // namespace OloEngine::RHI
