#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/SkeletonData.h"

#include <algorithm>
#include <string_view>

// =============================================================================
// BoneAttachment.h — how a BoneAttachmentComponent resolves (issue #1533).
//
// The component lives in Scene/Components.h beside RelationshipComponent; this
// header holds the resolution vocabulary the Scene, the inspector and the tests
// share, so the reason an attachment is not following its bone has one spelling.
// =============================================================================

namespace OloEngine
{
    // Why a BoneAttachmentComponent does or does not follow its bone. Every value
    // except Attached composes parent-relative instead, and is reported: a warning
    // once per (entity, reason) from Scene::PropagateWorldTransforms, and the value
    // itself in the inspector. The values index a warned-reasons bit mask, so they
    // stay below 32.
    enum class BoneAttachmentStatus : u8
    {
        // Composing against the bone this frame.
        Attached = 0,
        // m_Enabled is false: parent-relative, by the author's choice.
        Disabled,
        // No resolvable parent entity (none, dangling, or transform-less).
        NoParent,
        // The parent carries no SkeletonComponent, or its skeleton is null.
        ParentHasNoSkeleton,
        // m_BoneName is empty.
        NoBoneName,
        // The parent's skeleton has no bone of that name, or no posed global
        // transform for it (m_GlobalTransforms shorter than m_BoneNames).
        UnknownBone,
    };

    inline constexpr u32 kBoneAttachmentStatusCount = 6;
    static_assert(kBoneAttachmentStatusCount <= 32, "BoneAttachmentStatus indexes a u32 warned-reasons mask");

    [[nodiscard]] constexpr const char* BoneAttachmentStatusToString(BoneAttachmentStatus status) noexcept
    {
        switch (status)
        {
            case BoneAttachmentStatus::Attached:
                return "Attached";
            case BoneAttachmentStatus::Disabled:
                return "Disabled";
            case BoneAttachmentStatus::NoParent:
                return "NoParent";
            case BoneAttachmentStatus::ParentHasNoSkeleton:
                return "ParentHasNoSkeleton";
            case BoneAttachmentStatus::NoBoneName:
                return "NoBoneName";
            case BoneAttachmentStatus::UnknownBone:
                return "UnknownBone";
        }
        return "Unknown";
    }

    // One sentence per reason, for the log line and the inspector's status row.
    [[nodiscard]] constexpr const char* DescribeBoneAttachmentStatus(BoneAttachmentStatus status) noexcept
    {
        switch (status)
        {
            case BoneAttachmentStatus::Attached:
                return "following the bone";
            case BoneAttachmentStatus::Disabled:
                return "the attachment is disabled";
            case BoneAttachmentStatus::NoParent:
                return "the entity has no parent to take a skeleton from";
            case BoneAttachmentStatus::ParentHasNoSkeleton:
                return "the parent entity has no SkeletonComponent (or its skeleton is not loaded)";
            case BoneAttachmentStatus::NoBoneName:
                return "no bone name is set";
            case BoneAttachmentStatus::UnknownBone:
                return "the parent's skeleton has no posed bone of that name";
        }
        return "unknown";
    }

    struct BoneAttachmentResolution
    {
        BoneAttachmentStatus Status = BoneAttachmentStatus::NoParent;
        // Index into the parent skeleton's m_GlobalTransforms; valid only when
        // Status == Attached, -1 otherwise.
        i32 BoneIndex = -1;
    };

    // The index of the bone named `boneName` that also HAS a posed global
    // transform, or -1. A name past the end of m_GlobalTransforms is not
    // attachable: composing against it would read out of bounds.
    [[nodiscard]] inline i32 FindPosedBoneIndex(const SkeletonData& skeleton, std::string_view boneName) noexcept
    {
        if (boneName.empty())
        {
            return -1;
        }
        const sizet posedCount = std::min(skeleton.m_BoneNames.size(), skeleton.m_GlobalTransforms.size());
        for (sizet i = 0; i < posedCount; ++i)
        {
            if (skeleton.m_BoneNames[i] == boneName)
            {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }
} // namespace OloEngine
