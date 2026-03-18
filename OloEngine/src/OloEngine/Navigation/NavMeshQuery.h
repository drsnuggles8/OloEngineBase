#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Navigation/NavMesh.h"

#include <glm/glm.hpp>

#include <vector>

class dtNavMeshQuery;

namespace OloEngine
{
    class NavMeshQuery
    {
      public:
        NavMeshQuery() = default;
        explicit NavMeshQuery(const Ref<NavMesh>& navMesh, i32 queryBudget = 2048);
        ~NavMeshQuery();

        NavMeshQuery(const NavMeshQuery&) = delete;
        NavMeshQuery& operator=(const NavMeshQuery&) = delete;
        NavMeshQuery(NavMeshQuery&& other) noexcept;
        NavMeshQuery& operator=(NavMeshQuery&& other) noexcept;

        void Initialize(const Ref<NavMesh>& navMesh, i32 queryBudget = 2048);

        [[nodiscard]] bool FindPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& outPath) const;
        [[nodiscard]] bool FindNearestPoint(const glm::vec3& point, f32 searchRadius, glm::vec3& outNearest) const;
        [[nodiscard]] bool Raycast(const glm::vec3& start, const glm::vec3& end, glm::vec3& outHitPoint) const;
        [[nodiscard]] bool IsPointOnNavMesh(const glm::vec3& point, f32 tolerance = 0.5f) const;
        [[nodiscard]] bool IsValid() const
        {
            return m_Query != nullptr;
        }

      private:
        dtNavMeshQuery* m_Query = nullptr;
        Ref<NavMesh> m_NavMesh;
        i32 m_MaxPolys = 2048;
    };
} // namespace OloEngine
