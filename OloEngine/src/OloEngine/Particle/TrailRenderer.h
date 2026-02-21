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
        // Render trails as camera-facing quad strips via ParticleBatchRenderer
        // Generates quads expanded perpendicular to the camera and trail direction
        // for proper width tapering and visual quality.
        // Call between ParticleBatchRenderer::BeginBatch and EndBatch
        static void RenderTrails(const ParticlePool& pool, const ParticleTrailData& trailData,
                                 const ModuleTrail& trailModule,
                                 const glm::vec3& cameraPosition,
                                 const Ref<Texture2D>& texture,
                                 const glm::vec3& worldOffset = glm::vec3(0.0f),
                                 int entityID = -1);
    };
} // namespace OloEngine
