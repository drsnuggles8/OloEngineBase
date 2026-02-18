#include "OloEnginePCH.h"
#include "TrailRenderer.h"
#include "OloEngine/Renderer/Renderer2D.h"

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

        u32 particleCount = pool.GetAliveCount();

        for (u32 p = 0; p < particleCount; ++p)
        {
            const auto& trail = trailData.GetTrail(p);
            if (trail.Count < 2)
            {
                continue;
            }

            // Render trail as connected quads (triangle strip emulated via quad pairs).
            // For each segment between adjacent trail points, expand perpendicular to
            // both the segment direction and the view direction to create a camera-facing ribbon.
            for (u32 i = 0; i < trail.Count - 1; ++i)
            {
                const auto& a = trail.Get(i);
                const auto& b = trail.Get(i + 1);

                // Interpolate color and width based on position in trail
                f32 tA = static_cast<f32>(i) / static_cast<f32>(trail.Count - 1);
                f32 tB = static_cast<f32>(i + 1) / static_cast<f32>(trail.Count - 1);

                glm::vec4 colorA = glm::mix(trailModule.ColorStart, trailModule.ColorEnd, tA);
                glm::vec4 colorB = glm::mix(trailModule.ColorStart, trailModule.ColorEnd, tB);
                glm::vec4 avgColor = (colorA + colorB) * 0.5f;

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
                glm::vec3 perp = glm::normalize(glm::cross(segDir, viewDir));

                // Build a quad: 4 vertices forming a camera-facing ribbon segment.
                // Use Renderer2D::DrawPolygon for the quad.
                f32 halfA = widthA * 0.5f;
                f32 halfB = widthB * 0.5f;

                // Quad vertices: counterclockwise winding
                std::vector<glm::vec3> vertices = {
                    posA - perp * halfA,  // bottom-left
                    posB - perp * halfB,  // bottom-right
                    posB + perp * halfB,  // top-right
                    posA + perp * halfA   // top-left
                };

                Renderer2D::DrawPolygon(vertices, avgColor, entityID);
            }
        }
    }
}
