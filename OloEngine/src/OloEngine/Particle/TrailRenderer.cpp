#include "OloEnginePCH.h"
#include "TrailRenderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Material.h"

namespace OloEngine
{
    void TrailRenderer::RenderTrails3D(const ParticlePool& pool, const ParticleTrailData& trailData,
                                       const ModuleTrail& trailModule,
                                       const glm::vec3& cameraRight, const glm::vec3& cameraUp,
                                       const Ref<Texture2D>& texture)
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

            // Draw trail as connected line segments
            for (u32 i = 0; i < trail.Count - 1; ++i)
            {
                const auto& a = trail.Get(i);
                const auto& b = trail.Get(i + 1);

                // Interpolate color based on position in trail
                f32 t = static_cast<f32>(i) / static_cast<f32>(trail.Count - 1);
                glm::vec4 color = glm::mix(trailModule.ColorStart, trailModule.ColorEnd, t);
                f32 width = glm::mix(trailModule.WidthStart, trailModule.WidthEnd, t);

                auto* packet = Renderer3D::DrawLine(a.Position, b.Position, glm::vec3(color), width * 2.0f);
                if (packet)
                {
                    Renderer3D::SubmitPacket(packet);
                }
            }
        }
    }
}
