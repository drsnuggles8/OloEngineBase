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
            trail.clear();
            trail.reserve(maxTrailPoints);
        }
    }

    void ParticleTrailData::RecordPoint(u32 particleIndex, const glm::vec3& position, f32 width, const glm::vec4& color)
    {
        auto& trail = m_Trails[particleIndex];

        // Check minimum distance from last recorded point
        if (!trail.empty())
        {
            glm::vec3 diff = position - trail.front().Position;
            if (glm::dot(diff, diff) < 0.0001f) // Basically same position
            {
                return;
            }
        }

        // Insert at front (newest first)
        TrailPoint point;
        point.Position = position;
        point.Width = width;
        point.Color = color;
        point.Age = 0.0f;

        trail.insert(trail.begin(), point);

        // Trim to max trail points
        if (trail.size() > m_MaxTrailPoints)
        {
            trail.resize(m_MaxTrailPoints);
        }
    }

    void ParticleTrailData::SwapParticles(u32 a, u32 b)
    {
        std::swap(m_Trails[a], m_Trails[b]);
    }

    void ParticleTrailData::ClearTrail(u32 particleIndex)
    {
        if (particleIndex < m_Trails.size())
        {
            m_Trails[particleIndex].clear();
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
            // Age all points and remove expired ones
            for (auto it = trail.begin(); it != trail.end(); )
            {
                it->Age += ageDelta;
                if (it->Age >= 1.0f)
                {
                    it = trail.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
}
