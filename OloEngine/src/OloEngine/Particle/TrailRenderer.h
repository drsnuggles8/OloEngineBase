#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/ParticlePool.h"
#include "OloEngine/Particle/ParticleTrail.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class TrailRenderer
    {
    public:
        // Render trails for all alive particles as 3D triangle strips via Renderer3D
        // Uses DrawLine segments for Phase 2 initial implementation
        // Call between Renderer3D::BeginScene and EndScene
        static void RenderTrails3D(const ParticlePool& pool, const ParticleTrailData& trailData,
                                   const ModuleTrail& trailModule,
                                   const glm::vec3& cameraRight, const glm::vec3& cameraUp,
                                   const Ref<Texture2D>& texture);
    };
}
