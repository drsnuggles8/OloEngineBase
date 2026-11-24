#pragma once

#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"

namespace OloEngine
{
    // Each frame occupies ceil(BoneCount / 3) records. Transform, Normal
    // and PrevTransform hold successive bone matrices in the atlas tail of
    // the draw's instance SSBO. Frames include samples at zero and duration.
    // Rigid draws and ordinary skinned draws use the same record prefix.
    struct CrowdClip
    {
        TArray<InstanceData> Atlas;
        u32 FrameCount = 0;
        u32 BoneCount = 0;
        f32 Duration = 0.0f;

        [[nodiscard]] u32 RecordsPerFrame() const
        {
            return (BoneCount + 2u) / 3u;
        }
        [[nodiscard]] const glm::mat4& Bone(u32 frame, u32 bone) const;
    };

    // Uses the engine animation evaluator on a private rig and never mutates
    // the owner's skeleton or playback clock. Invalid input leaves out empty.
    [[nodiscard]] bool BakeCrowdClip(const Skeleton& rig, const Ref<AnimationClip>& clip, CrowdClip& out);
} // namespace OloEngine
