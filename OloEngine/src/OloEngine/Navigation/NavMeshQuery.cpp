#include "OloEnginePCH.h"
#include "OloEngine/Navigation/NavMeshQuery.h"
#include "OloEngine/Core/Log.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

namespace OloEngine
{
    static constexpr f32 DEFAULT_EXTENT[3] = { 2.0f, 4.0f, 2.0f };

    NavMeshQuery::NavMeshQuery(const Ref<NavMesh>& navMesh, i32 queryBudget)
    {
        Initialize(navMesh, queryBudget);
    }

    NavMeshQuery::~NavMeshQuery()
    {
        if (m_Query)
        {
            dtFreeNavMeshQuery(m_Query);
            m_Query = nullptr;
        }
    }

    NavMeshQuery::NavMeshQuery(NavMeshQuery&& other) noexcept
        : m_Query(other.m_Query), m_NavMesh(std::move(other.m_NavMesh)), m_MaxPolys(other.m_MaxPolys)
    {
        other.m_Query = nullptr;
    }

    NavMeshQuery& NavMeshQuery::operator=(NavMeshQuery&& other) noexcept
    {
        if (this != &other)
        {
            if (m_Query)
                dtFreeNavMeshQuery(m_Query);
            m_Query = other.m_Query;
            m_NavMesh = std::move(other.m_NavMesh);
            m_MaxPolys = other.m_MaxPolys;
            other.m_Query = nullptr;
        }
        return *this;
    }

    void NavMeshQuery::Initialize(const Ref<NavMesh>& navMesh, i32 queryBudget)
    {
        if (m_Query)
        {
            dtFreeNavMeshQuery(m_Query);
            m_Query = nullptr;
        }

        m_MaxPolys = std::max(queryBudget, 64);
        m_NavMesh = navMesh;
        if (!m_NavMesh || !m_NavMesh->GetDetourNavMesh())
            return;

        m_Query = dtAllocNavMeshQuery();
        if (!m_Query)
        {
            OLO_CORE_ERROR("NavMeshQuery: Could not allocate query");
            return;
        }

        dtStatus status = m_Query->init(m_NavMesh->GetDetourNavMesh(), m_MaxPolys);
        if (dtStatusFailed(status))
        {
            OLO_CORE_ERROR("NavMeshQuery: Could not initialize query");
            dtFreeNavMeshQuery(m_Query);
            m_Query = nullptr;
        }
    }

    bool NavMeshQuery::FindPath(const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& outPath) const
    {
        OLO_PROFILE_FUNCTION();

        outPath.clear();

        if (!m_Query)
            return false;

        const f32 startPos[3] = { start.x, start.y, start.z };
        const f32 endPos[3] = { end.x, end.y, end.z };

        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(0);

        dtPolyRef startRef = 0;
        dtPolyRef endRef = 0;
        f32 nearestStart[3]{};
        f32 nearestEnd[3]{};

        dtStatus status = m_Query->findNearestPoly(startPos, DEFAULT_EXTENT, &filter, &startRef, nearestStart);
        if (dtStatusFailed(status) || !startRef)
            return false;

        status = m_Query->findNearestPoly(endPos, DEFAULT_EXTENT, &filter, &endRef, nearestEnd);
        if (dtStatusFailed(status) || !endRef)
            return false;

        std::vector<dtPolyRef> polys(static_cast<size_t>(m_MaxPolys));
        i32 npolys = 0;
        status = m_Query->findPath(startRef, endRef, nearestStart, nearestEnd, &filter, polys.data(), &npolys, m_MaxPolys);
        if (dtStatusFailed(status) || npolys == 0)
            return false;

        // Find straight path
        std::vector<f32> straightPath(static_cast<size_t>(m_MaxPolys) * 3);
        std::vector<u8> straightPathFlags(static_cast<size_t>(m_MaxPolys));
        std::vector<dtPolyRef> straightPathPolys(static_cast<size_t>(m_MaxPolys));
        i32 nstraightPath = 0;

        m_Query->findStraightPath(nearestStart, nearestEnd, polys.data(), npolys,
                                  straightPath.data(), straightPathFlags.data(), straightPathPolys.data(),
                                  &nstraightPath, m_MaxPolys);

        outPath.reserve(static_cast<size_t>(nstraightPath));
        for (i32 i = 0; i < nstraightPath; ++i)
        {
            outPath.emplace_back(straightPath[i * 3], straightPath[i * 3 + 1], straightPath[i * 3 + 2]);
        }

        return !outPath.empty();
    }

    bool NavMeshQuery::FindNearestPoint(const glm::vec3& point, f32 searchRadius, glm::vec3& outNearest) const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Query)
            return false;

        const f32 pos[3] = { point.x, point.y, point.z };
        const f32 extent[3] = { searchRadius, searchRadius, searchRadius };

        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(0);

        dtPolyRef ref = 0;
        f32 nearest[3]{};

        dtStatus status = m_Query->findNearestPoly(pos, extent, &filter, &ref, nearest);
        if (dtStatusFailed(status) || !ref)
            return false;

        outNearest = { nearest[0], nearest[1], nearest[2] };
        return true;
    }

    bool NavMeshQuery::Raycast(const glm::vec3& start, const glm::vec3& end, glm::vec3& outHitPoint) const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Query)
            return false;

        const f32 startPos[3] = { start.x, start.y, start.z };
        const f32 endPos[3] = { end.x, end.y, end.z };

        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(0);

        dtPolyRef startRef = 0;
        f32 nearest[3]{};
        dtStatus status = m_Query->findNearestPoly(startPos, DEFAULT_EXTENT, &filter, &startRef, nearest);

        if (dtStatusFailed(status) || !startRef)
            return false;

        f32 t = 0.0f;
        f32 hitNormal[3]{};
        std::vector<dtPolyRef> path(static_cast<size_t>(m_MaxPolys));
        i32 pathCount = 0;

        status = m_Query->raycast(startRef, nearest, endPos, &filter, &t, hitNormal, path.data(), &pathCount, m_MaxPolys);
        if (dtStatusFailed(status))
            return false;

        if (t < 1.0f)
        {
            // Hit something
            outHitPoint.x = nearest[0] + (endPos[0] - nearest[0]) * t;
            outHitPoint.y = nearest[1] + (endPos[1] - nearest[1]) * t;
            outHitPoint.z = nearest[2] + (endPos[2] - nearest[2]) * t;
            return true;
        }

        // No hit — reached end point
        outHitPoint = end;
        return false;
    }

    bool NavMeshQuery::IsPointOnNavMesh(const glm::vec3& point, f32 tolerance) const
    {
        OLO_PROFILE_FUNCTION();

        glm::vec3 nearest;
        return FindNearestPoint(point, tolerance, nearest);
    }
} // namespace OloEngine
