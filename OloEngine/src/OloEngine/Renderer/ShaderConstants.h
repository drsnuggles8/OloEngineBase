#pragma once

#include "ShaderBindingLayout.h"

namespace OloEngine
{
    // @brief Shared constants for shaders to replace magic numbers
    //
    // This file contains all constants used across shaders to ensure
    // consistency and eliminate magic numbers in shader code.
    namespace ShaderConstants
    {
        // =============================================================================
        // PBR CONSTANTS
        // =============================================================================
        constexpr float PI = 3.14159265359f;
        constexpr float EPSILON = 0.0001f;

        // Default material values
        constexpr float DEFAULT_DIELECTRIC_F0 = 0.04f;
        constexpr float DEFAULT_ROUGHNESS = 0.5f;
        constexpr float DEFAULT_METALLIC = 0.0f;
        constexpr float DEFAULT_NORMAL_SCALE = 1.0f;
        constexpr float DEFAULT_OCCLUSION_STRENGTH = 1.0f;

        // IBL constants
        constexpr float MAX_REFLECTION_LOD = 4.0f;
        constexpr int IBL_PREFILTER_SAMPLES = 1024;
        constexpr int IBL_IRRADIANCE_SAMPLES = 512;

        // =============================================================================
        // RENDERING CONSTANTS
        // =============================================================================

        // Maximum number of bones for skeletal animation - centralized from ShaderBindingLayout
        constexpr int MAX_BONES = UBOStructures::AnimationConstants::MAX_BONES;

        // Shadow mapping constants.
        //
        // SHADOW_BIAS is a constant depth offset in the shadow map's own
        // NORMALIZED [0,1] depth. That unit is only meaningful for the LOCAL
        // LIGHT ATLAS, whose spot / point entries have a bounded perspective
        // range; the directional CSM's orthographic range is 400 m of fixed
        // z-padding plus the cascade's own extent (404-1300 m in the sample
        // scenes), so the same number there means 2-26 metres of world depth
        // and detaches every shadow from its caster. Issue #1119: the CSM's
        // bias is authored in SHADOW-MAP TEXELS instead and converted per
        // cascade in the shader — see SHADOW_CSM_DEPTH_BIAS_TEXELS.
        constexpr float SHADOW_BIAS = 0.005f;
        // Directional CSM constant depth bias, in shadow-map texels of the
        // cascade doing the lookup. Scale-free by construction: one texel is
        // one texel whether the cascade covers 4 m or 260 m, so the same
        // authored number behaves the same in every cascade, at every
        // MaxShadowDistance, and in every scene. Two texels comfortably
        // exceeds the depth slope a 3x3 PCF kernel (±1 texel) can see.
        constexpr float SHADOW_CSM_DEPTH_BIAS_TEXELS = 2.0f;
        constexpr int SHADOW_MAP_SIZE = 1024;

        // =============================================================================
        // TONE MAPPING CONSTANTS
        // =============================================================================
        constexpr float GAMMA = 2.2f;
        constexpr float EXPOSURE = 1.0f;

        // Tone mapping operators
        constexpr int TONEMAP_NONE = 0;
        constexpr int TONEMAP_REINHARD = 1;
        constexpr int TONEMAP_ACES = 2;
        constexpr int TONEMAP_UNCHARTED2 = 3;

        // =============================================================================
        // TEXTURE LIMITS
        // =============================================================================
        constexpr int MAX_TEXTURE_UNITS = 16;
        constexpr int MAX_CUBEMAP_SIZE = 2048;
        constexpr int MAX_TEXTURE_SIZE = 4096;
    } // namespace ShaderConstants
} // namespace OloEngine
