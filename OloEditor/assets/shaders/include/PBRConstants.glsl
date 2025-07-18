// =============================================================================
// PBRConstants.glsl - Shared constants for PBR shaders
// Part of OloEngine Enhanced PBR System
// This file contains all the constants that should be consistent across PBR shaders
// =============================================================================

#ifndef PBR_CONSTANTS_GLSL
#define PBR_CONSTANTS_GLSL

// =============================================================================
// LIGHT TYPE CONSTANTS
// =============================================================================
#define DIRECTIONAL_LIGHT 0
#define POINT_LIGHT 1
#define SPOT_LIGHT 2

// =============================================================================
// MATHEMATICAL CONSTANTS
// =============================================================================
#define PI 3.14159265359
#define TWO_PI 6.28318530718
#define HALF_PI 1.57079632679
#define INV_PI 0.31830988618
#define INV_TWO_PI 0.15915494309
#define EPSILON 0.0001
#define LARGE_EPSILON 0.001

// =============================================================================
// PBR MATERIAL CONSTANTS
// =============================================================================
#define DEFAULT_DIELECTRIC_F0 0.04
#define MAX_REFLECTION_LOD 4.0
#define MIN_ROUGHNESS 0.04  // Minimum roughness to avoid numerical issues
#define MAX_ROUGHNESS 1.0

// =============================================================================
// RENDERING CONSTANTS
// =============================================================================
#define GAMMA 2.2
#define INV_GAMMA 0.45454545455
#define MAX_LIGHTS 32
#define MAX_BONES 100

// =============================================================================
// TEXTURE BINDING CONSTANTS (from ShaderBindingLayout.h)
// =============================================================================
#define TEX_DIFFUSE 0
#define TEX_SPECULAR 1
#define TEX_NORMAL 2
#define TEX_HEIGHT 3
#define TEX_AMBIENT 4
#define TEX_EMISSIVE 5
#define TEX_ENVIRONMENT 9
#define TEX_USER_0 10  // Irradiance map
#define TEX_USER_1 11  // Prefilter map
#define TEX_USER_2 12  // BRDF LUT

// =============================================================================
// UNIFORM BUFFER BINDING CONSTANTS
// =============================================================================
#define UBO_CAMERA 0
#define UBO_LIGHTS 1
#define UBO_MATERIAL 2
#define UBO_MODEL 3
#define UBO_BONES 4
#define UBO_MULTI_LIGHTS 5

// =============================================================================
// QUALITY SETTINGS
// =============================================================================
#define IBL_SAMPLE_COUNT_LOW 256
#define IBL_SAMPLE_COUNT_MEDIUM 512
#define IBL_SAMPLE_COUNT_HIGH 1024
#define IBL_SAMPLE_COUNT_ULTRA 2048

// =============================================================================
// COLOR SPACE CONVERSION
// =============================================================================
#define SRGB_TO_LINEAR(color) pow(color, vec3(GAMMA))
#define LINEAR_TO_SRGB(color) pow(color, vec3(INV_GAMMA))

// =============================================================================
// UTILITY MACROS
// =============================================================================
#define SATURATE(x) clamp(x, 0.0, 1.0)
#define SQUARE(x) ((x) * (x))
#define MAX3(a, b, c) max(a, max(b, c))
#define MIN3(a, b, c) min(a, min(b, c))

#endif // PBR_CONSTANTS_GLSL
