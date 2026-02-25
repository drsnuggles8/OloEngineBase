#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>
#include <string>

namespace OloEngine
{
    // =============================================================================
    // UNIFORM BUFFER OBJECT STRUCTURES
    // =============================================================================

    // @brief Namespace containing all UBO structure definitions for shader binding
    //
    // These structures define the exact memory layout expected by shaders and must
    // match the corresponding GLSL uniform block layouts for proper data binding.
    namespace UBOStructures
    {
        struct CameraUBO
        {
            glm::mat4 ViewProjection;
            glm::mat4 View;
            glm::mat4 Projection;
            glm::vec3 Position;
            f32 _padding0;

            static constexpr u32 GetSize()
            {
                return sizeof(CameraUBO);
            }
        };

        struct LightUBO
        {
            glm::vec4 LightPosition;
            glm::vec4 LightDirection;
            glm::vec4 LightAmbient;
            glm::vec4 LightDiffuse;
            glm::vec4 LightSpecular;
            glm::vec4 LightAttParams;      // (constant, linear, quadratic, _)
            glm::vec4 LightSpotParams;     // (cutOff, outerCutOff, _, _)
            glm::vec4 ViewPosAndLightType; // (viewPos.xyz, lightType)

            static constexpr u32 GetSize()
            {
                return sizeof(LightUBO);
            }
        };

        // @brief Multi-light UBO structure for advanced lighting scenarios
        // Aligned with LightBuffer::LightData for consistency
        struct MultiLightData
        {
            glm::vec4 Position;          // Position in world space (w = 1.0 for point/spot, 0.0 for directional)
            glm::vec4 Direction;         // Direction for directional/spot lights
            glm::vec4 Color;             // Light color and intensity (w = intensity)
            glm::vec4 AttenuationParams; // (constant, linear, quadratic, range)
            glm::vec4 SpotParams;        // (inner_cutoff, outer_cutoff, falloff, type)

            static constexpr u32 GetSize()
            {
                return sizeof(MultiLightData);
            }
        };

        struct MultiLightUBO
        {
            static constexpr u32 MAX_LIGHTS = 32; // Maximum supported lights in the array

            i32 LightCount;                    // Number of active lights
            i32 MaxLights;                     // Maximum supported lights
            i32 ShadowCasterCount;             // Number of shadow-casting lights
            i32 Reserved;                      // Reserved for future use (16-byte alignment)
            MultiLightData Lights[MAX_LIGHTS]; // Array of light data

            static constexpr u32 GetSize()
            {
                return sizeof(MultiLightUBO);
            }
        };

        // @brief Animation limits and constants for bone matrix uploads
        // Must match the corresponding GLSL shader array sizes
        struct AnimationConstants
        {
            static constexpr u32 MAX_BONES = 100; // Maximum bone matrices per animated mesh

            // Compile-time validation for reasonable limits
            static_assert(MAX_BONES > 0, "MAX_BONES must be positive");
            static_assert(MAX_BONES <= 200, "MAX_BONES exceeds reasonable GPU limits");
            static_assert(MAX_BONES % 4 == 0, "MAX_BONES should be multiple of 4 for optimal GPU alignment");
        };

        struct MaterialUBO
        {
            glm::vec4 Ambient;
            glm::vec4 Diffuse;
            glm::vec4 Specular; // w = shininess
            glm::vec4 Emissive;
            i32 UseTextureMaps;
            i32 AlphaMode;   // Alpha blending mode (repurposed from padding)
            i32 DoubleSided; // Double-sided rendering flag (repurposed from padding)
            i32 _padding;    // Only 4 bytes padding needed for 16-byte alignment

            static constexpr u32 GetSize()
            {
                return sizeof(MaterialUBO);
            }
        };

        struct PBRMaterialUBO
        {
            glm::vec4 BaseColorFactor;   // Base color (albedo) with alpha
            glm::vec4 EmissiveFactor;    // Emissive color
            f32 MetallicFactor;          // Metallic factor
            f32 RoughnessFactor;         // Roughness factor
            f32 NormalScale;             // Normal map scale
            f32 OcclusionStrength;       // AO strength
            i32 UseAlbedoMap;            // Use albedo texture
            i32 UseNormalMap;            // Use normal map
            i32 UseMetallicRoughnessMap; // Use metallic-roughness texture
            i32 UseAOMap;                // Use ambient occlusion map
            i32 UseEmissiveMap;          // Use emissive map
            i32 EnableIBL;               // Enable IBL
            i32 ApplyGammaCorrection;    // Whether to apply gamma correction in this pass
            i32 AlphaCutoff;             // Alpha cutoff for transparency (repurposed from padding)

