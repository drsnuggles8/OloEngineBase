#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Framebuffer;
    class RenderGraph;
    struct FrameBlackboard;

    // ========================================================================
    // Phase C — RGBuilder
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
        std::string ResourceName;
        bool IsWrite = false;
        RGReadUsage ReadUsage = RGReadUsage::ShaderSample;
        RGWriteUsage WriteUsage = RGWriteUsage::RenderTarget;
        RGSubresourceRange Range = RGSubresourceRange::Full();
    };

    struct RGFeedbackDeclaration
    {
        std::string ResourceName;
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

        [[nodiscard]] RGFramebufferHandle ImportFramebuffer(
            std::string_view name,
            const Ref<Framebuffer>& fb,
            const RGResourceDesc& desc = {});

        [[nodiscard]] RGBufferHandle ImportBuffer(
            std::string_view name,
            u32 bufferID,
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
            u32 textureID,
            u32 width,
            u32 height,
            bool* validFlag = nullptr);

        void RegisterExternalTextureSink(
            RGFramebufferHandle sourceHandle,
            u32 textureID,
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
        // Phase C compile-time declaration capture
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

        [[nodiscard]] const std::vector<std::string>& GetDeclaredReads() const noexcept
        {
            return m_DeclaredReads;
        }

        [[nodiscard]] const std::vector<std::string>& GetDeclaredWrites() const noexcept
        {
            return m_DeclaredWrites;
        }

        [[nodiscard]] const std::vector<RGAccessDeclaration>& GetDeclaredAccesses() const noexcept
        {
            return m_DeclaredAccesses;
        }

        [[nodiscard]] const std::vector<RGFeedbackDeclaration>& GetDeclaredFeedbacks() const noexcept
        {
            return m_DeclaredFeedbacks;
        }

        [[nodiscard]] const std::vector<std::string>& GetDeclaredPassDependencies() const noexcept
        {
            return m_DeclaredPassDependencies;
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

        RenderGraph& m_Graph;
        const FrameBlackboard& m_Blackboard;
        std::string m_CurrentPassName;
        std::vector<std::string> m_DeclaredReads;
        std::vector<std::string> m_DeclaredWrites;
        std::vector<RGAccessDeclaration> m_DeclaredAccesses;
        std::vector<RGFeedbackDeclaration> m_DeclaredFeedbacks;
        std::vector<std::string> m_DeclaredPassDependencies;
        std::unordered_map<std::string, u32> m_NextVersionOrdinalByResource;
    };

} // namespace OloEngine
