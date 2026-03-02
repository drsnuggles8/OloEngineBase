#include "OloEnginePCH.h"
#include "OloEngine/Particle/ParticlePresets.h"
#include "OloEngine/Particle/EmissionShape.h"

#include <limits>

namespace OloEngine
{
    void ParticlePresets::ApplySnowfall(ParticleSystem& sys)
    {
        OLO_PROFILE_FUNCTION();

        // ---- Core ----
        sys.SetMaxParticles(50000);
        sys.Playing = true;
        sys.Looping = true;
        sys.Duration = std::numeric_limits<f32>::max(); // Continuous
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

    // =========================================================================
    // Smoke Presets
    // =========================================================================

    void ParticlePresets::ApplySmoke(ParticleSystem& sys)
    {
        OLO_PROFILE_FUNCTION();

        // ---- Core ----
        sys.SetMaxParticles(5000);
        sys.Playing = true;
        sys.Looping = true;
        sys.Duration = std::numeric_limits<f32>::max();
        sys.PlaybackSpeed = 1.0f;
        sys.SimulationSpace = ParticleSpace::World;

        // ---- Rendering ----
        sys.BlendMode = ParticleBlendMode::Alpha;
        sys.RenderMode = ParticleRenderMode::Billboard;
        sys.DepthSortEnabled = true;
        sys.UseGPU = false;

        // Soft particles for smooth depth blending
        sys.SoftParticlesEnabled = true;
        sys.SoftParticleDistance = 1.0f;

        // ---- Emitter ----
        sys.Emitter.RateOverTime = 75.0f;
        sys.Emitter.InitialSpeed = 1.2f;
        sys.Emitter.SpeedVariance = 0.4f;
        sys.Emitter.LifetimeMin = 3.0f;
        sys.Emitter.LifetimeMax = 5.0f;
        sys.Emitter.InitialSize = 0.5f;
        sys.Emitter.SizeVariance = 0.15f;
        sys.Emitter.InitialRotation = 0.0f;
        sys.Emitter.RotationVariance = 180.0f;
        sys.Emitter.InitialColor = glm::vec4(0.25f, 0.25f, 0.25f, 0.6f);

        // Emission shape: upward cone
        EmitCone cone;
        cone.Angle = 20.0f;
        cone.Radius = 0.3f;
        sys.Emitter.Shape = cone;

        // ---- Gravity (slight upward buoyancy) ----
        sys.GravityModule.Enabled = true;
        sys.GravityModule.Gravity = glm::vec3(0.0f, 0.6f, 0.0f);

        // ---- Drag (air resistance) ----
        sys.DragModule.Enabled = true;
        sys.DragModule.DragCoefficient = 0.7f;

        // ---- Wind ----
        sys.WindInfluence = 0.4f;

        // ---- Noise (organic turbulence) ----
        sys.NoiseModule.Enabled = true;
        sys.NoiseModule.Strength = 0.6f;
        sys.NoiseModule.Frequency = 0.3f;

        // ---- Color over lifetime: dark gray → transparent ----
        sys.ColorModule.Enabled = true;
        sys.ColorModule.ColorCurve = ParticleCurve4(
            glm::vec4(0.3f, 0.3f, 0.3f, 0.6f),
            glm::vec4(0.4f, 0.4f, 0.4f, 0.0f));

        // ---- Size over lifetime: expand 2.5× as smoke dissipates ----
        sys.SizeModule.Enabled = true;
        sys.SizeModule.SizeCurve = ParticleCurve(1.0f, 2.5f);

        // ---- Rotation (slow spin for organic look) ----
        sys.RotationModule.Enabled = true;
        sys.RotationModule.AngularVelocity = 15.0f;

        // ---- Disable ground collision (smoke rises) ----
        sys.GPUGroundCollision = false;
    }

    void ParticlePresets::ApplyThickSmoke(ParticleSystem& sys)
    {
        OLO_PROFILE_FUNCTION();

        // Start from standard smoke base
        ApplySmoke(sys);

        // GPU path for high particle counts
        sys.SetMaxParticles(20000);
        sys.UseGPU = true;
        sys.DepthSortEnabled = false;

        // Higher emission rate for denser coverage
        sys.Emitter.RateOverTime = 175.0f;
        sys.Emitter.InitialSpeed = 0.8f;
        sys.Emitter.SpeedVariance = 0.3f;
        sys.Emitter.InitialSize = 0.8f;
        sys.Emitter.SizeVariance = 0.25f;

        // Higher initial opacity, slower fade
        sys.Emitter.InitialColor = glm::vec4(0.2f, 0.2f, 0.2f, 0.8f);
        sys.ColorModule.ColorCurve = ParticleCurve4(
            glm::vec4(0.25f, 0.25f, 0.25f, 0.8f),
            glm::vec4(0.35f, 0.35f, 0.35f, 0.0f));

        // Heavier, slower-rising smoke
        sys.GravityModule.Gravity = glm::vec3(0.0f, 0.3f, 0.0f);
        sys.DragModule.DragCoefficient = 0.9f;

        // Larger expansion
        sys.SizeModule.SizeCurve = ParticleCurve(1.0f, 3.0f);

        // Wider cone for broader plume
        EmitCone thickCone;
        thickCone.Angle = 30.0f;
        thickCone.Radius = 0.5f;
        sys.Emitter.Shape = thickCone;

        // Stronger noise for chaotic billowing (GPU path)
        sys.GPUNoiseStrength = 1.2f;
        sys.GPUNoiseFrequency = 0.4f;

        // Wind influence
        sys.WindInfluence = 0.3f;
    }

    void ParticlePresets::ApplyLightSmoke(ParticleSystem& sys)
    {
        OLO_PROFILE_FUNCTION();

        // Start from standard smoke base
        ApplySmoke(sys);

        // Lower spawn rate, longer lifetime
        sys.Emitter.RateOverTime = 30.0f;
        sys.Emitter.LifetimeMin = 5.0f;
        sys.Emitter.LifetimeMax = 8.0f;

        // Smaller, lighter particles
        sys.Emitter.InitialSize = 0.35f;
        sys.Emitter.SizeVariance = 0.1f;
        sys.Emitter.InitialColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.3f);

        // Faster rise, less drag
        sys.Emitter.InitialSpeed = 1.8f;
        sys.Emitter.SpeedVariance = 0.5f;
        sys.GravityModule.Gravity = glm::vec3(0.0f, 0.9f, 0.0f);
        sys.DragModule.DragCoefficient = 0.5f;

        // More wind responsiveness
        sys.WindInfluence = 0.7f;

        // Lower opacity, ethereal appearance
        sys.BlendMode = ParticleBlendMode::Additive;
        sys.ColorModule.ColorCurve = ParticleCurve4(
            glm::vec4(0.6f, 0.6f, 0.6f, 0.3f),
            glm::vec4(0.7f, 0.7f, 0.7f, 0.0f));

        // Moderate expansion
        sys.SizeModule.SizeCurve = ParticleCurve(1.0f, 2.0f);

        // Subtle, delicate noise
        sys.NoiseModule.Strength = 0.3f;
        sys.NoiseModule.Frequency = 0.6f;

        // Narrower cone for wispy column
        EmitCone lightCone;
        lightCone.Angle = 15.0f;
        lightCone.Radius = 0.2f;
        sys.Emitter.Shape = lightCone;
    }
} // namespace OloEngine
