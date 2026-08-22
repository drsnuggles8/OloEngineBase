#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphPassSnapshot.h"

#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <algorithm>
#include <format>

// Backend-neutral end to end since #810. This TU owns the request lifecycle,
// the scratch-slot reuse cache, the per-mip/per-layer copy loop and result
// bookkeeping; every GPU step goes through the facade
// (CreateMatchingTextureHandle / CopyImageSubDataFull / QueryTextureFormat /
// GetTextureDimensions), so there is no clone engine to relocate and the
// PassSnapshotBackend.h seam is gone. See the header for why the previous
// "no RHI::ResourceHandle can exist on this path" reasoning did not survive
// the scratch becoming something this code creates rather than adopts.

namespace OloEngine
{
    RenderGraphPassSnapshot::~RenderGraphPassSnapshot()
    {
        // Deliberately no GPU calls: the instance lives as a process-static
        // (RenderGraphDebugRuntime), destroyed after the GL context. The real
        // cleanup site is ReleaseScratch(), invoked from renderer shutdown via
        // RenderGraphDebugRuntime::SetActiveGraph(nullptr).
    }

    void RenderGraphPassSnapshot::Arm(RenderGraph* graph, std::string passName, std::vector<Request> requests)
    {
        if (m_InstalledGraph && m_InstalledGraph != graph)
            m_InstalledGraph->RemovePostPassHook(kPostPassHookKey);

        m_InstalledGraph = graph;
        m_PassName = std::move(passName);
        m_Requests = std::move(requests);
        m_Results.clear();
        m_Pending = graph != nullptr && !m_Requests.empty();

        if (graph)
        {
            graph->AddPostPassHook(kPostPassHookKey,
                                   [this](const std::string& executedPass, RenderGraph& g)
                                   { this->OnPassExecuted(executedPass, g); });
        }
    }

    void RenderGraphPassSnapshot::Disarm()
    {
        if (m_InstalledGraph)
            m_InstalledGraph->RemovePostPassHook(kPostPassHookKey);
        m_InstalledGraph = nullptr;
        m_Pending = false;
        m_Requests.clear();
    }

    void RenderGraphPassSnapshot::ReleaseScratch()
    {
        for (auto& slot : m_Scratch)
        {
            if (slot.Texture.IsValid())
                RenderCommand::DeleteTexture(slot.Texture);
        }
        m_Scratch.clear();
        m_Results.clear();
    }

    namespace
    {
        // Which copy target a source's storage description implies. The copy
        // facade needs this on both operands, and getting it wrong is a hard
        // error rather than a wrong picture (GL requires matching targets;
        // Vulkan requires a compatible image type).
        bool TargetTypeFor(RHI::TextureShape shape, RendererAPI::TextureTargetType& out)
        {
            switch (shape)
            {
                case RHI::TextureShape::Texture2D:
                    out = RendererAPI::TextureTargetType::Texture2D;
                    return true;
                case RHI::TextureShape::Texture2DArray:
                    out = RendererAPI::TextureTargetType::Texture2DArray;
                    return true;
                case RHI::TextureShape::TextureCube:
                    out = RendererAPI::TextureTargetType::TextureCubeMap;
                    return true;
                case RHI::TextureShape::TextureCubeArray:
                    out = RendererAPI::TextureTargetType::TextureCubeMapArray;
                    return true;
                case RHI::TextureShape::Texture3D:
                    out = RendererAPI::TextureTargetType::Texture3D;
                    return true;
                case RHI::TextureShape::Texture2DMultisample:
                case RHI::TextureShape::Unknown:
                    break;
            }
            // Multisample sources are refused rather than resolved, and an
            // unknown shape cannot name a copy target — say so instead of
            // defaulting to 2D, which is a driver error at best and a copy of
            // the wrong subresource at worst.
            return false;
        }
    } // namespace

