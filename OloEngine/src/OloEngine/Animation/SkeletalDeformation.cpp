#include "OloEnginePCH.h"
#include "OloEngine/Animation/SkeletalDeformation.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Scene/Scene.h"

namespace OloEngine::Animation
{
    namespace
    {
        // Main-thread state. The advance walk runs at Scene's frame boundary,
        // ahead of both the gameplay tick and all render submission, and the
        // reset entry points are called from scene and play-mode transitions on
        // the same thread, so these need no synchronisation — see the note on
        // AdvanceHistory.
        SkeletalDeformationStats s_Stats;

        void CountReset(DeformationHistoryResetCause cause)
        {
            ++s_Stats.HistoryResets;
            s_Stats.LastResetCause = cause;
            switch (cause)
            {
                case DeformationHistoryResetCause::FirstUse:
                    ++s_Stats.HistoryResetsFirstUse;
                    break;
                case DeformationHistoryResetCause::BoneCountChanged:
                    ++s_Stats.HistoryResetsBoneCountChanged;
                    break;
                default:
                    ++s_Stats.HistoryResetsExplicit;
                    break;
            }
        }
    } // namespace

    std::string_view ToString(DeformationHistoryResetCause cause)
    {
        switch (cause)
        {
            case DeformationHistoryResetCause::None:
                return "None";
            case DeformationHistoryResetCause::FirstUse:
                return "FirstUse";
            case DeformationHistoryResetCause::BoneCountChanged:
                return "BoneCountChanged";
            case DeformationHistoryResetCause::SkeletonReplaced:
                return "SkeletonReplaced";
            case DeformationHistoryResetCause::SceneTransition:
                return "SceneTransition";
            case DeformationHistoryResetCause::Teleport:
                return "Teleport";
            case DeformationHistoryResetCause::Manual:
                return "Manual";
        }
        return "Unknown";
    }

    u32 SkeletalDeformationSystem::AdvanceHistory(Scene* scene)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
            return 0;

        // Only the per-frame counters are cleared here. The reset counters are
        // session totals on purpose -- see SkeletalDeformationStats.
        s_Stats.BeginFrame();

        auto view = scene->GetAllEntitiesWith<SkeletonComponent>();
        for (auto entityID : view)
        {
            auto& skeletonComponent = view.template get<SkeletonComponent>(entityID);
            if (!skeletonComponent.m_Skeleton)
                continue;

            Skeleton& skeleton = *skeletonComponent.m_Skeleton;
            const BoneHistoryAdvance outcome = skeleton.AdvanceBoneHistory();

            ++s_Stats.SkeletonsAdvanced;
            s_Stats.BoneMatricesAdvanced += static_cast<u32>(skeleton.m_FinalBoneMatrices.size());

            switch (outcome)
            {
                case BoneHistoryAdvance::Advanced:
                    ++s_Stats.SkeletonsWithHistory;
                    break;

                case BoneHistoryAdvance::FirstUse:
                    CountReset(DeformationHistoryResetCause::FirstUse);
                    break;

                case BoneHistoryAdvance::BoneCountChanged:
                    CountReset(DeformationHistoryResetCause::BoneCountChanged);
                    OLO_CORE_WARN(
                        "SkeletalDeformation: bone count changed under entity {} — deformation history "
                        "dropped, this frame emits zero bone motion",
                        std::to_underlying(entityID));
                    break;

                case BoneHistoryAdvance::PendingReset:
                    // Already counted, with its real cause, by whoever declared
                    // the discontinuity. Counting it again here would both
                    // double it and overwrite that cause with a generic one.
                    break;

                case BoneHistoryAdvance::NoBonesYet:
                    // Nothing was lost: this skeleton never had a previous pose.
                    break;
            }
        }

        return s_Stats.SkeletonsAdvanced;
    }

    u32 SkeletalDeformationSystem::ResetHistory(Scene* scene, DeformationHistoryResetCause cause)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
            return 0;

        u32 resetCount = 0;
        auto view = scene->GetAllEntitiesWith<SkeletonComponent>();
        for (auto entityID : view)
        {
            auto& skeletonComponent = view.template get<SkeletonComponent>(entityID);
            if (!skeletonComponent.m_Skeleton)
                continue;

            skeletonComponent.m_Skeleton->ResetBoneHistory();
            CountReset(cause);
            ++resetCount;
        }

        if (resetCount > 0)
        {
            OLO_CORE_TRACE("SkeletalDeformation: dropped deformation history for {} skeleton(s), cause {}",
                           resetCount, ToString(cause));
        }
        return resetCount;
    }

    const SkeletalDeformationStats& SkeletalDeformationSystem::GetStats()
    {
        return s_Stats;
    }

    void SkeletalDeformationSystem::ResetStats()
    {
        s_Stats.Reset();
    }

    void SkeletalDeformationSystem::NoteExplicitReset(DeformationHistoryResetCause cause)
    {
        CountReset(cause);
    }
} // namespace OloEngine::Animation
