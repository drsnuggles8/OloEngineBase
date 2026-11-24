#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Crowd/CrowdClip.h"

#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <array>
#include <cmath>
#include <utility>

namespace OloEngine
{
    namespace
    {
        constexpr f64 kSamplesPerSecond = 60.0;
        // The portable minimum SSBO range on the supported GL/Vulkan devices.
        // The draw owner separately checks the combined instance prefix and atlas.
        constexpr u32 kMaxAtlasRecords = (128u * 1024u * 1024u) / sizeof(InstanceData);
        constexpr std::array<glm::mat4 InstanceData::*, 3> kMatrixLanes{
            &InstanceData::Transform, &InstanceData::Normal, &InstanceData::PrevTransform
        };

        [[nodiscard]] bool HasSampleableRig(const Skeleton& rig)
        {
            const sizet n = rig.m_BoneNames.size();
            if (n == 0 || n > UBOStructures::AnimationConstants::MAX_BONES ||
                rig.m_ParentIndices.size() != n || rig.m_LocalTransforms.size() != n ||
                rig.m_GlobalTransforms.size() != n || rig.m_FinalBoneMatrices.size() != n ||
                (!rig.m_BindPoseLocalTransforms.empty() && rig.m_BindPoseLocalTransforms.size() != n))
                return false;

            // AnimationSystem evaluates globals in the importer's parent-first order.
            for (sizet i = 0; i < n; ++i)
                if (rig.m_ParentIndices[i] >= static_cast<i32>(i))
                    return false;
            return true;
        }
    } // namespace

    const glm::mat4& CrowdClip::Bone(u32 frame, u32 bone) const
    {
        OLO_CORE_ASSERT(frame < FrameCount && bone < BoneCount, "Crowd clip palette index out of range");
        return Atlas[frame * RecordsPerFrame() + bone / 3u].*kMatrixLanes[bone % 3u];
    }

    bool BakeCrowdClip(const Skeleton& rig, const Ref<AnimationClip>& clip, CrowdClip& out)
    {
        out = {};
        if (!clip || !std::isfinite(clip->Duration) || clip->Duration <= 0.0f || !HasSampleableRig(rig))
            return false;

        CrowdClip baked;
        baked.BoneCount = static_cast<u32>(rig.m_BoneNames.size());
        baked.Duration = clip->Duration;
        const u32 stride = baked.RecordsPerFrame();
        const f64 intervals = std::ceil(static_cast<f64>(clip->Duration) * kSamplesPerSecond);
        if (intervals >= static_cast<f64>(kMaxAtlasRecords / stride))
            return false;

        baked.FrameCount = static_cast<u32>(intervals) + 1u;
        baked.Atlas.SetNum(static_cast<i32>(baked.FrameCount * stride));
        auto pose = rig.Instantiate();
        AnimationStateComponent state(clip);
        state.m_Loop = false; // Sample the authored end key at duration, including for a non-looping actor.
        for (u32 frame = 0; frame < baked.FrameCount; ++frame)
        {
            state.m_CurrentTime = static_cast<f32>(static_cast<f64>(clip->Duration) * frame / intervals);
            Animation::AnimationSystem::Update(state, *pose, 0.0f);
            for (u32 bone = 0; bone < baked.BoneCount; ++bone)
            {
                const glm::mat4& matrix = pose->m_FinalBoneMatrices[bone];
                if (!Math::IsFinite(matrix))
                    return false;
                baked.Atlas[frame * stride + bone / 3u].*kMatrixLanes[bone % 3u] = matrix;
            }
        }
        out = std::move(baked);
        return true;
    }
} // namespace OloEngine
