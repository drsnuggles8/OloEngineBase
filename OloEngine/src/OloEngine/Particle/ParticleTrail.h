#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    // Ring buffer to store trail point history for one particle
    struct TrailPoint
    {
        glm::vec3 Position{ 0.0f };
        f32 Width = 1.0f;
        glm::vec4 Color{ 1.0f };
        f32 Age = 0.0f; // 0 = newest, 1 = oldest
    };

    // Fixed-size ring buffer for a single particle's trail points (O(1) insert and age)
    struct TrailRingBuffer
    {
        std::vector<TrailPoint> m_Points; // Fixed-size storage
        u32 m_Head = 0;                   // Index of the newest point
        u32 m_Count = 0;                  // Number of active points
        u32 m_Capacity = 0;

        void Resize(u32 maxPoints)
        {
            m_Capacity = maxPoints;
            m_Points.resize(maxPoints);
            m_Head = 0;
            m_Count = 0;
        }

        void Clear()
        {
            m_Head = 0;
            m_Count = 0;
        }

        // Push a new point to the front (newest). Returns index of the new point.
        void Push(const TrailPoint& point)
        {
            if (m_Capacity == 0)
            {
                return;
            }
            // Move head backwards (wrapping) to make room for new point
            m_Head = (m_Head == 0) ? (m_Capacity - 1) : (m_Head - 1);
            m_Points[m_Head] = point;
            if (m_Count < m_Capacity)
            {
                ++m_Count;
            }
        }

        // Get the i-th point (0 = newest, m_Count-1 = oldest)
        [[nodiscard]] const TrailPoint& Get(u32 i) const
        {
            OLO_CORE_ASSERT(m_Capacity > 0 && i < m_Count, "TrailRingBuffer::Get out of range");
            return m_Points[(m_Head + i) % m_Capacity];
        }

        [[nodiscard]] TrailPoint& Get(u32 i)
        {
            OLO_CORE_ASSERT(m_Capacity > 0 && i < m_Count, "TrailRingBuffer::Get out of range");
            return m_Points[(m_Head + i) % m_Capacity];
        }

        // Remove oldest points (trim m_Count)
        void TrimToCount(u32 newCount)
        {
            if (newCount < m_Count)
            {
                m_Count = newCount;
            }
        }
    };

    // Per-particle trail data stored as SOA alongside ParticlePool
    class ParticleTrailData
    {
      public:
        void Resize(u32 maxParticles, u32 maxTrailPoints);

        // Record a new trail point for particle at index
        void RecordPoint(u32 particleIndex, const glm::vec3& position, f32 width, const glm::vec4& color, f32 minVertexDistance = 0.1f);

        // Swap trail data when pool kills a particle (swap-to-back)
        void SwapParticles(u32 a, u32 b);

        // Clear trail for a particle when it's born
        void ClearTrail(u32 particleIndex);

        // Age all trail points by dt/lifetime
        void AgePoints(f32 dt, f32 trailLifetime);

        // Get trail ring buffer for a particle (iterate 0..m_Count-1 via Get())
        [[nodiscard]] const TrailRingBuffer& GetTrail(u32 particleIndex) const
        {
            return m_Trails[particleIndex];
        }
        [[nodiscard]] u32 GetMaxTrailPoints() const
        {
            return m_MaxTrailPoints;
        }

      private:
        std::vector<TrailRingBuffer> m_Trails;
        u32 m_MaxTrailPoints = 16;
    };

    struct ModuleTrail
    {
        bool Enabled = false;
        u32 MaxTrailPoints = 16;      // Points per particle trail
        f32 TrailLifetime = 0.5f;     // How long trail points last (seconds)
        f32 MinVertexDistance = 0.1f; // Min distance between recorded points
        f32 WidthStart = 1.0f;
        f32 WidthEnd = 0.0f;
        glm::vec4 ColorStart{ 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec4 ColorEnd{ 1.0f, 1.0f, 1.0f, 0.0f };
    };
} // namespace OloEngine
