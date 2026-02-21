#include "OloEnginePCH.h"
#include "ParticleRenderer.h"
#include "ParticleBatchRenderer.h"
#include "OloEngine/Renderer/Renderer2D.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    static const glm::vec4 s_DefaultUV = { 0.0f, 0.0f, 1.0f, 1.0f };

    u32 ParticleRenderer::ComputeFrame(const ParticlePool& pool, u32 index, const ModuleTextureSheetAnimation& sheet)
    {
        OLO_PROFILE_FUNCTION();

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
            f32 speed = glm::length(pool.m_Velocities[index]);
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
            const glm::vec3 pos = pool.m_Positions[i] + worldOffset;
            f32 size = pool.m_Sizes[i];
            f32 rotation = glm::radians(pool.m_Rotations[i]);
            const auto& color = pool.m_Colors[i];

            glm::mat4 transform = glm::translate(glm::mat4(1.0f), pos) * glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f }) * glm::scale(glm::mat4(1.0f), { size, size, 1.0f });

            if (useSpriteSheet)
            {
                u32 frame = ComputeFrame(pool, i, *spriteSheet);
                glm::vec2 uvMin, uvMax;
                spriteSheet->GetFrameUV(frame, uvMin, uvMax);

                Renderer2D::DrawQuad(transform, texture, uvMin, uvMax, color, entityID);
            }
            else
            {
                if (texture)
                    Renderer2D::DrawQuad(transform, texture, 1.0f, color, entityID);
                else
                    Renderer2D::DrawQuad(transform, color, entityID);
            }
        }
    }

    void ParticleRenderer::RenderParticlesBillboard(const ParticlePool& pool,
                                                    const Ref<Texture2D>& texture,
                                                    const glm::vec3& worldOffset, int entityID,
                                                    const std::vector<u32>* sortedIndices,
                                                    const ModuleTextureSheetAnimation* spriteSheet)
    {
        OLO_PROFILE_FUNCTION();

        u32 count = pool.GetAliveCount();
        bool useSpriteSheet = spriteSheet && spriteSheet->Enabled && spriteSheet->TotalFrames > 1 && texture;
        bool useSorted = sortedIndices && sortedIndices->size() == count;

        ParticleBatchRenderer::SetTexture(texture);

        for (u32 iter = 0; iter < count; ++iter)
        {
            u32 i = useSorted ? (*sortedIndices)[iter] : iter;
            const glm::vec3 pos = pool.m_Positions[i] + worldOffset;
            f32 size = pool.m_Sizes[i];
            f32 rotation = glm::radians(pool.m_Rotations[i]);
            const auto& color = pool.m_Colors[i];

            glm::vec4 uvRect = s_DefaultUV;
            if (useSpriteSheet)
            {
                u32 frame = ComputeFrame(pool, i, *spriteSheet);
                glm::vec2 uvMin, uvMax;
                spriteSheet->GetFrameUV(frame, uvMin, uvMax);
                uvRect = { uvMin.x, uvMin.y, uvMax.x, uvMax.y };
            }

            ParticleBatchRenderer::Submit(pos, size, rotation, color, uvRect, entityID);
        }
    }

    void ParticleRenderer::RenderParticlesStretched(const ParticlePool& pool,
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

        ParticleBatchRenderer::SetTexture(texture);

        for (u32 iter = 0; iter < count; ++iter)
        {
            u32 i = useSorted ? (*sortedIndices)[iter] : iter;
            const glm::vec3 pos = pool.m_Positions[i] + worldOffset;
            f32 size = pool.m_Sizes[i];
            const auto& color = pool.m_Colors[i];
            const glm::vec3& vel = pool.m_Velocities[i];

            glm::vec4 uvRect = s_DefaultUV;
            if (useSpriteSheet)
            {
                u32 frame = ComputeFrame(pool, i, *spriteSheet);
                glm::vec2 uvMin, uvMax;
                spriteSheet->GetFrameUV(frame, uvMin, uvMax);
                uvRect = { uvMin.x, uvMin.y, uvMax.x, uvMax.y };
            }

            ParticleBatchRenderer::SubmitStretched(pos, size, vel, lengthScale, color, uvRect, entityID);
        }
    }

    void ParticleRenderer::RenderParticlesMesh(const ParticlePool& pool,
                                               const Ref<Mesh>& mesh,
                                               const Ref<Texture2D>& texture,
                                               const glm::vec3& worldOffset,
                                               int entityID,
                                               const std::vector<u32>* sortedIndices)
    {
        OLO_PROFILE_FUNCTION();

        if (!mesh || !mesh->IsValid())
        {
            return;
        }

        u32 count = pool.GetAliveCount();
        if (count == 0)
        {
            return;
        }

        bool useSorted = sortedIndices && sortedIndices->size() == count;

        // Build instance data on stack (up to a reasonable limit)
        static thread_local std::vector<MeshParticleInstance> s_Instances;
        s_Instances.resize(count);

        for (u32 iter = 0; iter < count; ++iter)
        {
            u32 i = useSorted ? (*sortedIndices)[iter] : iter;
            const glm::vec3 pos = pool.m_Positions[i] + worldOffset;
            f32 size = pool.m_Sizes[i];
            f32 rotation = glm::radians(pool.m_Rotations[i]);
            const auto& color = pool.m_Colors[i];

            // Build model matrix: translate * rotate * scale (Y-axis rotation is the default for mesh particles)
            glm::mat4 model = glm::translate(glm::mat4(1.0f), pos) * glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 1.0f, 0.0f }) * glm::scale(glm::mat4(1.0f), glm::vec3(size));

            auto& inst = s_Instances[iter];
            inst.Model = model;
            inst.Color = color;
            inst.IDs = glm::ivec4(entityID, 0, 0, 0);
        }

        ParticleBatchRenderer::RenderMeshParticles(mesh, std::span{ s_Instances.data(), count }, texture);
    }
} // namespace OloEngine
