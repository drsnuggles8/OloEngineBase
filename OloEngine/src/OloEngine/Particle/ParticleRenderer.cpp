#include "OloEnginePCH.h"
#include "ParticleRenderer.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Material.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    void ParticleRenderer::RenderParticles2D(const ParticlePool& pool, const Ref<Texture2D>& texture, const glm::vec3& worldOffset, int entityID)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            const glm::vec3 pos = pool.Positions[i] + worldOffset;
            f32 size = pool.Sizes[i];
            f32 rotation = glm::radians(pool.Rotations[i]);
            const auto& color = pool.Colors[i];

            if (texture)
            {
                Renderer2D::DrawRotatedQuad(
                    pos,
                    { size, size },
                    rotation,
                    texture,
                    1.0f,
                    color
                );
            }
            else
            {
                Renderer2D::DrawRotatedQuad(
                    pos,
                    { size, size },
                    rotation,
                    color
                );
            }
        }
    }

    void ParticleRenderer::RenderParticlesBillboard(const ParticlePool& pool, const glm::vec3& cameraRight, const glm::vec3& cameraUp, const Ref<Texture2D>& texture, const glm::vec3& worldOffset, int entityID)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        for (u32 i = 0; i < count; ++i)
        {
            const glm::vec3 pos = pool.Positions[i] + worldOffset;
            f32 size = pool.Sizes[i];
            f32 rotation = glm::radians(pool.Rotations[i]);
            const auto& color = pool.Colors[i];

            // Build billboard transform
            f32 halfSize = size * 0.5f;
            glm::vec3 right = cameraRight * halfSize;
            glm::vec3 up = cameraUp * halfSize;

            // Apply rotation around the billboard normal (camera forward = cross(right, up))
            if (std::abs(rotation) > 0.001f)
            {
                f32 cosR = std::cos(rotation);
                f32 sinR = std::sin(rotation);
                glm::vec3 newRight = right * cosR + up * sinR;
                glm::vec3 newUp = -right * sinR + up * cosR;
                right = newRight;
                up = newUp;
            }

            glm::vec3 forward = glm::normalize(glm::cross(cameraRight, cameraUp));

            glm::mat4 transform(1.0f);
            transform[0] = glm::vec4(right * 2.0f, 0.0f);
            transform[1] = glm::vec4(up * 2.0f, 0.0f);
            transform[2] = glm::vec4(forward, 0.0f);
            transform[3] = glm::vec4(pos, 1.0f);

            if (texture)
            {
                Renderer2D::DrawQuad(transform, texture, 1.0f, color, entityID);
            }
            else
            {
                Renderer2D::DrawQuad(transform, color, entityID);
            }
        }
    }

    void ParticleRenderer::RenderParticles3D(const ParticlePool& pool, const glm::vec3& cameraRight, const glm::vec3& cameraUp, const Ref<Texture2D>& texture, const glm::vec3& worldOffset, int entityID)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        if (count == 0)
        {
            return;
        }

        glm::vec3 forward = glm::normalize(glm::cross(cameraRight, cameraUp));

        for (u32 i = 0; i < count; ++i)
        {
            const glm::vec3 pos = pool.Positions[i] + worldOffset;
            f32 size = pool.Sizes[i];
            f32 rotation = glm::radians(pool.Rotations[i]);
            const auto& color = pool.Colors[i];

            f32 halfSize = size * 0.5f;
            glm::vec3 right = cameraRight * halfSize;
            glm::vec3 up = cameraUp * halfSize;

            // Apply rotation around the billboard normal
            if (std::abs(rotation) > 0.001f)
            {
                f32 cosR = std::cos(rotation);
                f32 sinR = std::sin(rotation);
                glm::vec3 newRight = right * cosR + up * sinR;
                glm::vec3 newUp = -right * sinR + up * cosR;
                right = newRight;
                up = newUp;
            }

            // Build billboard transform matrix (paper-thin along forward so it's a flat quad)
            glm::mat4 transform(1.0f);
            transform[0] = glm::vec4(right * 2.0f, 0.0f);
            transform[1] = glm::vec4(up * 2.0f, 0.0f);
            transform[2] = glm::vec4(forward * 0.001f, 0.0f);
            transform[3] = glm::vec4(pos, 1.0f);

            if (texture)
            {
                auto* packet = Renderer3D::DrawQuad(transform, texture);
                if (packet)
                {
                    Renderer3D::SubmitPacket(packet);
                }
            }
            else
            {
                // Use an emissive flat material colored by the particle
                Material mat;
                mat.SetType(MaterialType::PBR);
                mat.SetBaseColorFactor(color);
                mat.SetMetallicFactor(0.0f);
                mat.SetRoughnessFactor(1.0f);
                mat.SetEmissiveFactor(glm::vec4(glm::vec3(color) * 2.0f, color.a));

                auto* packet = Renderer3D::DrawCube(transform, mat);
                if (packet)
                {
                    Renderer3D::SubmitPacket(packet);
                }
            }
        }
    }
}
