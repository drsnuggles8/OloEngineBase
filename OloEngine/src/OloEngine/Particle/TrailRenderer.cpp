#include "OloEnginePCH.h"
#include "TrailRenderer.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"

namespace OloEngine
{
    void TrailRenderer::RenderTrails(const ParticlePool& pool, const ParticleTrailData& trailData,
                                     const ModuleTrail& trailModule,
                                     const glm::vec3& cameraPosition,
                                     const Ref<Texture2D>& texture,
                                     const glm::vec3& worldOffset,
                                     int entityID)
    {
        OLO_PROFILE_FUNCTION();

        ParticleBatchRenderer::SetTrailTexture(texture);

        u32 particleCount = pool.GetAliveCount();

        for (u32 p = 0; p < particleCount; ++p)
        {
            const auto& trail = trailData.GetTrail(p);
            if (trail.Count < 2)
            {
                continue;
            }

            // Render trail as connected quads via SubmitTrailQuad.
            // Each segment between adjacent trail points expands perpendicular to
            // both the segment direction and the view direction for a camera-facing ribbon.
            for (u32 i = 0; i < trail.Count - 1; ++i)
            {
                const auto& a = trail.Get(i);
                const auto& b = trail.Get(i + 1);

                // Interpolate color and width based on position in trail
                f32 tA = static_cast<f32>(i) / static_cast<f32>(trail.Count - 1);
                f32 tB = static_cast<f32>(i + 1) / static_cast<f32>(trail.Count - 1);

                glm::vec4 colorA = glm::mix(trailModule.ColorStart, trailModule.ColorEnd, tA);
                glm::vec4 colorB = glm::mix(trailModule.ColorStart, trailModule.ColorEnd, tB);

                f32 widthA = glm::mix(trailModule.WidthStart, trailModule.WidthEnd, tA);
                f32 widthB = glm::mix(trailModule.WidthStart, trailModule.WidthEnd, tB);

                glm::vec3 posA = a.Position + worldOffset;
                glm::vec3 posB = b.Position + worldOffset;
                glm::vec3 segDir = posB - posA;
                f32 segLen = glm::length(segDir);
                if (segLen < 0.0001f)
                {
                    continue;
                }
                segDir /= segLen;

                // View direction from camera to segment midpoint
                glm::vec3 midpoint = (posA + posB) * 0.5f;
                glm::vec3 viewDir = glm::normalize(cameraPosition - midpoint);

                // Perpendicular to both segment and view — this is the ribbon expansion direction
                glm::vec3 rawPerp = glm::cross(segDir, viewDir);
                f32 perpLen = glm::length(rawPerp);
                glm::vec3 perp;
                if (perpLen > 0.0001f)
                {
                    perp = rawPerp / perpLen;
                }
                else
                {
                    // Segment is collinear with view direction — use fallback perpendicular
                    glm::vec3 fallback = (std::abs(segDir.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
                    perp = glm::normalize(glm::cross(segDir, fallback));
                }

                f32 halfA = widthA * 0.5f;
                f32 halfB = widthB * 0.5f;

                // Quad vertices (counterclockwise): BL, BR, TR, TL
                glm::vec3 positions[4] = {
                    posA - perp * halfA, // bottom-left
                    posB - perp * halfB, // bottom-right
                    posB + perp * halfB, // top-right
                    posA + perp * halfA  // top-left
                };
                glm::vec4 colors[4] = { colorA, colorB, colorB, colorA };

                // UV mapping: U along trail length (0 at head, 1 at tail), V across width
                glm::vec2 texCoords[4] = {
                    { tA, 0.0f }, // BL
                    { tB, 0.0f }, // BR
                    { tB, 1.0f }, // TR
                    { tA, 1.0f }  // TL
                };

                ParticleBatchRenderer::SubmitTrailQuad(positions, colors, texCoords, entityID);
            }
        }
    }
} // namespace OloEngine
