#pragma once

#include "OloEngine/Renderer/Crowd/CrowdAnimationComponent.h"
#include "OloEngine/Renderer/Crowd/CrowdClip.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <array>
#include <span>

namespace OloEngine
{
    struct CrowdDrawRange
    {
        u32 Count = 0;
        u32 FirstIndex = 0;
    };

    // A batch owns one for each view and submesh. Each compacted SSBO's
    // prefix is large enough for all inputs and its tail holds the shared
    // clip. View history follows a member's stable ID when a clip group
    // changes membership or order.
    class CrowdCull : public RefCounted
    {
      public:
        // Clocks and InstanceData::Custom offsets are in cycles. Near interpolates
        // the full clip. Mid and far interpolate the eight intervals of a
        // downsampled clip. Both clock samples feed the velocity-producing programs.
        [[nodiscard]] bool Dispatch(std::span<const InstanceData> instances, const CrowdClip& clip,
                                    f32 now, f32 previous, bool loop, bool resetHistory,
                                    const CrowdAnimationComponent& settings, glm::vec4 localSphere, glm::vec4 cullSphere,
                                    const std::array<CrowdDrawRange, 3>& ranges);
        [[nodiscard]] RHI::ResourceHandle Instances(u32 tier) const
        {
            return m_Output[tier]->GetRHIHandle();
        }
        [[nodiscard]] RHI::ResourceHandle Indirect(u32 tier) const
        {
            return m_Indirect[tier]->GetRHIHandle();
        }

      private:
        bool PrepareBuffers(u32 count, const CrowdClip& clip);
        Ref<ComputeShader> m_Shader;
        Ref<StorageBuffer> m_Input, m_PreviousHistory, m_NextHistory;
        std::array<Ref<StorageBuffer>, 3> m_Output;
        std::array<Ref<StorageBuffer>, 3> m_Indirect;
        Ref<UniformBuffer> m_Parameters;
        u32 m_Capacity = 0;
        TArray<u64> m_PreviousIDs;
        const InstanceData* m_AtlasData = nullptr;
        u64 m_Frame = 0;
    };
} // namespace OloEngine
