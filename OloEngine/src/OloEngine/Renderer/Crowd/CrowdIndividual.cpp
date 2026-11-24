#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Crowd/CrowdIndividual.h"

#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/SkeletalDeformation.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace OloEngine
{
    namespace
    {
        f32 OffsetClipTime(const Ref<AnimationClip>& clip, f32 seconds, f32 offsetCycles, bool loop)
        {
            if (!clip || !std::isfinite(clip->Duration) || clip->Duration <= 0.0f || !std::isfinite(seconds))
                return 0.0f;
            // Work in double to wrap even very large authored phase offsets
            // before they reach the float animation clock.
            const f64 cycles = static_cast<f64>(seconds) / clip->Duration + offsetCycles;
            const f64 phase = loop ? cycles - std::floor(cycles) : std::clamp(cycles, 0.0, 1.0);
            return static_cast<f32>(phase * clip->Duration);
        }

        void SampleAtPhase(Skeleton& pose, const Skeleton& source, const AnimationStateComponent& animation, f32 offset)
        {
            if (!animation.m_CurrentClip)
            {
                pose.m_FinalBoneMatrices = source.m_FinalBoneMatrices;
                return;
            }
            // The owner's tick has already advanced playback and the blend.
            // A zero-time sample preserves its root-pin/extraction policy and
            // weights, while applying the member offset to both clips.
            AnimationStateComponent state(animation.m_CurrentClip);
            state.m_NextClip = animation.m_NextClip;
            state.m_Blending = animation.m_Blending;
            state.m_BlendDuration = animation.m_BlendDuration;
            state.m_BlendTime = animation.m_BlendTime;
            state.m_BlendFactor = animation.m_BlendFactor;
            state.m_Loop = animation.m_Loop;
            state.m_NextLoop = animation.m_NextLoop;
            state.m_RootMotion = animation.m_RootMotion;
            const f32 cycles = std::isfinite(offset) ? offset : 0.0f;
            state.m_CurrentTime = OffsetClipTime(state.m_CurrentClip, animation.m_CurrentTime, cycles, state.m_Loop);
            state.m_NextTime = OffsetClipTime(state.m_NextClip, animation.m_NextTime, cycles, state.m_NextLoop);
            Animation::AnimationSystem::Update(state, pose, 0.0f);
        }

        // DrawAnimatedMesh can return nullptr for a camera-culled draw, or
        // after routing it directly to the overlay. The shadow camera still
        // needs the member's pose in both cases.
        void SubmitShadow(const Ref<Mesh>& mesh, const glm::mat4& transform, std::span<const glm::mat4> bones,
                          const CommandPacket* packet)
        {
            const auto vao = mesh->GetVertexArray();
            if (!vao || bones.empty())
                return;
            const auto* cmd = packet ? packet->GetCommandData<DrawMeshCommand>() : nullptr;
            const u32 count = cmd ? cmd->boneCount : static_cast<u32>(bones.size());
            u32 offset = 0;
            if (cmd)
                offset = cmd->boneBufferOffset;
            else
            {
                auto& arena = FrameDataBufferManager::Get();
                offset = arena.AllocateBoneMatrices(count, FrameDataBuffer::BoneAllocation::GrowDuringGather);
                if (offset == UINT32_MAX)
                    return;
                arena.WriteBoneMatrices(offset, bones.data(), count);
            }
            Renderer3D::AddSkinnedShadowCaster(vao->GetRHIHandle(), mesh->GetIndexCount(), mesh->GetBaseIndex(),
                                               transform, offset, count, mesh->GetTransformedBoundingBox(transform));
        }
    } // namespace

    const CrowdIndividual::Sample& CrowdIndividual::Evaluate(u64 owner, const InstanceData& member, const Ref<Skeleton>& rig,
                                                             const AnimationStateComponent& animation, u64 serial)
    {
        auto& entry = m_Owners[owner][member.StableID];
        auto& result = entry.Rendered;
        const bool hadPose = result.Pose != nullptr;
        const bool replaced = hadPose && entry.Source != rig;
        const bool sameRig = result.Pose && entry.Source == rig &&
                             result.Pose->m_FinalBoneMatrices.size() == rig->m_FinalBoneMatrices.size();
        if (sameRig && entry.Serial == serial)
            return result;

        const bool continuous = sameRig && entry.Serial + 1u == serial && rig->HasBoneHistory();
        if (!sameRig || !rig->HasBoneHistory())
        {
            entry.Source = rig;
            result.Pose = rig->Instantiate();
        }
        if (continuous)
            result.Pose->AdvanceBoneHistory();
        result.PreviousTransform = continuous ? result.Transform : member.Transform;
        result.Transform = member.Transform;
        SampleAtPhase(*result.Pose, *rig, animation, member.Custom);
        if (!continuous)
        {
            result.Pose->ResetBoneHistory();
            // Consume the reset on this discontinuous frame so the next
            // consecutive frame has a real previous member pose.
            result.Pose->AdvanceBoneHistory();
            u8 cause = rig->m_DeformationResetCause != kCauseNone ? rig->m_DeformationResetCause : kCauseFirstUse;
            if (hadPose && !sameRig)
                cause = replaced ? static_cast<u8>(Animation::DeformationHistoryResetCause::SkeletonReplaced) : kCauseBoneCountChanged;
            result.Pose->NoteDeformationResetCause(cause);
        }
        entry.Serial = serial;
        return result;
    }

    void CrowdIndividual::SubmitPart(u64 owner, i32 pickingID, const Ref<Mesh>& mesh, const Ref<Skeleton>& rig,
                                     const AnimationStateComponent& animation, const InstanceData& member, const Material& material,
                                     bool shadows, const GPUSceneMaterialKey& key)
    {
        if (!mesh || !rig || rig->m_FinalBoneMatrices.empty())
            return;
        const auto& sample = Evaluate(owner, member, rig, animation, Renderer3D::GetPhysicalFrameSerial());
        const Skeleton& pose = *sample.Pose;
        const GPUSceneAnimatedSurface animated{
            .m_IsAnimated = true,
            .m_DeformationRevision = pose.m_DeformationRevision,
            .m_PrevDeformationRevision = pose.m_PrevDeformationRevision,
            .m_ResetCause = pose.m_DeformationResetCause,
            .m_BonePalette = pose.m_FinalBoneMatrices.data(),
            .m_BoneCount = static_cast<u32>(pose.m_FinalBoneMatrices.size()),
        };
        const u32 link = Renderer3D::ExtractGPUSceneMesh(owner, member.StableID, mesh->GetMeshSource(), mesh->GetSubmeshIndex(),
                                                         sample.Transform, key, GPUSceneDrawLinkRequest::Link, animated,
                                                         InstanceTint(member.Color));
        InstanceData instance = member;
        instance.PrevTransform = sample.PreviousTransform;
        const std::span<const glm::mat4> previous = pose.HasBoneHistory() ? std::span<const glm::mat4>{ pose.m_PrevFinalBoneMatrices }
                                                                          : std::span<const glm::mat4>{};
        auto* packet = Renderer3D::DrawAnimatedMesh(mesh, sample.Transform, material, pose.m_FinalBoneMatrices, previous, false,
                                                    member.EntityID >= 0 ? member.EntityID : pickingID, link, &instance);
        if (packet)
            Renderer3D::SubmitPacket(packet);
        if (shadows && MaterialCastsShadows(material))
            SubmitShadow(mesh, sample.Transform, pose.m_FinalBoneMatrices, packet);
    }

    void CrowdIndividual::Remove(u64 owner)
    {
        m_Owners.erase(owner);
    }

    void CrowdIndividual::FinishFrame(u64 serial)
    {
        for (auto& [owner, members] : m_Owners)
        {
            // The caller can finish one view before a later view of this
            // physical frame first asks for the member.
            std::erase_if(members, [serial](const auto& pair)
                          { return serial - pair.second.Serial > 1u; });
        }
        std::erase_if(m_Owners, [](const auto& pair)
                      { return pair.second.empty(); });
    }
} // namespace OloEngine
