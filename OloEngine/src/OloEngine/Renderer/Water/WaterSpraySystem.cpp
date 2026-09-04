#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Water/WaterSpraySystem.h"

#include "OloEngine/Particle/GPUParticleSystem.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Renderer/Ocean/OceanFFTField.h"
#include "OloEngine/Renderer/Texture.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace OloEngine
{
    WaterSpraySystem::WaterSprayData WaterSpraySystem::s_Data;

    void WaterSpraySystem::Init(u32 maxParticles)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Initialized)
        {
            OLO_CORE_WARN("WaterSpraySystem::Init called when already initialized");
            return;
        }

        s_Data.m_System = CreateScope<GPUParticleSystem>(std::max(maxParticles, 256u));
        if (!s_Data.m_System->IsInitialized())
        {
            OLO_CORE_ERROR("WaterSpraySystem::Init — GPU particle system unavailable; "
                           "crest spray is disabled for this session");
            s_Data.m_System.reset();
            return;
        }

        // A procedural soft droplet, generated the same way PrecipitationSystem
        // builds its snowflake and raindrop rather than loaded from a file —
        // the particle textures in this engine are all generated, and an asset
        // here would be one more thing a headless run has to find on disk.
        //
        // Round rather than the raindrop's teardrop: a raindrop is elongated by
        // falling, and spray is thrown off in every direction at once.
        {
            constexpr u32 texSize = 32;
            constexpr f32 centre = static_cast<f32>(texSize) * 0.5f;
            constexpr f32 invRadius = 1.0f / centre;

            std::vector<u32> pixels(static_cast<sizet>(texSize) * texSize);
            for (u32 y = 0; y < texSize; ++y)
            {
                for (u32 x = 0; x < texSize; ++x)
                {
                    const f32 dx = (static_cast<f32>(x) + 0.5f - centre) * invRadius;
                    const f32 dy = (static_cast<f32>(y) + 0.5f - centre) * invRadius;
                    const f32 dist = std::sqrt(dx * dx + dy * dy);
                    // Squared falloff, so the droplet has a bright core and a
                    // soft edge instead of a hard disc that aliases at range.
                    f32 alpha = std::clamp(1.0f - dist, 0.0f, 1.0f);
                    alpha *= alpha;
                    const auto a = static_cast<u8>(alpha * 255.0f);
                    pixels[static_cast<sizet>(y) * texSize + x] =
                        (static_cast<u32>(a) << 24u) | 0x00FFFFFFu;
                }
            }

            TextureSpecification spec;
            spec.Width = texSize;
            spec.Height = texSize;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            s_Data.m_DropletTexture = Texture2D::Create(spec);
            if (s_Data.m_DropletTexture)
            {
                s_Data.m_DropletTexture->SetData(pixels.data(),
                                                 static_cast<u32>(pixels.size() * sizeof(u32)));
            }
        }

        s_Data.m_AccumulatedTime = 0.0f;
        s_Data.m_DrainTimeRemaining = 0.0f;
        s_Data.m_LastEmitCount = 0;
        s_Data.m_Initialized = true;
        OLO_CORE_INFO("WaterSpraySystem initialized ({} particle pool)",
                      s_Data.m_System->GetMaxParticles());
    }

    void WaterSpraySystem::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        s_Data.m_System.reset();
        s_Data.m_DropletTexture.Reset();
        s_Data.m_Field.Reset();
        s_Data.m_Settings = WaterSpray::WaterSpraySettings{};
        s_Data.m_LastEmitCount = 0;
        s_Data.m_DrainTimeRemaining = 0.0f;
        s_Data.m_Initialized = false;
    }

    bool WaterSpraySystem::IsInitialized()
    {
        return s_Data.m_Initialized;
    }

    void WaterSpraySystem::SetSource(const WaterSpray::WaterSpraySettings& settings,
                                     const Ref<Ocean::OceanFFTField>& field, f32 foamThreshold)
    {
        s_Data.m_Settings = settings;
        // A field with no cascades has no folding Jacobian to test, which is
        // the same reason foam advection refuses a Gerstner sea. Forcing the
        // flag here rather than checking it at every use keeps
        // GetSettings()/GetLastEmitCount() honest for diagnostics.
        s_Data.m_Settings.m_Enabled = settings.m_Enabled && field && field->GetCascadeCount() > 0u;
        s_Data.m_Field = field;
        s_Data.m_FoamThreshold = foamThreshold;
    }

    void WaterSpraySystem::Update(const glm::vec3& cameraPos, Timestep dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized)
        {
            s_Data.m_LastEmitCount = 0;
            return;
        }

        const f32 deltaTime = std::clamp(static_cast<f32>(dt), 0.0f, 0.25f);
        s_Data.m_AccumulatedTime += deltaTime;
        s_Data.m_LastEmitCount = 0;

        const bool active = s_Data.m_Settings.m_Enabled && s_Data.m_Field;
        if (active)
        {
            // Every particle in flight has to be able to finish its arc after
            // the sea calms, so remember how long that takes — measured off the
            // LONGEST life Emit() can hand out, not the authored one. Emit
            // jitters up to kLifetimeJitterMax, so a shorter window stops
            // simulating while the last frame's longest-lived droplets are
            // still in the air, and the early return below strands them frozen.
            s_Data.m_DrainTimeRemaining =
                std::max(s_Data.m_DrainTimeRemaining,
                         std::clamp(s_Data.m_Settings.m_Lifetime, 0.05f, 20.0f) *
                             WaterSpray::kLifetimeJitterMax);

            const Ocean::OceanFFTField& field = *s_Data.m_Field;
            // THE seam: the emitter asks "what is the surface doing here", and
            // this is the only place that answer is wired to a real ocean.
            // WaterSprayTest substitutes it, which is what makes the "calm sea
            // emits nothing" criterion a CI test instead of a screenshot.
            auto sampleCrest = [&field](glm::vec2 worldXZ) -> WaterSpray::CrestSample
            {
                const Ocean::OceanFFTField::SurfaceSample s = field.SampleCascades(worldXZ);
                return { s.Foam, s.Height, s.Horizontal };
            };

            std::vector<GPUParticle> particles =
                WaterSpray::Emit(s_Data.m_Settings, s_Data.m_FoamThreshold,
                                 glm::vec2(cameraPos.x, cameraPos.z), s_Data.m_AccumulatedTime,
                                 deltaTime, sampleCrest);

            s_Data.m_LastEmitCount = static_cast<u32>(particles.size());
            if (!particles.empty())
                s_Data.m_System->EmitParticles(std::span<const GPUParticle>(particles));

            // Throttled, because "spray emitted nothing" and "spray emitted and
            // is invisible" look identical on screen and are diagnosed
            // completely differently. One line a second says which.
            if (s_Data.m_LogThrottle++ % 60u == 0u)
            {
                OLO_CORE_TRACE("WaterSpraySystem — emitted {} this frame (threshold {:.3f})",
                               s_Data.m_LastEmitCount,
                               WaterSpray::EffectiveThreshold(s_Data.m_Settings.m_Threshold,
                                                              s_Data.m_FoamThreshold));
            }
        }
        else
        {
            // The guard comes BEFORE the decrement so the frame that ENDS the
            // drain still runs the chain below. Decrementing first returns on
            // that frame, which leaves the previous Compact's instance count
            // published in the indirect buffer — and the last droplets, whose
            // lifetimes expire on exactly that frame, are then drawn frozen
            // forever because nothing ever simulates or re-compacts them again.
            if (s_Data.m_DrainTimeRemaining <= 0.0f)
                return; // nothing airborne and nothing to emit — skip the whole chain
            s_Data.m_DrainTimeRemaining = std::max(s_Data.m_DrainTimeRemaining - deltaTime, 0.0f);
        }

        // Ballistic motion with wind drag, which is what §2.3 asks for and
        // what Particle_Simulate.comp already does. Nothing here integrates
        // anything itself.
        GPUSimParams simParams{};
        simParams.DeltaTime = deltaTime;
        simParams.MaxParticles = s_Data.m_System->GetMaxParticles();
        simParams.Gravity = glm::vec3(0.0f, -9.81f, 0.0f);
        simParams.EnableGravity = 1;
        simParams.EnableDrag = 1;
        // Spray is a fine droplet with a large area-to-mass ratio, so it slows
        // far faster than a raindrop does — which is what makes a plume of it
        // hang in the air over the crest instead of arcing like a thrown stone.
        simParams.DragCoefficient = 1.4f;
        simParams.EnableWind = 1;
        simParams.WindInfluence = 1.0f;
        // NO GROUND COLLISION, deliberately, and it is worth saying why so it
        // is not re-added: GPUSimParams' ground is a PLANE, and the thing these
        // droplets fall onto is a wave field. Setting the plane at the mean
        // water level kills every droplet spawned in a TROUGH — where the
        // surface is below that level and a crest can still be folding — which
        // measured as 256 emitted and 0 alive. Bounding the fall with the
        // LIFETIME instead costs nothing and cannot delete a live droplet: at
        // the default launch speed the arc is back where it started in about
        // 0.6 s, so a 0.9 s life ends it a little over a metre lower, well
        // inside the wave amplitude it was thrown from.

        s_Data.m_System->Simulate(simParams);
        s_Data.m_System->Compact();
        s_Data.m_System->PrepareIndirectDraw();
    }

    void WaterSpraySystem::Render()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.m_Initialized || !s_Data.m_System)
            return;

        // Flush whatever the CPU batcher has queued before switching to an
        // indirect GPU draw, exactly as PrecipitationSystem::Render does — the
        // two share the batcher and an unflushed batch would draw with this
        // pool's state.
        ParticleBatchRenderer::Flush();
        ParticleBatchRenderer::RenderGPUBillboards(*s_Data.m_System, s_Data.m_DropletTexture);
    }

    void WaterSpraySystem::Reset()
    {
        OLO_PROFILE_FUNCTION();

        // Drop the GPU pool, not just the CPU state. Without this the indirect
        // draw buffer keeps the previous scene's alive count: Update() returns
        // early on a reset (nothing to emit, drain expired) BEFORE
        // PrepareIndirectDraw, so Render() goes on drawing the old scene's
        // droplets, frozen at their old world positions. That is the
        // cross-frame-history defect docs/agent-rules/runtime-scene-switching.md
        // is about, and it is invisible in any test that loads one scene.
        // Shutdown+Init per pool is what PrecipitationSystem::Reset does.
        if (s_Data.m_Initialized && s_Data.m_System)
        {
            const u32 maxParticles = s_Data.m_System->GetMaxParticles();
            s_Data.m_System->Shutdown();
            s_Data.m_System->Init(maxParticles);
            if (!s_Data.m_System->IsInitialized())
            {
                // Tear the whole service down rather than leaving m_Initialized
                // true over a dead pool: Update() gates on that flag and would
                // go on driving a shut-down system, IsInitialized() would lie to
                // callers, and a later Init() would early-out as "already
                // initialized" so the session could never recover.
                OLO_CORE_ERROR("WaterSpraySystem::Reset — particle pool re-init failed; "
                               "crest spray is disabled for this session");
                s_Data.m_System.reset();
                s_Data.m_Initialized = false;
            }
        }

        s_Data.m_Settings = WaterSpray::WaterSpraySettings{};
        s_Data.m_Field.Reset();
        s_Data.m_FoamThreshold = 0.10f;
        s_Data.m_AccumulatedTime = 0.0f;
        s_Data.m_DrainTimeRemaining = 0.0f;
        s_Data.m_LastEmitCount = 0;
    }

    u32 WaterSpraySystem::GetLastEmitCount()
    {
        return s_Data.m_LastEmitCount;
    }

    u32 WaterSpraySystem::GetAliveParticleCount()
    {
        if (!s_Data.m_Initialized || !s_Data.m_System)
            return 0u;
        return s_Data.m_System->GetAliveCount();
    }

    const WaterSpray::WaterSpraySettings& WaterSpraySystem::GetSettings()
    {
        return s_Data.m_Settings;
    }
} // namespace OloEngine