    RHI::ResourceHandle RenderGraphPassSnapshot::AcquireScratch(const sizet slot, RHI::ResourceHandle source,
                                                                const RHI::TextureFormatInfo& format,
                                                                const u32 width, const u32 height,
                                                                const u32 depthOrLayers,
                                                                const RendererAPI::TextureTargetType target)
    {
        if (slot >= m_Scratch.size())
            m_Scratch.resize(slot + 1u);

        ScratchSlot& scratch = m_Scratch[slot];
        // Reuse only on an EXACT shape match. A near-match would be a copy into
        // a differently-sized or differently-formatted destination, which both
        // backends reject — and a rejected copy leaves the previous arm's
        // contents in the scratch, which the tools would then report as this
        // frame's.
        if (scratch.Texture.IsValid() && scratch.Target == target && scratch.NativeFormat == format.Native &&
            scratch.Width == width && scratch.Height == height && scratch.Depth == depthOrLayers &&
            scratch.Mips == format.MipLevels)
        {
            return scratch.Texture;
        }

        if (scratch.Texture.IsValid())
        {
            RenderCommand::DeleteTexture(scratch.Texture);
            scratch = {};
        }

        const RHI::ResourceHandle clone = RenderCommand::CreateMatchingTextureHandle(source);
        if (!clone.IsValid())
            return {};

        scratch.Texture = clone;
        scratch.Target = target;
        scratch.NativeFormat = format.Native;
        scratch.Width = width;
        scratch.Height = height;
        scratch.Depth = depthOrLayers;
        scratch.Mips = format.MipLevels;
        return clone;
    }

    void RenderGraphPassSnapshot::CaptureOne(const sizet slot, const Request& request, Result& out)
    {
        out.ResourceName = request.ResourceName;

        const RHI::ResourceHandle source = request.Resolve ? request.Resolve() : RHI::ResourceHandle{};
        if (!source.IsValid())
        {
            out.Error = "Resource '" + request.ResourceName + "' did not resolve to a texture after pass '" +
                        m_PassName + "' (wrong rendering path, effect disabled, or no GPU backing).";
            return;
        }
        out.SourceHandle = source;
        out.NativeSourceId = static_cast<u32>(RHI::GetNativeHandleForDebug(source).Value);

        RHI::TextureFormatInfo format;
        if (!RenderCommand::QueryTextureFormat(source, 0u, format))
        {
            out.Error = "'" + request.ResourceName +
                        "' has no storage at mip 0, or a format the snapshot cannot describe.";
            return;
        }

        u32 width = 0;
        u32 height = 0;
        RenderCommand::GetTextureDimensions(source, 0u, width, height);
        if (width == 0u || height == 0u)
        {
            out.Error = "'" + request.ResourceName + "' has no storage at mip 0.";
            return;
        }

        // A volume's third dimension is depth SLICES and an array's is LAYERS;
        // the copy addresses both through the same z, but the target type
        // differs and both backends check it. The count alone cannot tell them
        // apart, which is why QueryTextureFormat reports the SHAPE.
        const u32 depthOrLayers = std::max(format.ArrayLayers, 1u);
        RendererAPI::TextureTargetType target = RendererAPI::TextureTargetType::Texture2D;
        if (!TargetTypeFor(format.Shape, target))
        {
            out.Error = "'" + request.ResourceName +
                        "' has a storage shape the snapshot cannot clone (multisampled, or a "
                        "dimensionality the copy path does not address).";
            return;
        }

        const RHI::ResourceHandle clone = AcquireScratch(slot, source, format, width, height, depthOrLayers, target);
        if (!clone.IsValid())
        {
            out.Error = "Could not allocate a snapshot clone for '" + request.ResourceName +
                        "' (the source's storage could not be reproduced — a multisampled or "
                        "undescribable target).";
            return;
        }

        // Every mip, every layer — the clone must be usable for the same
        // mip/layer arguments the live resource accepts.
        for (u32 mip = 0u; mip < std::max(format.MipLevels, 1u); ++mip)
        {
            const u32 mipWidth = std::max(width >> mip, 1u);
            const u32 mipHeight = std::max(height >> mip, 1u);
            for (u32 layer = 0u; layer < depthOrLayers; ++layer)
            {
                RenderCommand::CopyImageSubDataFull(source, target, static_cast<i32>(mip), static_cast<i32>(layer),
                                                    clone, target, static_cast<i32>(mip), static_cast<i32>(layer),
                                                    mipWidth, mipHeight);
            }
        }

        out.Captured = true;
        out.Handle = clone;
        out.NativeCloneId = static_cast<u32>(RHI::GetNativeHandleForDebug(clone).Value);
        out.Width = width;
        out.Height = height;
        out.DepthOrLayers = depthOrLayers;
        out.MipLevels = std::max(format.MipLevels, 1u);
        out.Format = format;
    }

    void RenderGraphPassSnapshot::OnPassExecuted(const std::string& passName, RenderGraph& /*graph*/)
    {
        if (!m_Pending || passName != m_PassName)
            return;

        // One-shot: whatever happens below, this request is consumed.
        m_Pending = false;
        m_Results.clear();
        m_Results.resize(m_Requests.size());

        for (sizet i = 0; i < m_Requests.size(); ++i)
            CaptureOne(i, m_Requests[i], m_Results[i]);
    }
} // namespace OloEngine
