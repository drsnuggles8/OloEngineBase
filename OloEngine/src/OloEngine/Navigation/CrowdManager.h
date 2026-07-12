#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Navigation/NavMesh.h"

#include <DetourCrowd.h>

#include <glm/glm.hpp>

namespace OloEngine
{
    struct NavAgentComponent;

    // Mirrors the terminal states NavigationSystem's manual follower already
    // exposes via NavAgentComponent::m_TargetUnreachable (see FindPathResult),
    // so the crowd path can surface the same observable contract to consumers
    // (BTMoveTo / scripts) regardless of which follower an agent is using.
    enum class CrowdTargetState
    {
        None,        // no active move request
        Pending,     // path/queue request in flight (REQUESTING / WAITING_FOR_*)
        Valid,       // has a valid path leading to the exact requested position
        Unreachable, // DetourCrowd failed to path, or only reached a partial/nearest point
    };

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
        // Returns false if the target is not near any navmesh polygon (no move
        // request was issued at all) — the crowd equivalent of FindPathResult::Failed.
        bool SetAgentTarget(i32 agentId, const glm::vec3& target);
        [[nodiscard]] CrowdTargetState GetAgentTargetState(i32 agentId) const;
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
        [[nodiscard]] bool IsValidAgentId(i32 agentId) const
        {
            return m_Crowd && agentId >= 0 && agentId < m_Crowd->getAgentCount();
        }

        dtCrowd* m_Crowd = nullptr;
        Ref<NavMesh> m_NavMesh;
    };
} // namespace OloEngine
