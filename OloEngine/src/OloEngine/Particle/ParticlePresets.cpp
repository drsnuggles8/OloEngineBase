#include "OloEnginePCH.h"
#include "OloEngine/Particle/ParticlePresets.h"
#include "OloEngine/Particle/EmissionShape.h"

namespace OloEngine
{
    void ParticlePresets::ApplySnowfall(ParticleSystem& sys)
    {
        OLO_PROFILE_FUNCTION();

        // ---- Core ----
        sys.SetMaxParticles(50000);
        sys.Playing = true;
        sys.Looping = true;
        sys.Duration = 0.0f; // Continuous
        sys.PlaybackSpeed = 1.0f;
        sys.SimulationSpace = ParticleSpace::World;

        // ---- Rendering ----
        sys.BlendMode = ParticleBlendMode::Alpha;
        sys.RenderMode = ParticleRenderMode::Billboard;
        sys.DepthSortEnabled = false; // GPU doesn't sort; additive-like snowflakes look fine
        sys.UseGPU = true;

        // Soft particles for smooth blending near surfaces
        sys.SoftParticlesEnabled = true;
        sys.SoftParticleDistance = 0.5f;

        // ---- Emitter ----
        sys.Emitter.RateOverTime = 2000.0f; // Steady snowfall
        sys.Emitter.InitialSpeed = 0.3f;    // Very slow initial downward push
        sys.Emitter.SpeedVariance = 0.15f;
        sys.Emitter.LifetimeMin = 8.0f;
        sys.Emitter.LifetimeMax = 15.0f;
        sys.Emitter.InitialSize = 0.04f; // Small snowflakes
        sys.Emitter.SizeVariance = 0.02f;
        sys.Emitter.InitialRotation = 0.0f;
        sys.Emitter.RotationVariance = 180.0f;
        sys.Emitter.InitialColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.85f);

        // Emission shape: large box above camera
        EmitBox box;
        box.HalfExtents = glm::vec3(40.0f, 0.5f, 40.0f); // Wide, thin slab
        sys.Emitter.Shape = box;

        // ---- Gravity (gentle downward) ----
        sys.GravityModule.Enabled = true;
        sys.GravityModule.Gravity = glm::vec3(0.0f, -0.8f, 0.0f);

        // ---- Drag (floaty feel) ----
        sys.DragModule.Enabled = true;
        sys.DragModule.DragCoefficient = 0.3f;

        // ---- GPU Wind ----
        sys.WindInfluence = 1.0f;

        // ---- GPU Noise (chaotic fluttering) ----
        sys.GPUNoiseStrength = 0.8f;
        sys.GPUNoiseFrequency = 0.5f;

        // ---- GPU Ground Collision ----
        sys.GPUGroundCollision = true;
        sys.GPUGroundY = 0.0f;
        sys.GPUCollisionBounce = 0.0f;   // Snow doesn't bounce
        sys.GPUCollisionFriction = 1.0f; // Full stop on ground

        // ---- Color over lifetime (fade out) ----
        sys.ColorModule.Enabled = true;
        sys.ColorModule.ColorCurve = ParticleCurve4(
            glm::vec4(1.0f, 1.0f, 1.0f, 0.9f), // Start: bright, mostly opaque
            glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)  // End: fade to transparent
        );

        // ---- Size over lifetime (slight shrink) ----
        sys.SizeModule.Enabled = true;
        sys.SizeModule.SizeCurve = ParticleCurve(1.0f, 0.6f);

        // ---- Rotation (gentle spin) ----
        sys.RotationModule.Enabled = true;
        sys.RotationModule.AngularVelocity = 30.0f; // degrees/second
    }

    void ParticlePresets::ApplyBlizzard(ParticleSystem& sys)
    {
        OLO_PROFILE_FUNCTION();

        // Start from snowfall base
        ApplySnowfall(sys);

        // Override for blizzard intensity
        sys.SetMaxParticles(100000);
        sys.Emitter.RateOverTime = 8000.0f;
        sys.Emitter.InitialSpeed = 1.0f;
        sys.Emitter.SpeedVariance = 0.5f;
        sys.Emitter.LifetimeMin = 5.0f;
        sys.Emitter.LifetimeMax = 10.0f;
        sys.Emitter.InitialSize = 0.03f;
        sys.Emitter.SizeVariance = 0.015f;
        sys.Emitter.InitialColor = glm::vec4(0.95f, 0.95f, 1.0f, 0.7f);

        // Wider emission area
        EmitBox blizzardBox;
        blizzardBox.HalfExtents = glm::vec3(60.0f, 1.0f, 60.0f);
        sys.Emitter.Shape = blizzardBox;

        // Stronger gravity (heavier snow)
        sys.GravityModule.Gravity = glm::vec3(0.0f, -1.5f, 0.0f);

        // Higher wind influence
        sys.WindInfluence = 1.5f;

        // More chaotic turbulence
        sys.GPUNoiseStrength = 1.5f;
        sys.GPUNoiseFrequency = 0.8f;

        // More drag for dense air feel
        sys.DragModule.DragCoefficient = 0.5f;
    }
} // namespace OloEngine
