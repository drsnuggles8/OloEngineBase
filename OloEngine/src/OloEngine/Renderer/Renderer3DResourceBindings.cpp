#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    auto Renderer3D::GetRenderStreamNode(RenderStreamType stream) -> CommandBufferRenderPass*
    {
        if (!s_Data.Pipeline)
        {
            return nullptr;
        }

        return s_Data.Pipeline->GetRenderStreamNode(stream);
    }

    Renderer3D::DecalVisibilityObservation Renderer3D::ObserveDecalVisibility(i32 entityID)
    {
        if (!HasInitialized() || entityID < 0)
            return {};

        if (!s_Data.DecalVisibilityQueriesInitialized)
        {
            RenderCommand::CreateQueries(RHI::QueryType::OcclusionAnySamples,
                                         s_Data.DecalReceiverIntersectionQueries);
            RenderCommand::CreateQueries(RHI::QueryType::OcclusionAnySamples,
                                         s_Data.DecalVisibilityQueries);
            s_Data.DecalVisibilityQueriesInitialized = true;
        }

        s_Data.DecalVisibilityTarget = entityID;
        if (auto& active = s_Data.DecalVisibilityFrames[s_Data.DecalVisibilityWriteBuffer];
            !active.Submitted && !active.DrawIssued && !active.ReceiverQueryIssued && !active.QueryIssued)
            active.EntityID = entityID;

        if (s_Data.LastDecalVisibilityEntity == entityID)
            return s_Data.LastDecalVisibilityObservation;
        return {};
    }

    void Renderer3D::AdvanceDecalVisibilityFrame()
    {
        const u32 completedIndex = s_Data.DecalVisibilityWriteBuffer;
        const auto& completed = s_Data.DecalVisibilityFrames[completedIndex];
        if (completed.EntityID >= 0)
        {
            DecalVisibilityObservation observation;
            observation.HasSample = true;
            observation.Submitted = completed.Submitted;
            observation.DrawIssued = completed.DrawIssued;
            if (completed.ReceiverQueryIssued && s_Data.DecalVisibilityQueriesInitialized)
            {
                observation.ReceiverIntersectionKnown = true;
                observation.ReceiverIntersectsProjection =
                    RenderCommand::GetQueryResultU32(s_Data.DecalReceiverIntersectionQueries[completedIndex]) != 0u;
            }
            if (completed.QueryIssued && s_Data.DecalVisibilityQueriesInitialized)
            {
                // This targeted diagnostic deliberately permits the readback to
                // block. It is armed only by olo_render_why_not_visible and the
                // query belongs to the frame that just completed.
                observation.FragmentResultKnown = true;
                observation.FragmentsSurvived =
                    RenderCommand::GetQueryResultU32(s_Data.DecalVisibilityQueries[completedIndex]) != 0u;
            }
            s_Data.LastDecalVisibilityEntity = completed.EntityID;
            s_Data.LastDecalVisibilityObservation = observation;
        }

        s_Data.DecalVisibilityWriteBuffer = 1u - completedIndex;
        auto& next = s_Data.DecalVisibilityFrames[s_Data.DecalVisibilityWriteBuffer];
        next = {};
        next.EntityID = s_Data.DecalVisibilityTarget;
    }

    bool Renderer3D::IsDecalVisibilityObserved(i32 entityID)
    {
        return entityID == s_Data.DecalVisibilityTarget &&
               entityID == s_Data.DecalVisibilityFrames[s_Data.DecalVisibilityWriteBuffer].EntityID;
    }

    bool Renderer3D::BeginDecalVisibilityQuery(i32 entityID)
    {
        auto& active = s_Data.DecalVisibilityFrames[s_Data.DecalVisibilityWriteBuffer];
        if (entityID != s_Data.DecalVisibilityTarget || active.EntityID != entityID)
            return false;

        active.DrawIssued = true;
        if (!s_Data.DecalVisibilityQueriesInitialized || active.QueryIssued)
            return false;

        RenderCommand::BeginQuery(RHI::QueryType::OcclusionAnySamples,
                                  s_Data.DecalVisibilityQueries[s_Data.DecalVisibilityWriteBuffer]);
        active.QueryIssued = true;
        return true;
    }

    bool Renderer3D::BeginDecalReceiverIntersectionQuery(i32 entityID)
    {
        auto& active = s_Data.DecalVisibilityFrames[s_Data.DecalVisibilityWriteBuffer];
        if (entityID != s_Data.DecalVisibilityTarget || active.EntityID != entityID ||
            !s_Data.DecalVisibilityQueriesInitialized || active.ReceiverQueryIssued)
        {
            return false;
        }

        RenderCommand::BeginQuery(RHI::QueryType::OcclusionAnySamples,
                                  s_Data.DecalReceiverIntersectionQueries[s_Data.DecalVisibilityWriteBuffer]);
        active.ReceiverQueryIssued = true;
        return true;
    }

    void Renderer3D::EndDecalReceiverIntersectionQuery()
    {
        RenderCommand::EndQuery(RHI::QueryType::OcclusionAnySamples);
    }

    void Renderer3D::EndDecalVisibilityQuery()
    {
        RenderCommand::EndQuery(RHI::QueryType::OcclusionAnySamples);
    }

    void Renderer3D::SetRenderScale(f32 scale)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.RGraph)
        {
            return;
        }

        s_Data.RGraph->SetRenderScale(scale);

        // Upload immediately so the DRS UBO reflects the new scale even if called
        // outside the normal `RenderPipeline::PrepareFrame(...)` path (e.g. from
        // the settings panel).
        const glm::vec2 bounds = s_Data.RGraph->GetRenderScaleBounds();
        s_Data.SceneEffectsGPU.DRSData.RenderScaleBounds = bounds;
        if (s_Data.SceneEffectsGPU.DRS)
        {
            s_Data.SceneEffectsGPU.DRS->SetData(&s_Data.SceneEffectsGPU.DRSData, DRSUBOData::GetSize());
        }
    }

    ShaderLibrary& Renderer3D::GetShaderLibrary()
    {
        return m_ShaderLibrary;
    }
} // namespace OloEngine
