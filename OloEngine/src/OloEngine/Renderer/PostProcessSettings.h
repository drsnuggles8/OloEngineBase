#pragma once

#include "OloEngine/Core/Base.h"

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
    };

    // GPU-side UBO layout for post-process parameters (std140, binding 7)
    struct PostProcessUBOData
    {
        i32 TonemapOperator = 1;   // Reinhard
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

        static constexpr u32 GetSize() { return sizeof(PostProcessUBOData); }
    };

    // GPU-side UBO layout for motion blur matrices (std140, binding 8)
    struct MotionBlurUBOData
    {
        glm::mat4 InverseViewProjection = glm::mat4(1.0f);
        glm::mat4 PrevViewProjection = glm::mat4(1.0f);

        static constexpr u32 GetSize() { return sizeof(MotionBlurUBOData); }
    };
} // namespace OloEngine
