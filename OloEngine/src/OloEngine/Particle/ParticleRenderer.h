#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Particle/ParticlePool.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class ParticleRenderer
    {
    public:
        // Render all alive particles as 2D camera-facing quads via Renderer2D
        // Call between Renderer2D::BeginScene and EndScene
        // worldOffset is added to each particle position (used for Local simulation space)
        static void RenderParticles2D(const ParticlePool& pool, const Ref<Texture2D>& texture, const glm::vec3& worldOffset = glm::vec3(0.0f), int entityID = -1);

        // Render as billboarded quads facing the camera
        // Call between Renderer2D::BeginScene and EndScene
        static void RenderParticlesBillboard(const ParticlePool& pool, const glm::vec3& cameraRight, const glm::vec3& cameraUp, const Ref<Texture2D>& texture, const glm::vec3& worldOffset = glm::vec3(0.0f), int entityID = -1);

        // Render particles as 3D billboarded quads via Renderer3D
        // Call between Renderer3D::BeginScene and EndScene
        static void RenderParticles3D(const ParticlePool& pool, const glm::vec3& cameraRight, const glm::vec3& cameraUp, const Ref<Texture2D>& texture, const glm::vec3& worldOffset = glm::vec3(0.0f), int entityID = -1);
    };
}
