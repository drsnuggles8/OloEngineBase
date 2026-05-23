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
        // LIGHT TYPES
        // =============================================================================
        constexpr int DIRECTIONAL_LIGHT = 0;
        constexpr int POINT_LIGHT = 1;
        constexpr int SPOT_LIGHT = 2;

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

        // Maximum number of lights supported
        constexpr int MAX_LIGHTS = 256;

        // Maximum number of bones for skeletal animation - centralized from ShaderBindingLayout
        constexpr int MAX_BONES = UBOStructures::AnimationConstants::MAX_BONES;

        // Shadow mapping constants
        constexpr float SHADOW_BIAS = 0.005f;
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
