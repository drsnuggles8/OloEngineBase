#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphPassSnapshot.h"

#include "OloEngine/Renderer/Debug/PassSnapshotBackend.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <algorithm>
#include <format>

// This TU is orchestration only (#691): request lifecycle, the
// scratch-slot reuse cache, and result bookkeeping. The GL clone engine —
// native introspection, scratch allocation, the per-mip bitwise copy, error
// draining — lives behind the PassSnapshotBackend.h seam in
// Platform/OpenGL/OpenGLPassSnapshot.cpp, in native u32 currency end to end
// (the Resolver/Result contract is pinned native by the MCP tools; see the
// seam header).

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
            if (slot.Texture != 0)
                Detail::DeleteNativeTexture(slot.Texture);
        }
        m_Scratch.clear();
        m_Results.clear();
    }

    u32 RenderGraphPassSnapshot::AcquireScratch(const sizet slot, const u32 nativeTarget, const u32 nativeInternalFormat,
                                                const u32 width, const u32 height, const u32 depthOrLayers,
                                                const u32 mipLevels)
    {
        if (slot >= m_Scratch.size())
            m_Scratch.resize(slot + 1u);

        ScratchSlot& scratch = m_Scratch[slot];
        if (scratch.Texture != 0 && scratch.Target == nativeTarget && scratch.Format == nativeInternalFormat &&
            scratch.Width == width && scratch.Height == height && scratch.Depth == depthOrLayers &&
            scratch.Mips == mipLevels)
        {
            return scratch.Texture;
        }

        if (scratch.Texture != 0)
        {
            Detail::DeleteNativeTexture(scratch.Texture);
            scratch = {};
        }

        const u32 texture = Detail::CreateNativeScratchTexture(nativeTarget, nativeInternalFormat,
                                                               width, height, depthOrLayers, mipLevels);
        if (texture == 0)
            return 0;

        scratch.Texture = texture;
        scratch.Target = nativeTarget;
        scratch.Format = nativeInternalFormat;
        scratch.Width = width;
        scratch.Height = height;
        scratch.Depth = depthOrLayers;
        scratch.Mips = mipLevels;
        return texture;
    }

    void RenderGraphPassSnapshot::CaptureOne(const sizet slot, const Request& request, Result& out)
    {
        out.ResourceName = request.ResourceName;

        const u32 sourceId = request.Resolve ? request.Resolve() : 0u;
        const Detail::NativeTextureCloneInfo info = Detail::QueryNativeTextureCloneInfo(sourceId);
        if (sourceId == 0u || !info.IsTexture)
        {
            out.Error = "Resource '" + request.ResourceName + "' did not resolve to a GL texture after pass '" +
                        m_PassName + "' (wrong rendering path, effect disabled, or no GPU backing).";
            return;
        }
        out.SourceTextureID = sourceId;

        if (info.Samples > 1)
        {
            out.Error = "'" + request.ResourceName +
                        "' is multisampled; afterPass snapshots support single-sample textures only.";
            return;
        }

        if (info.Width == 0 || info.Height == 0)
        {
            out.Error = "'" + request.ResourceName + "' has no storage at mip 0.";
            return;
        }

        const u32 scratch = AcquireScratch(slot, info.Target, info.InternalFormat,
                                           info.Width, info.Height, info.DepthOrLayers,
                                           info.MipLevels);
        if (scratch == 0u)
        {
            out.Error = "Could not allocate a snapshot clone for '" + request.ResourceName + "' (GL target 0x" +
                        std::format("{:X}", info.Target) + ").";
            return;
        }

        if (const u32 error = Detail::CopyNativeTextureAllMips(sourceId, scratch, info.Target,
                                                               info.Width, info.Height, info.DepthOrLayers,
                                                               info.MipLevels);
            error != 0u)
        {
            out.Error = "glCopyImageSubData on '" + request.ResourceName + "' failed (GL 0x" +
                        std::format("{:X}", error) + ").";
            return;
        }

        out.Captured = true;
        out.TextureID = scratch;
        out.Width = info.Width;
        out.Height = info.Height;
        out.DepthOrLayers = info.DepthOrLayers;
        out.MipLevels = info.MipLevels;
        out.NativeInternalFormat = info.InternalFormat;
        out.NativeTarget = info.Target;
    }

    void RenderGraphPassSnapshot::OnPassExecuted(const std::string& passName, RenderGraph& /*graph*/)
    {
        if (!m_Pending || passName != m_PassName)
            return;

        // One-shot: whatever happens below, this request is consumed.
        m_Pending = false;
        m_Results.clear();
        m_Results.resize(m_Requests.size());

        // Drain any stale error so the checks in CaptureOne attribute
        // failures to THIS copy, not to whatever the executing pass left.
        Detail::DrainNativeErrors();

        for (sizet i = 0; i < m_Requests.size(); ++i)
            CaptureOne(i, m_Requests[i], m_Results[i]);
    }
} // namespace OloEngine
