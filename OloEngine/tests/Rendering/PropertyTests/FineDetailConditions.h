#pragma once

// =============================================================================
// FineDetailConditions: what a fine-detail A/B has to hold constant (issue #1401).
//
// `VisualEvidence::FineDetailDensity` is a property of a frame at one output
// resolution under one post stack. Two arms captured under different
// conditions differ for reasons that have nothing to do with the feature, so
// the A/B snapshots the conditions at each arm's capture time and
// `VisualEvidence::ExpectConditionsPinned` compares them.
//
// Kept apart from VisualEvidenceGuards.h so the guards stay renderer-free.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"

namespace OloEngine::Tests::VisualEvidence
{
    struct FineDetailConditions
    {
        u32 Width = 0;
        u32 Height = 0;
        RenderingPath Path = RenderingPath::Forward;
        u32 DeferredMsaaSamples = 1;
        PostProcessSettings Post;

        auto operator==(const FineDetailConditions&) const -> bool = default;
    };

    /// The live renderer's conditions right now. Call it immediately after the
    /// arm's capture, on the same code path the capture used.
    [[nodiscard]] inline FineDetailConditions SnapshotFineDetailConditions(u32 width, u32 height)
    {
        const auto& renderer = Renderer3D::GetRendererSettings();
        FineDetailConditions conditions;
        conditions.Width = width;
        conditions.Height = height;
        conditions.Path = renderer.Path;
        conditions.DeferredMsaaSamples = renderer.Deferred.MSAASampleCount;
        conditions.Post = Renderer3D::GetPostProcessSettings();
        return conditions;
    }
} // namespace OloEngine::Tests::VisualEvidence
