#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"

namespace OloEngine
{
    void Renderer3D::BeginParallelSubmission()
    {
        OLO_PROFILE_FUNCTION();

        auto* geometryNode = s_Data.Pipeline->StreamNodes.Get(RenderStreamType::Geometry);
        if (!geometryNode)
        {
            OLO_CORE_ERROR("Renderer3D::BeginParallelSubmission: Geometry render stream is unavailable!");
            return;
        }

        // Prepare command bucket for parallel submission
        geometryNode->GetCommandBucket().PrepareForParallelSubmission();

        // Worker allocators already reset in BeginFrame — no separate prepare needed

        // Prepare frame data buffer for parallel submission
        FrameDataBufferManager::Get().PrepareForParallelSubmission();

        s_Data.ParallelSubmissionActive = true;
    }

    void Renderer3D::EndParallelSubmission()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.ParallelSubmissionActive)
        {
            OLO_CORE_WARN("Renderer3D::EndParallelSubmission: Not in parallel submission mode!");
            return;
        }

        // Merge frame data scratch buffers
        FrameDataBufferManager::Get().MergeScratchBuffers();

        auto* geometryNode = s_Data.Pipeline->StreamNodes.Get(RenderStreamType::Geometry);
        if (!geometryNode)
        {
            OLO_CORE_ERROR("Renderer3D::EndParallelSubmission: Geometry render stream is unavailable!");
            s_Data.ParallelSubmissionActive = false;
            return;
        }

        // Merge command bucket thread-local commands
        geometryNode->GetCommandBucket().MergeThreadLocalCommands();

        // Remap bone offsets from worker-local to global
        // Must be done after both MergeScratchBuffers() and MergeThreadLocalCommands()
        geometryNode->GetCommandBucket().RemapBoneOffsets(FrameDataBufferManager::Get());

        s_Data.ParallelSubmissionActive = false;
    }

    WorkerSubmitContext Renderer3D::GetWorkerContext(u32 workerIndex)
    {
        OLO_PROFILE_FUNCTION();

        WorkerSubmitContext ctx;

        // Get worker allocator directly from FrameResourceManager — zero overhead
        ctx.WorkerIndex = workerIndex;
        ctx.Allocator = FrameResourceManager::Get().GetWorkerAllocator(workerIndex);

        // Get command bucket
        if (auto* geometryNode = s_Data.Pipeline->StreamNodes.Get(RenderStreamType::Geometry))
        {
            ctx.Bucket = &geometryNode->GetCommandBucket();
            // Use the explicit worker index - no thread ID lookup needed
            ctx.Bucket->UseWorkerIndex(workerIndex);
        }

        // Set scene context
        ctx.SceneContext = &s_Data.ParallelContext;

        ctx.CommandsSubmitted = 0;
        ctx.MeshesCulled = 0;

        return ctx;
    }

    const ParallelSceneContext* Renderer3D::GetParallelSceneContext()
    {
        return &s_Data.ParallelContext;
    }

    bool Renderer3D::IsParallelSubmissionActive()
    {
        return s_Data.ParallelSubmissionActive;
    }

    void Renderer3D::SubmitRenderStreamPacket(RenderStreamType stream, CommandPacket* packet)
    {
        OLO_PROFILE_FUNCTION();

        if (!packet)
        {
            OLO_CORE_WARN("Renderer3D::SubmitRenderStreamPacket: Attempted to submit a null CommandPacket pointer!");
            return;
        }

        if (auto* streamNode = s_Data.Pipeline->StreamNodes.Get(stream))
        {
            streamNode->SubmitPacket(packet);
            return;
        }

        OLO_CORE_WARN("Renderer3D::SubmitRenderStreamPacket: Requested render stream is unavailable!");
    }

    void Renderer3D::SubmitPacketParallel(WorkerSubmitContext& ctx, CommandPacket* packet)
    {
        OLO_PROFILE_FUNCTION();

        if (!packet)
        {
            OLO_CORE_WARN("Renderer3D::SubmitPacketParallel: Null packet!");
            return;
        }

        if (!ctx.Bucket)
        {
            OLO_CORE_ERROR("Renderer3D::SubmitPacketParallel: No bucket in context!");
            return;
        }

        ctx.Bucket->SubmitPacketParallel(packet, ctx.WorkerIndex);
        ctx.CommandsSubmitted++;
    }
} // namespace OloEngine
