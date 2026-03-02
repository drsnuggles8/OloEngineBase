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

        // --- Advanced Simulation (modify with care) ---
        f32 WindInfluence = 0.5f;     // How strongly wind affects snow puffs (0–1)
        f32 NoiseStrength = 0.3f;     // Turbulence intensity for organic motion
        f32 NoiseFrequency = 2.0f;    // Spatial frequency of turbulence noise
        f32 GroundY = 0.0f;           // Ground plane Y for collision
        f32 CollisionBounce = 0.0f;   // Bounce factor on ground hit (0 = no bounce)
        f32 CollisionFriction = 1.0f; // Friction on ground contact (1 = full stop)
    };

    // Fog mode constants (match FogCommon.glsl defines)
    enum class FogMode : i32
    {
        Linear = 0,
        Exponential = 1,
        ExponentialSquared = 2
    };

    // Fog & atmospheric scattering settings (scene-level)
    struct FogSettings
    {
        bool Enabled = false;
        FogMode Mode = FogMode::ExponentialSquared;

        // Base fog
        glm::vec3 Color = glm::vec3(0.5f, 0.6f, 0.7f);
        f32 Density = 0.02f; // For Exponential / Exponential² modes
        f32 Start = 10.0f;   // For Linear mode (near plane)
        f32 End = 300.0f;    // For Linear mode (far plane)

        // Height fog
        f32 HeightFalloff = 0.1f; // Density decay rate above reference height
        f32 HeightOffset = 0.0f;  // Sea-level reference for height fog

        // Opacity
        f32 MaxOpacity = 1.0f; // Clamp fog factor (0 = no fog, 1 = fully opaque at distance)

        // Atmospheric scattering
        bool EnableScattering = false;
        f32 RayleighStrength = 1.0f;
        f32 MieStrength = 0.005f;
        f32 MieDirectionality = 0.76f;                            // Henyey-Greenstein g (0 = isotropic, ~1 = full forward scatter)
        glm::vec3 RayleighColor = glm::vec3(0.27f, 0.51f, 0.83f); // Sky blue
        f32 SunIntensity = 22.0f;

        // Volumetric ray-marching
        bool EnableVolumetric = false;
        i32 VolumetricSamples = 32;        // Ray-march steps (16-64)
        f32 AbsorptionCoefficient = 0.02f; // Beer-Lambert absorption (separate from scatter density)

        // Noise / turbulence (modulates fog density spatially)
        bool EnableNoise = false;
        f32 NoiseScale = 0.01f;    // 3D noise spatial frequency (world-space)
        f32 NoiseSpeed = 0.1f;     // Animation speed (units/sec)
        f32 NoiseIntensity = 0.3f; // Noise modulation strength (0 = uniform, 1 = full variation)

        // Volumetric light shafts (god rays through shadow map)
        bool EnableLightShafts = false;
        f32 LightShaftIntensity = 1.0f; // In-scattering boost for lit volume samples
    };

    // GPU-side UBO layout for fog parameters (std140, binding 17)
    struct FogUBOData
    {
        // vec4(Color.rgb, Density)
        glm::vec4 ColorAndDensity = glm::vec4(0.5f, 0.6f, 0.7f, 0.02f);
        // vec4(Start, End, HeightFalloff, HeightOffset)
        glm::vec4 DistanceParams = glm::vec4(10.0f, 300.0f, 0.1f, 0.0f);
        // vec4(RayleighStrength, MieStrength, MieG, SunIntensity)
        glm::vec4 ScatterParams = glm::vec4(1.0f, 0.005f, 0.76f, 22.0f);
        // vec4(RayleighColor.rgb, MaxOpacity)
        glm::vec4 RayleighColorAndMaxOpacity = glm::vec4(0.27f, 0.51f, 0.83f, 1.0f);
        // vec4(SunDirection.xyz, fogFrameIndex) — w = temporal jitter frame counter
        glm::vec4 SunDirection = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
        // vec4(Enabled, Mode, ScatteringEnabled, VolumetricEnabled)
        glm::vec4 Flags = glm::vec4(0.0f);
        // vec4(NoiseScale, NoiseSpeed, NoiseIntensity, Time)
        glm::vec4 NoiseParams = glm::vec4(0.01f, 0.1f, 0.3f, 0.0f);
        // vec4(VolumetricSamples, AbsorptionCoeff, LightShaftIntensity, LightShaftsEnabled)
        glm::vec4 VolumetricParams = glm::vec4(32.0f, 0.02f, 1.0f, 0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(FogUBOData);
        }
    };

    // Precipitation type constants
    enum class PrecipitationType : i32
    {
        Snow = 0,
        Rain = 1,
        Hail = 2,
        Sleet = 3
    };

    // Precipitation intensity presets (map to parameter sets)
    enum class PrecipitationIntensity : i32
    {
        None = 0,
        Light = 1,
        Moderate = 2,
        Heavy = 3,
        Blizzard = 4
    };

    // Scene-global precipitation settings (camera-relative, multi-layer)
    struct PrecipitationSettings
    {
        bool Enabled = false;
        PrecipitationType Type = PrecipitationType::Snow;
        f32 Intensity = 0.5f;            // 0–1 continuous
        f32 TransitionSpeed = 1.0f;      // Interpolation speed toward target

        // Emission
        u32 BaseEmissionRate = 4000;      // Particles/sec at intensity=1
        u32 MaxParticlesNearField = 100000;
        u32 MaxParticlesFarField = 200000;

        // Near-field layer (detailed, close to camera)
        glm::vec3 NearFieldExtent = glm::vec3(15.0f, 20.0f, 15.0f);
        f32 NearFieldParticleSize = 0.04f;
        f32 NearFieldSizeVariance = 0.02f;
        f32 NearFieldSpeedMin = 0.8f;
        f32 NearFieldSpeedMax = 2.0f;
        f32 NearFieldLifetime = 10.0f;

        // Far-field layer (atmospheric, larger volume)
        glm::vec3 FarFieldExtent = glm::vec3(60.0f, 30.0f, 60.0f);
        f32 FarFieldParticleSize = 0.025f;
        f32 FarFieldSpeedMin = 0.5f;
        f32 FarFieldSpeedMax = 1.5f;
        f32 FarFieldLifetime = 15.0f;
        f32 FarFieldAlphaMultiplier = 0.5f;

        // Physics
        f32 GravityScale = 0.8f;
        f32 WindInfluence = 1.0f;
        f32 DragCoefficient = 0.3f;
        f32 TurbulenceStrength = 0.8f;
        f32 TurbulenceFrequency = 0.5f;

        // Ground interaction
        bool GroundCollisionEnabled = true;
        f32 GroundY = 0.0f;
        bool FeedAccumulation = true;
        f32 AccumulationFeedRate = 0.0001f;

        // Screen-space effects
        bool ScreenStreaksEnabled = true;
        f32 ScreenStreakIntensity = 0.3f;
        f32 ScreenStreakLength = 0.15f;
        bool LensImpactsEnabled = true;
        f32 LensImpactRate = 3.0f;       // Impacts/sec at intensity=1
        f32 LensImpactLifetime = 1.5f;
        f32 LensImpactSize = 0.03f;

        // LOD / performance budget
        f32 LODNearDistance = 30.0f;
        f32 LODFarDistance = 120.0f;
        f32 FrameBudgetMs = 1.0f;

        // Visual
        glm::vec4 ParticleColor = glm::vec4(0.95f, 0.97f, 1.0f, 0.85f);
        f32 ColorVariance = 0.05f;
        f32 RotationSpeed = 30.0f;       // deg/s
    };

    // GPU-side UBO layout for precipitation parameters (std140, binding 18)
    struct PrecipitationUBOData
    {
        // vec4(Intensity, WindInfluence, StreakIntensity, StreakLength)
        glm::vec4 IntensityAndScreenFX = glm::vec4(0.5f, 1.0f, 0.3f, 0.15f);
        // vec4(LensImpactRate, LensImpactLifetime, LensImpactSize, Enabled)
        glm::vec4 LensParams = glm::vec4(3.0f, 1.5f, 0.03f, 0.0f);
        // vec4(ScreenWindDirX, ScreenWindDirY, Time, StreaksEnabled)
        glm::vec4 ScreenWindAndTime = glm::vec4(0.0f);
        // vec4(ParticleColor rgba)
        glm::vec4 ParticleColor = glm::vec4(0.95f, 0.97f, 1.0f, 0.85f);

        static constexpr u32 GetSize()
        {
            return sizeof(PrecipitationUBOData);
        }
    };

    // GPU-side UBO layout for precipitation screen-space effects (std140, binding 19)
    struct PrecipitationScreenUBOData
    {
        // vec4(DirX, DirY, Intensity, Length)
        glm::vec4 StreakParams = glm::vec4(0.0f);
        // LensImpacts[16] — each element has 2 vec4 (PositionAndSize, TimeParams)
        struct LensImpactGPU
        {
            glm::vec4 PositionAndSize{0.0f};
            glm::vec4 TimeParams{0.0f};
        } LensImpacts[16]{};

        static constexpr u32 GetSize()
        {
            return sizeof(PrecipitationScreenUBOData);
        }
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
