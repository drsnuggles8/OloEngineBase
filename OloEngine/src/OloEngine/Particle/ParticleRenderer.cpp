#include "OloEnginePCH.h"
#include "ParticleRenderer.h"
#include "OloEngine/Renderer/Renderer2D.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    u32 ParticleRenderer::ComputeFrame(const ParticlePool& pool, u32 index, const ModuleTextureSheetAnimation& sheet)
    {
        if (sheet.TotalFrames <= 1)
        {
            return 0;
        }

        if (sheet.Mode == TextureSheetAnimMode::OverLifetime)
        {
            f32 age = pool.GetAge(index); // 0..1
            return static_cast<u32>(age * static_cast<f32>(sheet.TotalFrames - 1) + 0.5f);
        }
        else // BySpeed
        {
            f32 speed = glm::length(pool.Velocities[index]);
            f32 t = std::min(speed / std::max(sheet.SpeedRange, 0.001f), 1.0f);
            return static_cast<u32>(t * static_cast<f32>(sheet.TotalFrames - 1) + 0.5f);
        }
    }

    void ParticleRenderer::RenderParticles2D(const ParticlePool& pool, const Ref<Texture2D>& texture,
                                             const glm::vec3& worldOffset, int entityID,
                                             const std::vector<u32>* sortedIndices,
                                             const ModuleTextureSheetAnimation* spriteSheet)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        bool useSpriteSheet = spriteSheet && spriteSheet->Enabled && spriteSheet->TotalFrames > 1 && texture;
        bool useSorted = sortedIndices && sortedIndices->size() == count;

        for (u32 iter = 0; iter < count; ++iter)
        {
            u32 i = useSorted ? (*sortedIndices)[iter] : iter;
            const glm::vec3 pos = pool.Positions[i] + worldOffset;
            f32 size = pool.Sizes[i];
            f32 rotation = glm::radians(pool.Rotations[i]);
            const auto& color = pool.Colors[i];

            if (useSpriteSheet)
            {
                u32 frame = ComputeFrame(pool, i, *spriteSheet);
                glm::vec2 uvMin, uvMax;
                spriteSheet->GetFrameUV(frame, uvMin, uvMax);

                // Build transform manually for UV control
                glm::mat4 transform = glm::translate(glm::mat4(1.0f), pos)
                    * glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
                    * glm::scale(glm::mat4(1.0f), { size, size, 1.0f });
                Renderer2D::DrawQuad(transform, texture, uvMin, uvMax, color, entityID);
            }
            else if (texture)
            {
                Renderer2D::DrawRotatedQuad(pos, { size, size }, rotation, texture, 1.0f, color);
            }
            else
            {
                Renderer2D::DrawRotatedQuad(pos, { size, size }, rotation, color);
            }
        }
    }

    void ParticleRenderer::RenderParticlesBillboard(const ParticlePool& pool,
                                                    const glm::vec3& cameraRight, const glm::vec3& cameraUp,
                                                    const Ref<Texture2D>& texture,
                                                    const glm::vec3& worldOffset, int entityID,
                                                    const std::vector<u32>* sortedIndices,
                                                    const ModuleTextureSheetAnimation* spriteSheet)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        bool useSpriteSheet = spriteSheet && spriteSheet->Enabled && spriteSheet->TotalFrames > 1 && texture;
        bool useSorted = sortedIndices && sortedIndices->size() == count;

        for (u32 iter = 0; iter < count; ++iter)
        {
            u32 i = useSorted ? (*sortedIndices)[iter] : iter;
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

            glm::mat4 transform(1.0f);
            transform[0] = glm::vec4(right * 2.0f, 0.0f);
            transform[1] = glm::vec4(up * 2.0f, 0.0f);
            transform[2] = glm::vec4(glm::normalize(glm::cross(cameraRight, cameraUp)), 0.0f);
            transform[3] = glm::vec4(pos, 1.0f);

            if (useSpriteSheet)
            {
                u32 frame = ComputeFrame(pool, i, *spriteSheet);
                glm::vec2 uvMin, uvMax;
                spriteSheet->GetFrameUV(frame, uvMin, uvMax);
                Renderer2D::DrawQuad(transform, texture, uvMin, uvMax, color, entityID);
            }
            else if (texture)
            {
                Renderer2D::DrawQuad(transform, texture, 1.0f, color, entityID);
            }
            else
            {
                Renderer2D::DrawQuad(transform, color, entityID);
            }
        }
    }

    void ParticleRenderer::RenderParticlesStretched(const ParticlePool& pool,
                                                    const glm::vec3& cameraRight, const glm::vec3& cameraUp,
                                                    const Ref<Texture2D>& texture,
                                                    f32 lengthScale,
                                                    const glm::vec3& worldOffset, int entityID,
                                                    const std::vector<u32>* sortedIndices,
                                                    const ModuleTextureSheetAnimation* spriteSheet)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        bool useSpriteSheet = spriteSheet && spriteSheet->Enabled && spriteSheet->TotalFrames > 1 && texture;
        bool useSorted = sortedIndices && sortedIndices->size() == count;
        glm::vec3 forward = glm::normalize(glm::cross(cameraRight, cameraUp));

        for (u32 iter = 0; iter < count; ++iter)
        {
            u32 i = useSorted ? (*sortedIndices)[iter] : iter;
            const glm::vec3 pos = pool.Positions[i] + worldOffset;
            f32 size = pool.Sizes[i];
            const auto& color = pool.Colors[i];
            const glm::vec3& vel = pool.Velocities[i];
            f32 speed = glm::length(vel);

            // Stretch along velocity direction
            glm::vec3 stretchDir = (speed > 0.001f) ? (vel / speed) : cameraUp;

            // Project stretch direction onto the billboard plane (perpendicular to forward)
            glm::vec3 projStretch = stretchDir - glm::dot(stretchDir, forward) * forward;
            f32 projLen = glm::length(projStretch);
            if (projLen < 0.001f)
            {
                projStretch = cameraUp;
            }
            else
            {
                projStretch /= projLen;
            }

            f32 halfWidth = size * 0.5f;
            f32 halfLength = halfWidth + speed * lengthScale * 0.5f;

            glm::vec3 up = projStretch * halfLength;
            glm::vec3 right = glm::normalize(glm::cross(projStretch, forward)) * halfWidth;

            glm::mat4 transform(1.0f);
            transform[0] = glm::vec4(right * 2.0f, 0.0f);
            transform[1] = glm::vec4(up * 2.0f, 0.0f);
            transform[2] = glm::vec4(forward, 0.0f);
            transform[3] = glm::vec4(pos, 1.0f);

            if (useSpriteSheet)
            {
                u32 frame = ComputeFrame(pool, i, *spriteSheet);
                glm::vec2 uvMin, uvMax;
                spriteSheet->GetFrameUV(frame, uvMin, uvMax);
                Renderer2D::DrawQuad(transform, texture, uvMin, uvMax, color, entityID);
            }
            else if (texture)
            {
                Renderer2D::DrawQuad(transform, texture, 1.0f, color, entityID);
            }
            else
            {
                Renderer2D::DrawQuad(transform, color, entityID);
            }
        }
    }
}
