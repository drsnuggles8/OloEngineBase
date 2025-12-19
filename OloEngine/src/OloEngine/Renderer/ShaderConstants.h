#pragma once

#include "ShaderBindingLayout.h"
#include <string>

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
        constexpr int MAX_LIGHTS = 32;

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

        // =============================================================================
        // GLSL CONSTANT DEFINITIONS
        // This section provides GLSL-compatible constant definitions
        // that can be included in shader files
        // =============================================================================

        static const char* GetGLSLConstants()
        {
            return R"(
// =============================================================================
// SHADER CONSTANTS
// =============================================================================

// Light types
const int DIRECTIONAL_LIGHT = 0;
const int POINT_LIGHT = 1;
const int SPOT_LIGHT = 2;

// PBR constants
const float PI = 3.14159265359;
const float EPSILON = 0.0001;

// Default material values
const float DEFAULT_DIELECTRIC_F0 = 0.04;
const float DEFAULT_ROUGHNESS = 0.5;
const float DEFAULT_METALLIC = 0.0;
const float DEFAULT_NORMAL_SCALE = 1.0;
const float DEFAULT_OCCLUSION_STRENGTH = 1.0;

// IBL constants
const float MAX_REFLECTION_LOD = 4.0;

// Rendering constants
const int MAX_LIGHTS = 32;
const int MAX_BONES = UBOStructures::AnimationConstants::MAX_BONES;

// Shadow mapping constants
const float SHADOW_BIAS = 0.005;

// Tone mapping constants
const float GAMMA = 2.2;
const float EXPOSURE = 1.0;

// Tone mapping operators
const int TONEMAP_NONE = 0;
const int TONEMAP_REINHARD = 1;
const int TONEMAP_ACES = 2;
const int TONEMAP_UNCHARTED2 = 3;
)";
        }

        // =============================================================================
        // HELPER FUNCTIONS FOR SHADER GENERATION
        // =============================================================================

        // @brief Get GLSL preprocessor defines for constants
        // @return String containing #define statements for all constants
        static std::string GetGLSLDefines()
        {
            return std::string(R"(
#define DIRECTIONAL_LIGHT 0
#define POINT_LIGHT 1
#define SPOT_LIGHT 2
#define PI 3.14159265359
#define EPSILON 0.0001
#define DEFAULT_DIELECTRIC_F0 0.04
#define MAX_REFLECTION_LOD 4.0
#define MAX_LIGHTS 32
#define MAX_BONES )") +
                   std::to_string(UBOStructures::AnimationConstants::MAX_BONES) + R"(
#define GAMMA 2.2
#define TONEMAP_NONE 0
#define TONEMAP_REINHARD 1
#define TONEMAP_ACES 2
#define TONEMAP_UNCHARTED2 3
)";
        }
    } // namespace ShaderConstants
} // namespace OloEngine
