#include "OloEnginePCH.h"
#include "OloEngine/Animation/SkeletalDeformation.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
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
                case DeformationHistoryResetCause::MorphSurfaceChanged:
                    ++s_Stats.HistoryResetsMorphSurfaceChanged;
                    break;
                case DeformationHistoryResetCause::MorphSetChanged:
                    ++s_Stats.HistoryResetsMorphSetChanged;
                    break;
                case DeformationHistoryResetCause::MeshTopologyChanged:
                    ++s_Stats.HistoryResetsMeshTopologyChanged;
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
            case DeformationHistoryResetCause::MorphSurfaceChanged:
                return "MorphSurfaceChanged";
            case DeformationHistoryResetCause::MorphSetChanged:
                return "MorphSetChanged";
            case DeformationHistoryResetCause::MeshTopologyChanged:
                return "MeshTopologyChanged";
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
            // Attribute before the next advance can overwrite it: the record
            // this skeleton produces carries the cause, not just the fact
            // (#1228).
            skeletonComponent.m_Skeleton->NoteDeformationResetCause(static_cast<u8>(cause));
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

    void SkeletalDeformationSystem::NoteMorphUnknownTargets(u32 count)
    {
        s_Stats.MorphUnknownTargets += count;
    }

    void SkeletalDeformationSystem::NoteMorphIncompatibleSet()
    {
        ++s_Stats.MorphIncompatibleSets;
    }

    void SkeletalDeformationSystem::NoteMorphBaseCacheInvalidated()
    {
        ++s_Stats.MorphBaseCacheInvalidations;
    }

    u32 MorphDeformationSystem::AdvanceHistory(Scene* scene)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
            return 0;

        // NOT BeginFrame() -- SkeletalDeformationSystem::AdvanceHistory already
        // cleared the per-frame counters for this frame, and the two halves of the
        // surface share one stats block on purpose.
        auto view = scene->GetAllEntitiesWith<MorphTargetComponent>();
        for (auto entityID : view)
        {
            auto& morph = view.template get<MorphTargetComponent>(entityID);

            // Rotate the weight vector the last RENDERED frame deformed with into
            // the previous slot, whether or not this entity animated. An entity
            // skipped while paused keeps prev and current one frame apart and
            // re-reports that stale difference forever -- the bone-side failure
            // #1226 fixed, in the morph half.
            morph.PrevAppliedWeights = morph.AppliedWeights;
            morph.HasMorphHistory = morph.HasAppliedSurface;

            // The rejection is a statement about ONE frame: the surface moved
            // between the last frame and this one. It is re-decided by the morph
            // evaluation below, so it must not survive into a frame that did not
            // re-decide it -- a paused morphing entity would otherwise be stuck
            // emitting zero motion for the length of the pause.
            morph.RejectDeformationHistory = false;

            ++s_Stats.MorphSurfacesAdvanced;
            if (morph.HasMorphHistory)
                ++s_Stats.MorphSurfacesWithHistory;
        }

        return s_Stats.MorphSurfacesAdvanced;
    }

    u32 MorphDeformationSystem::ResetHistory(Scene* scene, DeformationHistoryResetCause cause)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
            return 0;

        u32 resetCount = 0;
        auto view = scene->GetAllEntitiesWith<MorphTargetComponent>();
        for (auto entityID : view)
        {
            auto& morph = view.template get<MorphTargetComponent>(entityID);
            morph.PrevAppliedWeights.clear();
            morph.HasMorphHistory = false;
            morph.RejectDeformationHistory = true;
            CountReset(cause);
            ++resetCount;
        }

        if (resetCount > 0)
        {
            OLO_CORE_TRACE("MorphDeformation: dropped morph history for {} entit(ies), cause {}",
                           resetCount, ToString(cause));
        }
        return resetCount;
    }

    void RejectDeformationHistory(Skeleton* skeleton, MorphTargetComponent* morph,
                                  DeformationHistoryResetCause cause)
    {
        bool rejectedAnything = false;

        if (skeleton)
        {
            skeleton->ResetBoneHistory();
            skeleton->NoteDeformationResetCause(static_cast<u8>(cause));
            rejectedAnything = true;
        }
        if (morph)
        {
            morph->RejectDeformationHistory = true;
            morph->HasMorphHistory = false;
            morph->PrevAppliedWeights.clear();
            rejectedAnything = true;
            // Only a MORPH surface counts here. A skeleton-only rejection (an LOD
            // switch on an unmorphed character) bumping this would make the
            // olo_skeletal_deformation_stats `morph.surfacesRejected` field report
            // entities that have no morph surface at all, which inverts the one
            // question it exists to answer.
            ++s_Stats.MorphSurfacesRejected;
        }

        if (rejectedAnything)
        {
            CountReset(cause);
        }
    }
} // namespace OloEngine::Animation
