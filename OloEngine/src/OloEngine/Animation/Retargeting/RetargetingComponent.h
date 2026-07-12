#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/ComponentReflection.h"
#include "OloEngine/Animation/AnimationClip.h"

#include <bit>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Live (un-baked) animation retargeting (issue #631 part 2 — deferral #3 of
    // docs/design/animation-retargeting.md): plays clips authored for a FOREIGN
    // skeleton on this entity's rig with no offline bake step. The retargeting
    // system lazily retargets every source clip onto the entity's skeleton
    // (rotation re-base + per-bone translation ratio + humanoid-role mapping)
    // and splices the results into AnimationStateComponent::m_AvailableClips
    // under their original names, so clip playback, animation graphs
    // (ResolveClips by name) and root-motion extraction all work unchanged.
    //
    // Source rig + clips come from ONE of:
    //  * m_SourceEntity — a scene entity carrying SkeletonComponent +
    //    AnimationStateComponent (its m_AvailableClips are the source
    //    animations). Takes precedence; works headless.
    //  * m_SourcePath — a model file (FBX/GLTF) loaded lazily at runtime.
    struct RetargetingComponent
    {
        OLO_PROPERTY()
        bool Enabled = true;

        // Model file whose skeleton the source clips were authored for.
        std::string m_SourcePath;

        // Scene entity providing the source skeleton + clips (see above).
        UUID m_SourceEntity = 0;

        // Map bones by humanoid role (AutoDetect + overrides below), falling
        // back to name matching for unmapped bones. Off = name matching only.
        OLO_PROPERTY()
        bool UseHumanoidRoles = true;

        // Transfer non-root translation deltas scaled by the per-bone length
        // ratio (RetargetOptions::TranslationMode::PerBoneRatio, part 2a).
        OLO_PROPERTY()
        bool PerBoneTranslation = true;

        // Transfer the root bone's translation (locomotion / hip motion).
        OLO_PROPERTY()
        bool TransferRootTranslation = true;

        // Scale for the transferred root translation; 0 = derive automatically
        // from the two rigs' bind-pose extents (ComputeRootTranslationScale).
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1000.0f)
        f32 RootTranslationScale = 0.0f;

        // Hand corrections for the heuristic role detector, applied after
        // AutoDetect: bone name (exact) → HumanoidBone role (int). A negative
        // role clears the bone's detected assignment.
        std::unordered_map<std::string, i32> m_SourceRoleOverrides;
        std::unordered_map<std::string, i32> m_TargetRoleOverrides;

        // Manual operator== — a defaulted one would trip the UUID C2666
        // ambiguity on MSVC (see docs/agent-rules/cpp-coding-quality.md §7).
        // Equality covers the authored settings only; used by the editor's
        // tier-2 undo detection and the retargeting system's rebake check.
        auto operator==(const RetargetingComponent& other) const -> bool
        {
            return Enabled == other.Enabled &&
                   m_SourcePath == other.m_SourcePath &&
                   static_cast<u64>(m_SourceEntity) == static_cast<u64>(other.m_SourceEntity) &&
                   UseHumanoidRoles == other.UseHumanoidRoles &&
                   PerBoneTranslation == other.PerBoneTranslation &&
                   TransferRootTranslation == other.TransferRootTranslation &&
                   // bitwise float compare: settings-equality (rebake/undo
                   // detection), not numeric math — mirrors the tier-1 memcmp.
                   std::bit_cast<u32>(RootTranslationScale) == std::bit_cast<u32>(other.RootTranslationScale) &&
                   m_SourceRoleOverrides == other.m_SourceRoleOverrides &&
                   m_TargetRoleOverrides == other.m_TargetRoleOverrides;
        }
    };

    // Runtime-only cache for RetargetingComponent. Created on demand by the
    // retargeting system; deliberately NOT serialized and NOT in the
    // AllComponents tuple — a scene copy (play mode) rebakes lazily from the
    // authored settings.
    struct RetargetingStateComponent
    {
        // The settings the current bake was produced from; a mismatch with the
        // live component triggers a rebake (RetargetingComponent::operator==).
        RetargetingComponent BakedFromSettings;

        // True once a bake was attempted for BakedFromSettings — also set on
        // failure (missing file / no skeleton), so a bad path warns once
        // instead of re-loading from disk every tick.
        bool Attempted = false;

        // The retargeted clips, spliced into the entity's
        // AnimationStateComponent::m_AvailableClips under their source names.
        std::vector<Ref<AnimationClip>> BakedClips;
    };
} // namespace OloEngine
