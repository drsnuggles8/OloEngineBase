#include "OloEnginePCH.h"
#include "ParticleTrail.h"

namespace OloEngine
{
    void ParticleTrailData::Resize(u32 maxParticles, u32 maxTrailPoints)
    {
        m_MaxTrailPoints = maxTrailPoints;
        m_Trails.resize(maxParticles);
        for (auto& trail : m_Trails)
        {
            trail.Resize(maxTrailPoints);
        }
    }

    void ParticleTrailData::RecordPoint(u32 particleIndex, const glm::vec3& position, f32 width, const glm::vec4& color, f32 minVertexDistance)
    {
        auto& trail = m_Trails[particleIndex];

        // Check minimum distance from last recorded point
        if (trail.Count > 0)
        {
            const glm::vec3& lastPos = trail.Get(0).Position;
            glm::vec3 diff = position - lastPos;
            f32 minDistSq = minVertexDistance * minVertexDistance;
            if (glm::dot(diff, diff) < minDistSq)
            {
                return;
            }
        }

        TrailPoint point;
        point.Position = position;
        point.Width = width;
        point.Color = color;
        point.Age = 0.0f;

        trail.Push(point);
    }

    void ParticleTrailData::SwapParticles(u32 a, u32 b)
    {
        std::swap(m_Trails[a], m_Trails[b]);
    }

    void ParticleTrailData::ClearTrail(u32 particleIndex)
    {
        if (particleIndex < m_Trails.size())
        {
            m_Trails[particleIndex].Clear();
        }
    }

    void ParticleTrailData::AgePoints(f32 dt, f32 trailLifetime)
    {
        if (trailLifetime <= 0.0f)
        {
            return;
        }

        f32 ageDelta = dt / trailLifetime;

        for (auto& trail : m_Trails)
        {
            // Age all points from newest to oldest, trim expired from the end
            u32 newCount = trail.Count;
            for (u32 i = 0; i < newCount; ++i)
            {
                TrailPoint& pt = trail.Get(i);
                pt.Age += ageDelta;
                if (pt.Age >= 1.0f)
                {
                    // This point and all older points are expired
                    newCount = i;
                    break;
                }
            }
            trail.TrimToCount(newCount);
        }
    }
} // namespace OloEngine
