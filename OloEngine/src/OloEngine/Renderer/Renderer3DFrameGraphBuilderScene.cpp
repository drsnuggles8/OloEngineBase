#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3DFrameGraphBuilderInternal.h"

#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Passes/DeferredLightingPass.h"
#include "OloEngine/Renderer/Passes/DeferredOpaqueDecalPass.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

namespace OloEngine::Renderer3DFrameGraphBuilderInternal
{
    [[nodiscard]] auto CreateShadowNodeSetup() -> PassGraphNode::SetupCallback
    {
        return [](RGBuilder& builder, FrameBlackboard& board)
        {
            if (board.ShadowMapCSM.IsValid())
            {
                for (u32 cascade = 0; cascade < ShadowMap::MAX_CSM_CASCADES; ++cascade)
                {
                    builder.Write(board.ShadowMapCSM, RGWriteUsage::DepthStencil, RGSubresourceRange::Layer(cascade));
                }
            }

            if (board.ShadowMapSpot.IsValid())
            {
                for (u32 light = 0; light < ShadowMap::MAX_SPOT_SHADOWS; ++light)
                {
                    builder.Write(board.ShadowMapSpot, RGWriteUsage::DepthStencil, RGSubresourceRange::Layer(light));
                }
            }

            for (const auto& pointHandle : board.ShadowMapPoint)
            {
                if (pointHandle.IsValid())
                    builder.Write(pointHandle, RGWriteUsage::DepthStencil);
            }
        };
    }

    [[nodiscard]] auto CreateDeferredOpaqueDecalNodeSetup(RendererSettings* settings,
                                                          DecalRenderPass* decalPass) -> PassGraphNode::SetupCallback
    {
        return [settings, decalPass](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!settings || settings->Path != RenderingPath::Deferred)
                return;
            if (!decalPass || decalPass->GetCommandBucket().GetCommandCount() == 0)
                return;

            if (board.SceneDepth.IsValid())
            {
                [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            }

            if (board.GBufferAlbedo.IsValid())
                builder.Write(board.GBufferAlbedo, RGWriteUsage::RenderTarget);
            if (board.GBufferNormal.IsValid())
                builder.Write(board.GBufferNormal, RGWriteUsage::RenderTarget);
            if (board.GBufferEmissive.IsValid())
                builder.Write(board.GBufferEmissive, RGWriteUsage::RenderTarget);
        };
    }

    [[nodiscard]] auto CreateDeferredLightingNodeSetup(RendererSettings* settings) -> PassGraphNode::SetupCallback
    {
        return [settings](RGBuilder& builder, FrameBlackboard& board)
        {
            if (!settings || settings->Path != RenderingPath::Deferred)
                return;

            if (board.GBufferAlbedo.IsValid())
            {
                [[maybe_unused]] const auto gbufferAlbedoRead = builder.Read(board.GBufferAlbedo, RGReadUsage::ShaderSample);
            }
            if (board.GBufferNormal.IsValid())
            {
                [[maybe_unused]] const auto gbufferNormalRead = builder.Read(board.GBufferNormal, RGReadUsage::ShaderSample);
            }
            if (board.GBufferEmissive.IsValid())
            {
                [[maybe_unused]] const auto gbufferEmissiveRead = builder.Read(board.GBufferEmissive, RGReadUsage::ShaderSample);
            }
            if (board.SceneDepth.IsValid())
            {
                [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.SceneDepth, RGReadUsage::ShaderSample);
            }
            if (board.ShadowMapCSM.IsValid())
            {
                [[maybe_unused]] const auto shadowCSMRead = builder.Read(board.ShadowMapCSM, RGReadUsage::ShaderSample);
            }
            if (board.ShadowMapSpot.IsValid())
            {
                [[maybe_unused]] const auto shadowSpotRead = builder.Read(board.ShadowMapSpot, RGReadUsage::ShaderSample);
            }
            for (const auto& pointHandle : board.ShadowMapPoint)
            {
                if (pointHandle.IsValid())
                {
                    [[maybe_unused]] const auto pointRead = builder.Read(pointHandle, RGReadUsage::ShaderSample);
                }
            }
            if (board.AOBuffer.IsValid())
            {
                [[maybe_unused]] const auto aoRead = builder.Read(board.AOBuffer, RGReadUsage::ShaderSample);
            }
            if (board.IrradianceMap.IsValid())
            {
                [[maybe_unused]] const auto irradianceRead = builder.Read(board.IrradianceMap, RGReadUsage::ShaderSample);
            }
            if (board.PrefilterMap.IsValid())
            {
                [[maybe_unused]] const auto prefilterRead = builder.Read(board.PrefilterMap, RGReadUsage::ShaderSample);
            }
            if (board.BrdfLut.IsValid())
            {
                [[maybe_unused]] const auto brdfRead = builder.Read(board.BrdfLut, RGReadUsage::ShaderSample);
            }

            if (board.SceneColor.IsValid())
                builder.Write(board.SceneColor, RGWriteUsage::RenderTarget);
        };
    }

    void RegisterRenderStreamNodes(RenderGraph& graph,
                                   const Renderer3DFrameGraphInputs& inputs,
                                   const bool deferred)
    {
        AddExistingNode(graph, inputs.GeometryNode);

        if (deferred)
        {
            AddExistingNode(graph, inputs.ForwardOverlayNode);
        }

        AddExistingNode(graph, inputs.FoliageNode);
        // Keep insertion order aligned with the explicit baseline dependency
        // chain below (Foliage -> Decal -> Water). BuildFrameGraph derives
        // WAW edges in insertion order; inserting Water before Decal can
        // derive Water -> Decal and create a Decal <-> Water cycle.
        AddExistingNode(graph, inputs.DecalNode);
        AddExistingNode(graph, inputs.WaterNode);
    }

    void RegisterSceneAndLightingNodes(RenderGraph& graph,
                                       const Renderer3DFrameGraphInputs& inputs,
                                       const bool deferred)
    {
        graph.AddNode(MakePassNode("ShadowPass", inputs.ShadowPass, CreateShadowNodeSetup()));

        if (!deferred)
            return;

        if (inputs.OpaqueDecalPass)
        {
            graph.AddNode(MakePassNode("DeferredOpaqueDecalPass",
                                       inputs.OpaqueDecalPass,
                                       CreateDeferredOpaqueDecalNodeSetup(inputs.Settings, inputs.DecalPass)));
        }
        graph.AddNode(MakePassNode("DeferredLightingPass",
                                   inputs.DeferredLightPass,
                                   CreateDeferredLightingNodeSetup(inputs.Settings)));
    }
} // namespace OloEngine::Renderer3DFrameGraphBuilderInternal