            static constexpr u32 GetSize()
            {
                return sizeof(PBRMaterialUBO);
            }
        };

        struct ModelUBO
        {
            glm::mat4 Model;
            glm::mat4 Normal; // transpose(inverse(model))
            i32 EntityID;
            i32 _paddingEntity[3];

            static constexpr u32 GetSize()
            {
                return sizeof(ModelUBO);
            }
        };

        struct AnimationUBO
        {
            static constexpr u32 MAX_BONES = AnimationConstants::MAX_BONES; // Use centralized constant
            glm::mat4 BoneMatrices[MAX_BONES];

            static constexpr u32 GetSize()
            {
                return sizeof(AnimationUBO);
            }

            // Compile-time validation to ensure shader compatibility
            static_assert(MAX_BONES == AnimationConstants::MAX_BONES, "AnimationUBO::MAX_BONES must match AnimationConstants::MAX_BONES");
            static_assert(sizeof(BoneMatrices) == MAX_BONES * sizeof(glm::mat4), "BoneMatrices array size mismatch");
        };

        // @brief Shader constant generation utilities
        // These functions generate GLSL preprocessor defines to inject C++ constants into shaders
        struct ShaderConstantGenerator
        {
            static std::string GetAnimationDefines()
            {
                return std::string("#define MAX_BONES ") + std::to_string(AnimationConstants::MAX_BONES) + "\n";
            }

            static std::string GetLightingDefines()
            {
                return std::string("#define MAX_LIGHTS ") + std::to_string(MultiLightUBO::MAX_LIGHTS) + "\n";
            }

            static std::string GetAllShaderDefines()
            {
                return GetAnimationDefines() + GetLightingDefines();
            }
        };

        struct IBLParametersUBO
        {
            f32 Roughness;
            f32 ExposureAdjustment; // Renamed from _padding0 to serve actual purpose
            f32 IBLIntensity;       // Renamed from _padding1 to serve actual purpose
            f32 IBLRotation;        // Environment rotation angle (repurposed from padding)

            static constexpr u32 GetSize()
            {
                return sizeof(IBLParametersUBO);
            }
        };

        // @brief Terrain rendering parameters
        struct TerrainUBO
        {
            glm::vec4 WorldSizeAndHeightScale; // xy = world size X/Z, z = height scale, w = chunk size
            glm::vec4 TerrainParams;           // x = texel size, y = inv heightmap res, z = layerCount, w = triplanarSharpness
            i32 HeightmapResolution;
            i32 _terrainPad0 = 0;
            i32 _terrainPad1 = 0;
            i32 _terrainPad2 = 0;
            glm::vec4 TessFactors;          // x = inner, y = +X edge, z = -X edge, w = +Z edge
            glm::vec4 TessFactors2;         // x = -Z edge, y = morphFactor, z = LODLevel, w = tessEnabled flag
            glm::vec4 LayerTilingScales0;   // Tiling scales for layers 0-3
            glm::vec4 LayerTilingScales1;   // Tiling scales for layers 4-7
            glm::vec4 LayerBlendSharpness0; // Height blend sharpness for layers 0-3
            glm::vec4 LayerBlendSharpness1; // Height blend sharpness for layers 4-7

            static constexpr u32 GetSize()
            {
                return sizeof(TerrainUBO);
            }
        };

        // @brief Brush preview UBO for terrain editing visualization (binding 11)
        struct BrushPreviewUBO
        {
            glm::vec4 BrushPosAndRadius; // xyz = world position, w = radius
            glm::vec4 BrushParams;       // x = active (1.0/0.0), y = falloff, z = mode (0=sculpt, 1=paint), w = unused

            static constexpr u32 GetSize()
            {
                return sizeof(BrushPreviewUBO);
            }
        };

        struct FoliageUBO
        {
            f32 Time;
            f32 WindStrength;
            f32 WindSpeed;
            f32 ViewDistance;
            f32 FadeStart;
            f32 AlphaCutoff;
            f32 _pad0 = 0.0f;
            f32 _pad1 = 0.0f;
            glm::vec3 BaseColor;
            f32 _pad2 = 0.0f;

