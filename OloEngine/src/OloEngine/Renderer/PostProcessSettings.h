#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // Tonemap operator constants (match PBRCommon.glsl defines)
    enum class TonemapOperator : i32
    {
        None = 0,
        Reinhard = 1,
        ACES = 2,
        Uncharted2 = 3
    };

    struct PostProcessSettings
    {
        // Tone mapping (always active)
        TonemapOperator Tonemap = TonemapOperator::Reinhard;
        f32 Exposure = 1.0f;
        f32 Gamma = 2.2f;

        // Bloom
        bool BloomEnabled = false;
        f32 BloomThreshold = 1.0f;
        f32 BloomIntensity = 0.5f;
        i32 BloomIterations = 5;

        // Vignette
        bool VignetteEnabled = false;
        f32 VignetteIntensity = 0.3f;
        f32 VignetteSmoothness = 0.5f;

        // Chromatic Aberration
        bool ChromaticAberrationEnabled = false;
        f32 ChromaticAberrationIntensity = 0.005f;

        // FXAA
        bool FXAAEnabled = false;

        // Depth of Field
        bool DOFEnabled = false;
        f32 DOFFocusDistance = 10.0f;
        f32 DOFFocusRange = 5.0f;
        f32 DOFBokehRadius = 3.0f;

        // Motion Blur
        bool MotionBlurEnabled = false;
        f32 MotionBlurStrength = 0.5f;
        i32 MotionBlurSamples = 8;

        // Color Grading
        bool ColorGradingEnabled = false;

        // SSAO
        bool SSAOEnabled = false;
        f32 SSAORadius = 0.5f;
        f32 SSAOBias = 0.025f;
        f32 SSAOIntensity = 1.0f;
        i32 SSAOSamples = 32;
        bool SSAODebugView = false;
    };

    // GPU-side UBO layout for post-process parameters (std140, binding 7)
    struct PostProcessUBOData
    {
        i32 TonemapOperator = 1; // Reinhard
        f32 Exposure = 1.0f;
        f32 Gamma = 2.2f;
        f32 BloomThreshold = 1.0f;

        f32 BloomIntensity = 0.5f;
        f32 VignetteIntensity = 0.3f;
        f32 VignetteSmoothness = 0.5f;
        f32 ChromaticAberrationIntensity = 0.005f;

        f32 DOFFocusDistance = 10.0f;
        f32 DOFFocusRange = 5.0f;
        f32 DOFBokehRadius = 3.0f;
        f32 MotionBlurStrength = 0.5f;

        i32 MotionBlurSamples = 8;
        f32 InverseScreenWidth = 0.0f;
        f32 InverseScreenHeight = 0.0f;
        f32 _padding0 = 0.0f;

        // Per-pass volatile data (re-uploaded before each effect)
        f32 TexelSizeX = 0.0f;
        f32 TexelSizeY = 0.0f;
        f32 CameraNear = 0.1f;
        f32 CameraFar = 1000.0f;

        static constexpr u32 GetSize()
        {
            return sizeof(PostProcessUBOData);
        }
    };

    // GPU-side UBO layout for motion blur matrices (std140, binding 8)
    struct MotionBlurUBOData
    {
        glm::mat4 InverseViewProjection = glm::mat4(1.0f);
        glm::mat4 PrevViewProjection = glm::mat4(1.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(MotionBlurUBOData);
        }
    };

    // GPU-side UBO layout for SSAO parameters (std140, binding 9)
    struct SSAOUBOData
    {
        f32 Radius = 0.5f;
        f32 Bias = 0.025f;
        f32 Intensity = 1.0f;
        i32 Samples = 32;

        i32 ScreenWidth = 0;
        i32 ScreenHeight = 0;
        i32 DebugView = 0;
        f32 _pad1 = 0.0f;

        glm::mat4 Projection = glm::mat4(1.0f);
        glm::mat4 InverseProjection = glm::mat4(1.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SSAOUBOData);
        }
    };
    // Snow rendering settings (scene-level, separate from PostProcess)
    struct SnowSettings
    {
        bool Enabled = false;

        // Coverage parameters
        f32 HeightStart = -5.0f; // World-Y where snow starts appearing
        f32 HeightFull = 5.0f;   // World-Y where snow reaches full coverage
        f32 SlopeStart = 0.7f;   // Normal.y threshold where snow starts reducing
        f32 SlopeFull = 0.3f;    // Normal.y threshold where snow is gone

        // Snow material
        glm::vec3 Albedo = glm::vec3(0.92f, 0.93f, 0.98f);
        f32 Roughness = 0.65f;

        // SSS
        glm::vec3 SSSColor = glm::vec3(0.4f, 0.6f, 0.9f);
        f32 SSSIntensity = 0.6f;

        // Sparkle
        f32 SparkleIntensity = 0.8f;
        f32 SparkleDensity = 80.0f;
        f32 SparkleScale = 1.0f;

        // Normal perturbation
        f32 NormalPerturbStrength = 0.25f;

        // Wind drift (snow coverage responds to wind direction)
        f32 WindDriftFactor = 0.0f; // 0 = no wind effect, 1 = full wind-driven accumulation bias

        // SSS blur pass
        bool SSSBlurEnabled = false;
        f32 SSSBlurRadius = 2.0f;
        f32 SSSBlurFalloff = 1.0f;
    };

    // GPU-side UBO layout for snow parameters (std140, binding 13)
    struct SnowUBOData
    {
        // vec4(HeightStart, HeightFull, SlopeStart, SlopeFull)
        glm::vec4 CoverageParams = glm::vec4(-5.0f, 5.0f, 0.7f, 0.3f);
        // vec4(Albedo.rgb, Roughness)
        glm::vec4 AlbedoAndRoughness = glm::vec4(0.92f, 0.93f, 0.98f, 0.65f);
        // vec4(SSSColor.rgb, SSSIntensity)
        glm::vec4 SSSColorAndIntensity = glm::vec4(0.4f, 0.6f, 0.9f, 0.6f);
        // vec4(SparkleIntensity, SparkleDensity, SparkleScale, NormalPerturbStrength)
        glm::vec4 SparkleParams = glm::vec4(0.8f, 80.0f, 1.0f, 0.25f);
        // vec4(Enabled, WindDriftFactor, pad, pad)
        glm::vec4 Flags = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SnowUBOData);
        }
    };

    // GPU-side UBO layout for SSS blur parameters (std140, binding 14)
    struct SSSUBOData
    {
        // vec4(BlurRadius, BlurFalloff, ScreenWidth, ScreenHeight)
        glm::vec4 BlurParams = glm::vec4(2.0f, 1.0f, 0.0f, 0.0f);
        // vec4(Enabled, pad, pad, pad)
        glm::vec4 Flags = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SSSUBOData);
        }
    };

    // Snow accumulation & deformation settings (scene-level)
    struct SnowAccumulationSettings
    {
        bool Enabled = false;

        // Accumulation
        f32 AccumulationRate = 0.02f; // Meters of snow per second when snowing
        f32 MaxDepth = 0.5f;          // Maximum snow depth (meters)
        f32 MeltRate = 0.005f;        // Meters of snow lost per second (temperature-driven)
        f32 RestorationRate = 0.01f;  // How fast deformed snow fills back in (m/s)

        // Displacement
        f32 DisplacementScale = 1.0f; // Multiplier for vertex displacement from snow depth

        // Clipmap
        u32 ClipmapResolution = 2048; // Texels per axis for the snow depth texture
        f32 ClipmapExtent = 128.0f;   // World-space side length of innermost clipmap ring (meters)
        u32 NumClipmapRings = 3;      // Number of clipmap LOD rings

        // Physics
        f32 SnowDensity = 0.3f; // Density factor for compaction (0 = powder, 1 = packed ice)
    };

    // GPU-side UBO layout for snow accumulation (std140, binding 16)
    // Contains clipmap matrices + accumulation parameters
    struct SnowAccumulationUBOData
    {
        static constexpr u32 MAX_CLIPMAP_RINGS = 3;

        // Orthographic view-projection for each clipmap ring (top-down)
        glm::mat4 ClipmapViewProj[MAX_CLIPMAP_RINGS] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
        // vec4(centerX, centerZ, extent, invExtent) per ring
        glm::vec4 ClipmapCenterAndExtent[MAX_CLIPMAP_RINGS] = { glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f) };
        // vec4(AccumulationRate, MaxDepth, MeltRate, RestorationRate)
        glm::vec4 AccumulationParams = glm::vec4(0.02f, 0.5f, 0.005f, 0.01f);
        // vec4(DisplacementScale, SnowDensity, Enabled, NumRings)
        glm::vec4 DisplacementParams = glm::vec4(1.0f, 0.3f, 0.0f, 3.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SnowAccumulationUBOData);
        }
    };

    // Snow ejecta particle settings (scene-level)
    struct SnowEjectaSettings
    {
        bool Enabled = false;                                  // Whether snow deformers emit ejecta particles
        u32 ParticlesPerDeform = 8;                            // Particles emitted per deformer stamp per frame
        f32 EjectaSpeed = 2.5f;                                // Base outward velocity (m/s)
        f32 SpeedVariance = 0.8f;                              // Random speed variation factor (0–1)
        f32 UpwardBias = 0.6f;                                 // Fraction of velocity directed upward vs outward
        f32 LifetimeMin = 0.4f;                                // Minimum particle lifetime (seconds)
        f32 LifetimeMax = 1.2f;                                // Maximum particle lifetime (seconds)
        f32 InitialSize = 0.04f;                               // Starting particle size (meters)
        f32 SizeVariance = 0.02f;                              // Random size variation (meters)
        f32 GravityScale = 0.3f;                               // Gravity multiplier (snow falls slowly)
        f32 DragCoefficient = 2.0f;                            // Air drag for quick deceleration
        glm::vec4 Color = glm::vec4(0.95f, 0.97f, 1.0f, 0.7f); // RGBA snow puff color
        f32 VelocityThreshold = 0.1f;                          // Min deformer speed to emit (m/s)
        u32 MaxParticles = 8192;                               // Max GPU particles in the ejecta pool
    };

    // Wind simulation settings (scene-level, separate from PostProcess)
    struct WindSettings
    {
        bool Enabled = false;

        // Base wind
        glm::vec3 Direction = glm::vec3(1.0f, 0.0f, 0.0f); // Normalized wind direction
        f32 Speed = 5.0f;                                  // Wind speed in m/s

        // Gust modulation
        f32 GustStrength = 0.3f;  // 0–1 amplitude of gust modulation
        f32 GustFrequency = 0.5f; // Hz — how rapidly gusts oscillate

        // Turbulence (noise-driven spatial variation)
        f32 TurbulenceIntensity = 0.5f; // Strength of turbulent fluctuations
        f32 TurbulenceScale = 0.1f;     // Spatial frequency of turbulence noise

        // 3D wind field grid
        f32 GridWorldSize = 200.0f; // Side length of cube centered on camera (meters)
        u32 GridResolution = 128;   // Voxels per axis (128³)
    };

    // GPU-side UBO layout for wind parameters (std140, binding 15)
    struct WindUBOData
    {
        // vec4(Direction.xyz, Speed)
        glm::vec4 DirectionAndSpeed = glm::vec4(1.0f, 0.0f, 0.0f, 5.0f);
        // vec4(GustStrength, GustFrequency, TurbulenceIntensity, TurbulenceScale)
        glm::vec4 GustAndTurbulence = glm::vec4(0.3f, 0.5f, 0.5f, 0.1f);
        // vec4(GridMin.xyz, GridWorldSize)
        glm::vec4 GridMinAndSize = glm::vec4(0.0f);
        // vec4(Time, Enabled, GridResolution, pad)
        glm::vec4 TimeAndFlags = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(WindUBOData);
        }
    };
} // namespace OloEngine
