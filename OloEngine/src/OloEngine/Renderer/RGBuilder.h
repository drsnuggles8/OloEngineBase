#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <functional>
#include <string>
#include <string_view>
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
    //       auto sceneColor = builder.UseBlackboard().SceneColor;
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

      private:
        void RecordRead(std::string_view resourceName, RGReadUsage usage, const RGSubresourceRange& range);
        void RecordWrite(std::string_view resourceName, RGWriteUsage usage, const RGSubresourceRange& range);

        RenderGraph& m_Graph;
        const FrameBlackboard& m_Blackboard;
        std::string m_CurrentPassName;
        std::vector<std::string> m_DeclaredReads;
        std::vector<std::string> m_DeclaredWrites;
        std::vector<RGAccessDeclaration> m_DeclaredAccesses;
    };

} // namespace OloEngine