            static constexpr u32 GetSize()
            {
                return sizeof(FoliageUBO);
            }
        };

        // @brief Shadow mapping UBO for directional (CSM), spot, and point light shadows
        struct ShadowUBO
        {
            static constexpr u32 MAX_CSM_CASCADES = 4;
            static constexpr u32 MAX_SPOT_SHADOWS = 4;
            static constexpr u32 MAX_POINT_SHADOWS = 4;

            glm::mat4 DirectionalLightSpaceMatrices[MAX_CSM_CASCADES]; // Light VP per cascade
            glm::vec4 CascadePlaneDistances;                           // View-space far plane per cascade
            glm::vec4 ShadowParams;                                    // x=bias, y=normalBias, z=softness, w=maxShadowDistance
            glm::mat4 SpotLightSpaceMatrices[MAX_SPOT_SHADOWS];        // Light VP per spot shadow
            glm::vec4 PointLightShadowParams[MAX_POINT_SHADOWS];       // xyz=position, w=farPlane
            i32 DirectionalShadowEnabled;
            i32 SpotShadowCount;
            i32 PointShadowCount;
            i32 ShadowMapResolution;
            i32 CascadeDebugEnabled; // Visualize cascade boundaries
            i32 _shadowPad0 = 0;
            i32 _shadowPad1 = 0;
            i32 _shadowPad2 = 0;

            static constexpr u32 GetSize()
            {
                return sizeof(ShadowUBO);
            }
        };
    } // namespace UBOStructures

    // Standardized shader binding layout for consistent resource sharing
    // across all shaders in the engine. This ensures efficient data sharing
    // and eliminates binding conflicts.
    class ShaderBindingLayout
    {
      public:
        // =============================================================================
        // UNIFORM BUFFER OBJECT (UBO) BINDINGS
        // =============================================================================

        static constexpr u32 UBO_CAMERA = 0;       // Camera matrices (view, projection, etc.)
        static constexpr u32 UBO_LIGHTS = 1;       // Lighting properties and data
        static constexpr u32 UBO_MATERIAL = 2;     // Material properties
        static constexpr u32 UBO_MODEL = 3;        // Model/transform matrices
        static constexpr u32 UBO_ANIMATION = 4;    // Animation/bone matrices
        static constexpr u32 UBO_MULTI_LIGHTS = 5; // Multi-light buffer for advanced lighting
        static constexpr u32 UBO_SHADOW = 6;       // Shadow mapping matrices and parameters
        static constexpr u32 UBO_USER_0 = 7;       // User-defined buffer 0 (PostProcess)
        static constexpr u32 UBO_USER_1 = 8;       // User-defined buffer 1 (MotionBlur)
        static constexpr u32 UBO_SSAO = 9;         // SSAO parameters
        static constexpr u32 UBO_TERRAIN = 10;     // Terrain parameters (height scale, world size, etc.)
        static constexpr u32 UBO_FOLIAGE = 12;     // Foliage instance rendering parameters

        // =============================================================================
        // TEXTURE SAMPLER BINDINGS
        // =============================================================================

