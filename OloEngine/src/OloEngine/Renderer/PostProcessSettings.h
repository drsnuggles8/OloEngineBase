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
        // vec4(Enabled, pad, pad, pad)
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
} // namespace OloEngine
