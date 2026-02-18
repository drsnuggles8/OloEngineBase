#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/ParticlePool.h"
#include "OloEngine/Particle/ParticleModules.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class ParticleRenderer
    {
    public:
        // Render all alive particles as 2D camera-facing quads via Renderer2D
        // Call between Renderer2D::BeginScene and EndScene
        // worldOffset is added to each particle position (used for Local simulation space)
        // If sortedIndices is non-null, particles are rendered in that order (for depth sorting)
        // If spriteSheet is non-null and enabled, per-particle UV sub-rects are used
        static void RenderParticles2D(const ParticlePool& pool,
                                      const Ref<Texture2D>& texture,
                                      const glm::vec3& worldOffset = glm::vec3(0.0f),
                                      int entityID = -1,
                                      const std::vector<u32>* sortedIndices = nullptr,
                                      const ModuleTextureSheetAnimation* spriteSheet = nullptr);

        // Render as billboarded quads facing the camera
        // Call between Renderer2D::BeginScene and EndScene
        static void RenderParticlesBillboard(const ParticlePool& pool,
                                             const glm::vec3& cameraRight, const glm::vec3& cameraUp,
                                             const Ref<Texture2D>& texture,
                                             const glm::vec3& worldOffset = glm::vec3(0.0f),
                                             int entityID = -1,
                                             const std::vector<u32>* sortedIndices = nullptr,
                                             const ModuleTextureSheetAnimation* spriteSheet = nullptr);

        // Render particles as stretched billboards (velocity-aligned)
        // Call between Renderer2D::BeginScene and EndScene
        static void RenderParticlesStretched(const ParticlePool& pool,
                                             const glm::vec3& cameraRight, const glm::vec3& cameraUp,
                                             const Ref<Texture2D>& texture,
                                             f32 lengthScale = 1.0f,
                                             const glm::vec3& worldOffset = glm::vec3(0.0f),
                                             int entityID = -1,
                                             const std::vector<u32>* sortedIndices = nullptr,
                                             const ModuleTextureSheetAnimation* spriteSheet = nullptr);

    private:
        // Compute sprite sheet frame for a particle based on age or speed
        static u32 ComputeFrame(const ParticlePool& pool, u32 index, const ModuleTextureSheetAnimation& sheet);
    };
}