        static constexpr u32 TEX_DIFFUSE = 0;               // Primary diffuse/albedo texture
        static constexpr u32 TEX_SPECULAR = 1;              // Specular/metallic texture
        static constexpr u32 TEX_NORMAL = 2;                // Normal map
        static constexpr u32 TEX_HEIGHT = 3;                // Height/displacement map
        static constexpr u32 TEX_AMBIENT = 4;               // Ambient occlusion
        static constexpr u32 TEX_EMISSIVE = 5;              // Emissive map
        static constexpr u32 TEX_ROUGHNESS = 6;             // Roughness map
        static constexpr u32 TEX_METALLIC = 7;              // Metallic map
        static constexpr u32 TEX_SHADOW = 8;                // Shadow map (CSM, sampler2DArrayShadow)
        static constexpr u32 TEX_ENVIRONMENT = 9;           // Environment/skybox
        static constexpr u32 TEX_USER_0 = 10;               // User-defined texture 0
        static constexpr u32 TEX_USER_1 = 11;               // User-defined texture 1
        static constexpr u32 TEX_USER_2 = 12;               // User-defined texture 2
        static constexpr u32 TEX_SHADOW_SPOT = 13;          // Spot light shadow map (sampler2DArrayShadow)
        static constexpr u32 TEX_SHADOW_POINT_0 = 14;       // Point light shadow cubemap 0
        static constexpr u32 TEX_SHADOW_POINT_1 = 15;       // Point light shadow cubemap 1
        static constexpr u32 TEX_SHADOW_POINT_2 = 16;       // Point light shadow cubemap 2
        static constexpr u32 TEX_SHADOW_POINT_3 = 17;       // Point light shadow cubemap 3
        static constexpr u32 TEX_POSTPROCESS_LUT = 18;      // Post-process color grading LUT
        static constexpr u32 TEX_POSTPROCESS_DEPTH = 19;    // Post-process scene depth access
        static constexpr u32 TEX_SSAO = 20;                 // Blurred SSAO result
        static constexpr u32 TEX_SSAO_NOISE = 21;           // SSAO 4x4 rotation noise texture
        static constexpr u32 TEX_SCENE_NORMALS = 22;        // View-space normals from G-buffer
        static constexpr u32 TEX_TERRAIN_HEIGHTMAP = 23;    // Terrain heightmap (R32F)
        static constexpr u32 TEX_TERRAIN_SPLATMAP = 24;     // Terrain splatmap 0 (RGBA8, layers 0-3)
        static constexpr u32 TEX_TERRAIN_ALBEDO_ARRAY = 25; // Terrain albedo layer array (Texture2DArray)
        static constexpr u32 TEX_TERRAIN_NORMAL_ARRAY = 26; // Terrain normal map layer array (Texture2DArray)
        static constexpr u32 TEX_TERRAIN_ARM_ARRAY = 27;    // Terrain ARM layer array (Texture2DArray)
        static constexpr u32 TEX_TERRAIN_SPLATMAP_1 = 28;   // Terrain splatmap 1 (RGBA8, layers 4-7)

        // =============================================================================
        // SHADER STORAGE BUFFER OBJECT (SSBO) BINDINGS
        // =============================================================================

        static constexpr u32 SSBO_GPU_PARTICLES = 0;     // GPU particle data array
        static constexpr u32 SSBO_ALIVE_INDICES = 1;     // Compacted alive particle index buffer
        static constexpr u32 SSBO_COUNTERS = 2;          // Atomic counters (alive/dead/emit counts)
        static constexpr u32 SSBO_FREE_LIST = 3;         // Free-slot indices for emission recycling
        static constexpr u32 SSBO_INDIRECT_DRAW = 4;     // Indirect draw command buffer
        static constexpr u32 SSBO_EMIT_STAGING = 5;      // Staging buffer for newly emitted particles
        static constexpr u32 SSBO_FOLIAGE_INSTANCES = 6; // Foliage instance data (reserved for GPU-driven path)

        // =============================================================================
        // TYPE ALIASES FOR CONVENIENCE
        // =============================================================================

        using CameraUBO = UBOStructures::CameraUBO;
        using LightUBO = UBOStructures::LightUBO;
        using MultiLightData = UBOStructures::MultiLightData;
        using MultiLightUBO = UBOStructures::MultiLightUBO;
        using MaterialUBO = UBOStructures::MaterialUBO;
        using PBRMaterialUBO = UBOStructures::PBRMaterialUBO;
        using ModelUBO = UBOStructures::ModelUBO;
        using AnimationUBO = UBOStructures::AnimationUBO;
        using IBLParametersUBO = UBOStructures::IBLParametersUBO;
        using ShadowUBO = UBOStructures::ShadowUBO;
        using TerrainUBO = UBOStructures::TerrainUBO;
        using BrushPreviewUBO = UBOStructures::BrushPreviewUBO;
        using FoliageUBO = UBOStructures::FoliageUBO;

        // =============================================================================
        // GLSL LAYOUT STRINGS FOR CODE GENERATION
        // =============================================================================

