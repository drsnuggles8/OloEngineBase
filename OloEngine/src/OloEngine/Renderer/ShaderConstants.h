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
        // EVERY SHADOW DEPTH BIAS IS AUTHORED IN TEXELS and converted in the
        // shader. A constant offset in a map's normalized [0,1] depth means a
        // different world distance everywhere it lands: the directional CSM's
        // orthographic range is 400 m of fixed z-padding plus the cascade's own
        // extent, so 0.005 there was 2-26 metres of world depth (issue #1119);
        // a local-light atlas entry is perspective with a 0.1 m near plane, so
        // there it was 0.05 d^2 metres -- 1.8 m six metres from a spot (#1533).
        // Directional CSM constant depth bias, in shadow-map texels of the
        // cascade doing the lookup. Scale-free by construction: one texel is
        // one texel whether the cascade covers 4 m or 260 m, so the same
        // authored number behaves the same in every cascade, at every
        // MaxShadowDistance, and in every scene. Two texels comfortably
        // exceeds the depth slope a 3x3 PCF kernel (±1 texel) can see.
        constexpr float SHADOW_CSM_DEPTH_BIAS_TEXELS = 2.0f;
        // Local-light atlas constant depth bias, in texels of the entry's tile
        // AT THE RECEIVER (#1533; ShadowAtlasBias.h is the conversion). A
        // perspective texel grows with distance while its depth compresses, so
        // the shader converts per receiver rather than per entry. Two texels,
        // as the cascades: with the 1.5-texel normal offset
        // (ATLAS_NORMAL_OFFSET_TEXELS in PBRCommon.glsl) it clears a 3x3
        // kernel's own plane to about 80 degrees of incidence.
        constexpr float SHADOW_ATLAS_DEPTH_BIAS_TEXELS = 2.0f;
        // The receiver's offset along its normal, in texels of the entry at the
        // receiver: the C++ name of PBRCommon.glsl's ATLAS_NORMAL_OFFSET_TEXELS.
        constexpr float SHADOW_ATLAS_NORMAL_OFFSET_TEXELS = 1.5f;
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
