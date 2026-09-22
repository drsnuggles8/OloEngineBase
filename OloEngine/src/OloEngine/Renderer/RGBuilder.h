#pragma once

#include "OloEngine/Containers/String.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <concepts>
#include <functional>
#include <string>
#include <type_traits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OloEngine
{
    // Heterogeneous hash + equality so a `std::unordered_map<std::string, V>`
    // can be looked up via `std::string_view` / `const char*` without
    // allocating a temporary `std::string` per call. Use these as the third
    // and fourth template arguments to `unordered_map` / `unordered_set` to
    // get free transparent lookup. Also used internally by `RGStringInterner`
    // for the same reason.
    // A single template each, not an overload set: a `const char*` key converts
    // implicitly to std::string, std::string_view AND FString, so any overload
    // pair over those types is ambiguous for a string-literal lookup. Normalising
    // every key to a view first keeps heterogeneous lookup allocation-free.
    namespace RGStringKey
    {
        template<typename T>
        [[nodiscard]] inline std::string_view AsView(const T& s) noexcept
        {
            if constexpr (std::same_as<std::remove_cvref_t<T>, FString>)
                return s.ToView();
            else
                return std::string_view(s);
        }
    } // namespace RGStringKey

    struct RGStringTransparentHash
    {
        using is_transparent = void;
        template<typename T>
        [[nodiscard]] size_t operator()(const T& s) const noexcept
        {
            return std::hash<std::string_view>{}(RGStringKey::AsView(s));
        }
    };

    struct RGStringTransparentEqual
    {
        using is_transparent = void;
        template<typename L, typename R>
        [[nodiscard]] bool operator()(const L& a, const R& b) const noexcept
        {
            return RGStringKey::AsView(a) == RGStringKey::AsView(b);
        }
    };

    template<typename V>
    using RGTransparentStringMap = std::unordered_map<std::string, V, RGStringTransparentHash, RGStringTransparentEqual>;
    using RGTransparentStringSet = std::unordered_set<std::string, RGStringTransparentHash, RGStringTransparentEqual>;

    class Framebuffer;
    class RenderGraph;
    struct FrameBlackboard;

    // ========================================================================
    // RGBuilder
    //
    // Declarative API for pass setup callbacks. Passes use RGBuilder to
    // declare which resources they read, write, create, import, or extract.
    // The builder returns typed handles that the graph compiler uses to
    // derive execution order, barriers, lifetime, and aliasing.
    //
    // Typical usage in a pass setup callback:
    //
    //   void SetupMyPass(RGBuilder& builder, const MyPassParams& params)
    //   {
    //       auto sceneColor = builder.UseBlackboard().Scene.SceneColor;
    //       auto myAO = builder.Read(sceneColor, RGReadUsage::ShaderSample);
    //       auto myOutput = builder.Create(outputDesc);
    //       builder.Write(myOutput, RGWriteUsage::RenderTarget);
    //   }
    //
    // During execute, the graph resolves handles to physical resources:
    //
    //   void ExecuteMyPass(RGCommandContext& ctx, const MyPassParams& params)
    //   {
    //       auto aoBind = ctx.Resolve(myAO);  // u32 texture ID
    //       auto outputFB = ctx.Resolve(myOutput);  // Ref<Framebuffer>
    //       // ... GPU work using resolved resources
    //   }
    //
    // ========================================================================

    // Read access mode — describes how a pass consumes a resource
    enum class RGReadUsage : u8
    {
        ShaderSample = 0,        // Sampled in shader (texture unit)
        ShaderImage = 1,         // Image load in shader (imageLoad)
        ShaderStorage = 2,       // SSBO load
        RenderTargetRead = 3,    // Framebuffer colour/depth input
        ComputeIndirectArgs = 4, // Indirect draw args buffer
        TransferSource = 5,      // CopyImageSubData source / blit src
        InputAttachment = 6,     // Vulkan input attachment
        // A shader tracing rays against the scene TLAS (issue #978). Distinct
        // from ShaderStorage because the stage/access pair it lowers to names
        // the acceleration-structure access bits, and because the source scope
        // it has to wait on is an AS BUILD rather than another shader.
        AccelerationStructure = 7,
    };

    // Write access mode — describes how a pass produces a resource
    enum class RGWriteUsage : u8
    {
        RenderTarget = 0,  // Framebuffer colour attachment
        DepthStencil = 1,  // Framebuffer depth/stencil attachment
        ShaderImage = 2,   // Image store in shader
        ShaderStorage = 3, // SSBO store
        TransferDest = 4,  // CopyImageSubData dest / blit dst
        Clear = 5,         // Just cleared (no GPU write)
        // vkCmdBuildAccelerationStructuresKHR / the compaction copy (#978).
        AccelerationStructureBuild = 6,
    };

    // Subresource range for texture/buffer access — supports all views
    struct RGSubresourceRange
    {
        u32 BaseMip = 0;
        u32 MipCount = ~0u; // ~0u means "all mips from BaseMip"
        u32 BaseLayer = 0;
        u32 LayerCount = ~0u; // ~0u means "all layers from BaseLayer"
        u32 BaseSlice = 0;
        u32 SliceCount = ~0u; // ~0u means "all slices from BaseSlice"

        // The submission plan dedupes per-consumer transitions on
        // (resource, range, from, to). Trailing-return form for MSVC (see
        // cpp-coding-quality.md §7).
        [[nodiscard]] auto operator==(const RGSubresourceRange& other) const -> bool = default;

        static RGSubresourceRange Full()
        {
            return {};
        }

        static RGSubresourceRange Mip(u32 mip)
        {
            return { mip, 1, 0, ~0u, 0, ~0u };
        }

        static RGSubresourceRange Layer(u32 layer)
        {
            return { 0, ~0u, layer, 1, 0, ~0u };
        }
    };

    struct RGAccessDeclaration
    {
        FString ResourceName;
        bool IsWrite = false;
        RGReadUsage ReadUsage = RGReadUsage::ShaderSample;
        RGWriteUsage WriteUsage = RGWriteUsage::RenderTarget;
        RGSubresourceRange Range = RGSubresourceRange::Full();
    };

    struct RGFeedbackDeclaration
    {
        FString ResourceName;
        RGSubresourceRange Range = RGSubresourceRange::Full();
    };

    // ========================================================================
    // RGBuilder — declared-access interface for pass setup
    // ========================================================================

    class RGBuilder
    {
      public:
        RGBuilder(RenderGraph& graph, const FrameBlackboard& blackboard)
            : m_Graph(graph), m_Blackboard(blackboard)
        {
        }

        // -------------------------------------------------------------------
        // Read operations
        // -------------------------------------------------------------------

        [[nodiscard]] RGTextureHandle Read(
            RGTextureHandle handle,
            RGReadUsage usage = RGReadUsage::ShaderSample,
            const RGSubresourceRange& range = RGSubresourceRange::Full());

        [[nodiscard]] RGFramebufferHandle Read(
            RGFramebufferHandle handle,
            RGReadUsage usage = RGReadUsage::RenderTargetRead);

        [[nodiscard]] RGBufferHandle Read(
            RGBufferHandle handle,
            RGReadUsage usage = RGReadUsage::ShaderStorage);

        // -------------------------------------------------------------------
        // Write operations
        // -------------------------------------------------------------------

        // Mark a resource as written. Must call exactly once per writer.
        void Write(
            RGTextureHandle handle,
            RGWriteUsage usage = RGWriteUsage::RenderTarget,
            const RGSubresourceRange& range = RGSubresourceRange::Full());

        void Write(
            RGFramebufferHandle handle,
            RGWriteUsage usage = RGWriteUsage::RenderTarget);

        void Write(
            RGBufferHandle handle,
            RGWriteUsage usage = RGWriteUsage::ShaderStorage);

        // -------------------------------------------------------------------
        // Versioned write operations
        // -------------------------------------------------------------------

        // Opt-in explicit write-renaming for resources that are logically
        // rewritten in sequence. The returned handle names a new graph-owned
        // version cloned from the source descriptor and is already recorded
        // as the written resource for the current pass.
        [[nodiscard]] RGTextureHandle WriteNewVersion(
            RGTextureHandle sourceHandle,
            RGWriteUsage usage = RGWriteUsage::RenderTarget,
            std::string_view versionTag = {},
            const RGSubresourceRange& range = RGSubresourceRange::Full());

        [[nodiscard]] RGFramebufferHandle WriteNewVersion(
            RGFramebufferHandle sourceHandle,
            RGWriteUsage usage = RGWriteUsage::RenderTarget,
            std::string_view versionTag = {});

        [[nodiscard]] RGBufferHandle WriteNewVersion(
            RGBufferHandle sourceHandle,
            RGWriteUsage usage = RGWriteUsage::ShaderStorage,
            std::string_view versionTag = {});

        // -------------------------------------------------------------------
        // View creation helpers
        // -------------------------------------------------------------------

        [[nodiscard]] RGTextureHandle CreateFramebufferAttachmentView(
            std::string_view name,
            RGFramebufferHandle framebufferHandle,
            u32 colorAttachmentIndex);

        [[nodiscard]] RGTextureHandle CreateFramebufferDepthAttachmentView(
            std::string_view name,
            RGFramebufferHandle framebufferHandle);

        // -------------------------------------------------------------------
        // Same-pass read/write declarations
        // -------------------------------------------------------------------

        // Declare an intentional same-pass read/write overlap on a single
        // resource. This is the correct construct ONLY for genuine intra-pass
        // ping-pong / iteration patterns where one Execute legitimately reads
        // and writes the same handle — e.g. mip-chain reduction (Bloom,
        // HZB), jump-flood ping/pong (Selection outline JFA), denoise
        // ping-pong (GTAO), or a write-then-sample blit (Fog half-res,
        // Water refraction copy). The hazard validator suppresses the
        // same-pass feedback diagnostic for the declared subresource range
        // only — it does NOT silence inter-pass ordering hazards.
        //
        // Inter-pass read-modify-write of a shared resource (Decal/Particle
        // accumulating into SceneColor / OIT targets, etc.) must instead use
        // `WriteNewVersion` so the new pass output is a renamed version and
        // the prior version's read precedes the rename — no feedback loop
        // exists for the validator to see.
        void AllowSamePassReadWrite(
            RGTextureHandle handle,
            const RGSubresourceRange& range = RGSubresourceRange::Full());

        void AllowSamePassReadWrite(
            RGFramebufferHandle handle);

        void AllowSamePassReadWrite(
            RGBufferHandle handle,
            const RGSubresourceRange& range = RGSubresourceRange::Full());

        // -------------------------------------------------------------------
        // Create operations — allocate virtual (transient) resources
        // -------------------------------------------------------------------

        [[nodiscard]] RGTextureHandle CreateTexture(
            std::string_view name,
            const RGResourceDesc& desc);

        [[nodiscard]] RGFramebufferHandle CreateFramebuffer(
            std::string_view name,
            const RGResourceDesc& desc);

        [[nodiscard]] RGBufferHandle CreateBuffer(
            std::string_view name,
            const RGResourceDesc& desc);

        // -------------------------------------------------------------------
        // Import operations — register external resources
        // -------------------------------------------------------------------

        // Import a swap-chain, asset, or long-lived resource.
        // Imported resources are assumed to persist across frames unless
        // explicitly extracted.
        [[nodiscard]] RGTextureHandle ImportTexture(
            std::string_view name,
            u32 textureID,
            const RGResourceDesc& desc = {});

        // Handle-taking sibling. This is the form a migrated pass reaches for:
        // a resource created through a ...Handle creator is imported here, so
        // the graph carries its identity rather than a recyclable GL name.
        [[nodiscard]] RGTextureHandle ImportTextureHandle(
            std::string_view name,
            RHI::ResourceHandle texture,
            const RGResourceDesc& desc = {});

        [[nodiscard]] RGFramebufferHandle ImportFramebuffer(
            std::string_view name,
            const Ref<Framebuffer>& fb,
            const RGResourceDesc& desc = {});

        [[nodiscard]] RGBufferHandle ImportBuffer(
            std::string_view name,
            u32 bufferID,
            const RGResourceDesc& desc = {});
        [[nodiscard("Use the imported buffer identity")]] RGBufferHandle ImportBufferHandle(std::string_view name, RHI::ResourceHandle buffer,
                                                                                            const RGResourceDesc& desc = {});

        // -------------------------------------------------------------------
        // Extract operations — readback or reuse next frame
        // -------------------------------------------------------------------

        // Export a resource so it persists into the next frame (e.g. TAA history).
        // The graph calls the callback with the resolved physical resource
        // after Execute() completes. The callback typically stores the ID
        // for reimport next frame.
        void ExtractTexture(
            RGTextureHandle handle,
            std::function<void(u32)> callback);

        void ExtractFramebuffer(
            RGFramebufferHandle handle,
            std::function<void(Ref<Framebuffer>)> callback);

        // Declare a persistent external sink update during graph setup.
        // This roots the producing subgraph and copies the current-frame
        // resource into a caller-owned texture after Execute() completes.
        void RegisterExternalTextureSink(
            RGTextureHandle sourceHandle,
            RHI::ResourceHandle texture,
            u32 width,
            u32 height,
            bool* validFlag = nullptr);

        void RegisterExternalTextureSink(
            RGFramebufferHandle sourceHandle,
            RHI::ResourceHandle texture,
            u32 width,
            u32 height,
            u32 colorAttachmentIndex = 0,
            bool* validFlag = nullptr);

        // Declare a temporal-history egress contract during graph setup.
        // This does not queue the runtime copy-back callback; it only tells
        // the graph compiler that the current-frame resource must remain
        // reachable because it feeds a next-frame history import.
        void ExtractHistoryTexture(
            std::string_view historyResource,
            RGTextureHandle sourceHandle);

        void ExtractHistoryTexture(
            std::string_view historyResource,
            RGFramebufferHandle sourceHandle,
            u32 colorAttachmentIndex = 0);

        // -------------------------------------------------------------------
        // Blackboard access
        // -------------------------------------------------------------------

        // Read-only access to the frame blackboard for canonical resources.
        [[nodiscard]] const FrameBlackboard& UseBlackboard() const noexcept
        {
            return m_Blackboard;
        }

        // -------------------------------------------------------------------
        // Compile-time declaration capture
        // -------------------------------------------------------------------

        void BeginPass(std::string_view passName);

        void DependsOnPass(std::string_view passName);

        // Convenience: emit DependsOnPass(previousWriter) for the most recent
        // writer of the given resource base name, if any. Used by read-modify-
        // write modifier chains (SceneColor RMW, OITAccum/OITRevealage) so
        // each modifier's Setup can pin its predecessor without the pipeline
        // builder needing to wire a typed pass pointer via class-specific
        // setters. No-op when no previous writer exists or when the previous
        // writer is the current pass itself.
        void DependsOnPreviousWriter(std::string_view resourceName);

        [[nodiscard]] const TArray64<FString>& GetDeclaredReads() const noexcept
        {
            return m_DeclaredReads;
        }

        [[nodiscard]] const TArray64<FString>& GetDeclaredWrites() const noexcept
        {
            return m_DeclaredWrites;
        }

        [[nodiscard]] const TArray64<RGAccessDeclaration>& GetDeclaredAccesses() const noexcept
        {
            return m_DeclaredAccesses;
        }

        [[nodiscard]] const TArray64<RGFeedbackDeclaration>& GetDeclaredFeedbacks() const noexcept
        {
            return m_DeclaredFeedbacks;
        }

        [[nodiscard]] const TArray64<FString>& GetDeclaredPassDependencies() const noexcept
        {
            return m_DeclaredPassDependencies;
        }

        // Resources whose transient lifetime this pass extends without a
        // full hazard-tracked access declaration — currently just the parent
        // framebuffer of an attachment view this pass writes (see Write()).
        // Deliberately NOT folded into GetDeclaredAccesses(): unlike Read's
        // parent propagation (safe because same-pass Read+Read and harmless
        // sibling-view expansion never trigger a hazard), a propagated
        // *write* on the parent's name would (a) collide with a same-pass
        // read of a sibling view under the feedback-hazard validator, and
        // (b) get expanded by expandTextureViewAccesses back down onto every
        // sibling attachment view, falsely implying this pass wrote views it
        // never touched. Consumed only by RenderGraphTransientPlanner.
        [[nodiscard]] const TArray64<FString>& GetDeclaredLifetimeExtensions() const noexcept
        {
            return m_DeclaredLifetimeExtensions;
        }

        // Builder-side accessor for the owning graph. Used by free helpers
        // (e.g. `RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass`)
        // that need to look up the latest-version handle for a resource base
        // name from inside a pass's Setup. Read-only — Setup paths declare
        // accesses through this builder, not by mutating the graph directly.
        [[nodiscard]] const RenderGraph& GetGraph() const noexcept
        {
            return m_Graph;
        }

      private:
        [[nodiscard]] std::string BuildVersionedResourceName(std::string_view resourceName,
                                                             std::string_view versionTag);
        void RecordFeedback(std::string_view resourceName, const RGSubresourceRange& range);
        void RecordRead(std::string_view resourceName, RGReadUsage usage, const RGSubresourceRange& range);
        void RecordWrite(std::string_view resourceName, RGWriteUsage usage, const RGSubresourceRange& range);
        void RecordLifetimeExtension(std::string_view resourceName);

        RenderGraph& m_Graph;
        const FrameBlackboard& m_Blackboard;
        FString m_CurrentPassName;
        TArray64<FString> m_DeclaredReads;
        TArray64<FString> m_DeclaredWrites;
        TArray64<RGAccessDeclaration> m_DeclaredAccesses;
        TArray64<RGFeedbackDeclaration> m_DeclaredFeedbacks;
        TArray64<FString> m_DeclaredPassDependencies;
        TArray64<FString> m_DeclaredLifetimeExtensions;
        RGTransparentStringMap<u32> m_NextVersionOrdinalByResource;
    };

    // The owned name is heap-backed; all access metadata is value state.
    template<>
    struct TIsTriviallyRelocatable<RGAccessDeclaration>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(RGAccessDeclaration::ResourceName)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGAccessDeclaration::IsWrite)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGAccessDeclaration::ReadUsage)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGAccessDeclaration::WriteUsage)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGAccessDeclaration::Range)>;
    };
    // The owned name is heap-backed; all access metadata is value state.
    template<>
    struct TIsTriviallyRelocatable<RGFeedbackDeclaration>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(RGFeedbackDeclaration::ResourceName)> &&
                                      TIsTriviallyRelocatable_V<decltype(RGFeedbackDeclaration::Range)>;
    };
} // namespace OloEngine