        static const char* GetCameraUBOLayout()
        {
            return R"(
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};)";
        }

        static const char* GetLightUBOLayout()
        {
            return R"(
layout(std140, binding = 1) uniform LightProperties {
    vec4 u_LightPosition;
    vec4 u_LightDirection;
    vec4 u_LightAmbient;
    vec4 u_LightDiffuse;
    vec4 u_LightSpecular;
    vec4 u_LightAttParams;
    vec4 u_LightSpotParams;
    vec4 u_ViewPosAndLightType;
};)";
        }

        static std::string GetMultiLightUBOLayout()
        {
            return std::string(R"(
struct LightData {
    vec4 position;         // Position in world space (w = light type)
    vec4 direction;        // Direction for directional/spot lights
    vec4 color;            // Light color and intensity (w = intensity)
    vec4 attenuationParams; // (constant, linear, quadratic, range)
    vec4 spotParams;       // (inner_cutoff, outer_cutoff, falloff, enabled)
};

layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int _padding;
    LightData u_Lights[)") +
                   std::to_string(UBOStructures::MultiLightUBO::MAX_LIGHTS) + R"(];
};)";
        }

        static const char* GetMaterialUBOLayout()
        {
            return R"(
layout(std140, binding = 2) uniform MaterialProperties {
    vec4 u_MaterialAmbient;
    vec4 u_MaterialDiffuse;
    vec4 u_MaterialSpecular;
    vec4 u_MaterialEmissive;
    int u_UseTextureMaps;
    int u_AlphaMode;
    int u_DoubleSided;
    int _padding;
};)";
        }

        static const char* GetPBRMaterialUBOLayout()
        {
            return R"(
layout(std140, binding = 2) uniform PBRMaterialProperties {
    vec4 u_BaseColorFactor;
    vec4 u_EmissiveFactor;
    float u_MetallicFactor;
    float u_RoughnessFactor;
    float u_NormalScale;
    float u_OcclusionStrength;
    int u_UseAlbedoMap;
    int u_UseNormalMap;
    int u_UseMetallicRoughnessMap;
    int u_UseAOMap;
    int u_UseEmissiveMap;
    int u_EnableIBL;
    int u_ApplyGammaCorrection;
    int u_AlphaCutoff;
};)";
        }

        static const char* GetModelUBOLayout()
        {
            return R"(
layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};)";
        }

        static std::string GetAnimationUBOLayout()
        {
            // Compile-time validation that GLSL array size matches C++ constant
            constexpr u32 glsl_max_bones = UBOStructures::AnimationConstants::MAX_BONES;
            static_assert(glsl_max_bones == UBOStructures::AnimationUBO::MAX_BONES,
                          "GLSL MAX_BONES must match C++ AnimationUBO::MAX_BONES");

            return std::string(R"(
layout(std140, binding = 4) uniform AnimationMatrices {
    mat4 u_BoneMatrices[)") +
                   std::to_string(glsl_max_bones) + R"(];
};)";
        }

        // Compile-time validation for shader constant consistency
        static_assert(UBOStructures::AnimationUBO::MAX_BONES == UBOStructures::AnimationConstants::MAX_BONES,
                      "Animation shader constants must be consistent across UBO structures");

        static const char* GetStandardTextureBindings()
        {
            return R"(
layout(binding = 0) uniform sampler2D u_DiffuseMap;
layout(binding = 1) uniform sampler2D u_SpecularMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 3) uniform sampler2D u_HeightMap;
layout(binding = 4) uniform sampler2D u_AmbientMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;
layout(binding = 6) uniform sampler2D u_RoughnessMap;
layout(binding = 7) uniform sampler2D u_MetallicMap;)";
        }

        static const char* GetPBRTextureBindings()
        {
            return R"(
layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 4) uniform sampler2D u_AOMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;
layout(binding = 9) uniform samplerCube u_EnvironmentMap;
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D u_BRDFLutMap;)";
        }

        static const char* GetShadowUBOLayout()
        {
            return R"(
layout(std140, binding = 6) uniform ShadowData {
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;  // x=bias, y=normalBias, z=softness, w=maxShadowDistance
    mat4 u_SpotLightSpaceMatrices[4];
    vec4 u_PointLightShadowParams[4]; // xyz=position, w=farPlane
    int u_DirectionalShadowEnabled;
    int u_SpotShadowCount;
    int u_PointShadowCount;
    int u_ShadowMapResolution;
    int u_CascadeDebugEnabled;
    int _shadowPad0;
    int _shadowPad1;
    int _shadowPad2;
};)";
        }
    };
} // namespace OloEngine
