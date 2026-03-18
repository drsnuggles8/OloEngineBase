#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Navigation/NavMesh.h"

#include <DetourCrowd.h>

#include <glm/glm.hpp>

namespace OloEngine
{
    struct NavAgentComponent;

    class CrowdManager
    {
      public:
        CrowdManager() = default;
        ~CrowdManager();

        CrowdManager(const CrowdManager&) = delete;
        CrowdManager& operator=(const CrowdManager&) = delete;

        void Initialize(const Ref<NavMesh>& navMesh, i32 maxAgents = 256);
        void Shutdown();

        i32 AddAgent(const glm::vec3& position, const NavAgentComponent& params);
        void RemoveAgent(i32 agentId);
        void SetAgentTarget(i32 agentId, const glm::vec3& target);
        void Update(f32 dt);

        [[nodiscard]] bool GetAgentPosition(i32 agentId, glm::vec3& outPosition) const;
        [[nodiscard]] bool GetAgentVelocity(i32 agentId, glm::vec3& outVelocity) const;
        [[nodiscard]] bool IsAgentActive(i32 agentId) const;
        [[nodiscard]] bool IsValid() const
        {
            return m_Crowd != nullptr;
        }
        [[nodiscard]] i32 GetActiveAgentCount() const;

      private:
        dtCrowd* m_Crowd = nullptr;
        Ref<NavMesh> m_NavMesh;
    };
} // namespace OloEngine
