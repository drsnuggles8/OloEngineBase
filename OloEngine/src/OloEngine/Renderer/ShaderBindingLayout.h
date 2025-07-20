#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * Standardized shader binding layout for consistent resource sharing
     * across all shaders in the engine. This ensures efficient data sharing
     * and eliminates binding conflicts.
     */
    class ShaderBindingLayout
    {
    public:
        // =============================================================================
        // UNIFORM BUFFER OBJECT (UBO) BINDINGS
        // =============================================================================
        
        static constexpr u32 UBO_CAMERA        = 0;  // Camera matrices (view, projection, etc.)
        static constexpr u32 UBO_LIGHTS        = 1;  // Lighting properties and data
        static constexpr u32 UBO_MATERIAL      = 2;  // Material properties
        static constexpr u32 UBO_MODEL         = 3;  // Model/transform matrices
        static constexpr u32 UBO_ANIMATION     = 4;  // Animation/bone matrices
        static constexpr u32 UBO_MULTI_LIGHTS  = 5;  // Multi-light buffer for advanced lighting
        static constexpr u32 UBO_USER_0        = 6;  // User-defined buffer 0
        static constexpr u32 UBO_USER_1        = 7;  // User-defined buffer 1
        static constexpr u32 UBO_USER_2        = 8;  // User-defined buffer 2
        
        // =============================================================================
        // TEXTURE SAMPLER BINDINGS
        // =============================================================================
        
        static constexpr u32 TEX_DIFFUSE       = 0;  // Primary diffuse/albedo texture
        static constexpr u32 TEX_SPECULAR      = 1;  // Specular/metallic texture
        static constexpr u32 TEX_NORMAL        = 2;  // Normal map
        static constexpr u32 TEX_HEIGHT        = 3;  // Height/displacement map
        static constexpr u32 TEX_AMBIENT       = 4;  // Ambient occlusion
        static constexpr u32 TEX_EMISSIVE      = 5;  // Emissive map
        static constexpr u32 TEX_ROUGHNESS     = 6;  // Roughness map
        static constexpr u32 TEX_METALLIC      = 7;  // Metallic map
        static constexpr u32 TEX_SHADOW        = 8;  // Shadow map
        static constexpr u32 TEX_ENVIRONMENT   = 9;  // Environment/skybox
        static constexpr u32 TEX_USER_0        = 10; // User-defined texture 0
        static constexpr u32 TEX_USER_1        = 11; // User-defined texture 1
        static constexpr u32 TEX_USER_2        = 12; // User-defined texture 2
        static constexpr u32 TEX_USER_3        = 13; // User-defined texture 3
        
        // =============================================================================
        // BUFFER STRUCTURE DEFINITIONS
        // =============================================================================
        
        struct CameraUBO
        {
            glm::mat4 ViewProjection;
            glm::mat4 View;
            glm::mat4 Projection;
            glm::vec3 Position;
            f32 _padding0;
            
            static constexpr u32 GetSize() { return sizeof(CameraUBO); }
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
            
            static constexpr u32 GetSize() { return sizeof(LightUBO); }
        };

        /**
         * @brief Multi-light UBO structure for advanced lighting scenarios
         * Aligned with LightBuffer::LightData for consistency
         */
        struct MultiLightData
        {
            glm::vec4 Position;         // Position in world space (w = 1.0 for point/spot, 0.0 for directional)
            glm::vec4 Direction;        // Direction for directional/spot lights
            glm::vec4 Color;            // Light color and intensity (w = intensity)
            glm::vec4 AttenuationParams; // (constant, linear, quadratic, range)
            glm::vec4 SpotParams;       // (inner_cutoff, outer_cutoff, falloff, type)
            
            static constexpr u32 GetSize() { return sizeof(MultiLightData); }
        };

        struct MultiLightUBO
        {
            static constexpr u32 MAX_LIGHTS = 32;                    // Maximum supported lights in the array
            
            i32 LightCount;                                           // Number of active lights
            i32 MaxLights;                                            // Maximum supported lights
            i32 ShadowCasterCount;                                    // Number of shadow-casting lights
            i32 Reserved;                                             // Reserved for future use (16-byte alignment)
            MultiLightData Lights[MAX_LIGHTS];                       // Array of light data
            
            static constexpr u32 GetSize() { return sizeof(MultiLightUBO); }
        };
        
        struct MaterialUBO
        {
            glm::vec4 Ambient;
            glm::vec4 Diffuse;
            glm::vec4 Specular;            // w = shininess
            glm::vec4 Emissive;
            i32 UseTextureMaps;
            i32 AlphaMode;                 // Alpha blending mode (repurposed from padding)
            i32 DoubleSided;               // Double-sided rendering flag (repurposed from padding)
            i32 _padding;                  // Only 4 bytes padding needed for 16-byte alignment
            
            static constexpr u32 GetSize() { return sizeof(MaterialUBO); }
        };
        
        struct PBRMaterialUBO
        {
            glm::vec4 BaseColorFactor;     // Base color (albedo) with alpha
            glm::vec4 EmissiveFactor;      // Emissive color
            f32 MetallicFactor;            // Metallic factor
            f32 RoughnessFactor;           // Roughness factor
            f32 NormalScale;               // Normal map scale
            f32 OcclusionStrength;         // AO strength
            i32 UseAlbedoMap;              // Use albedo texture
            i32 UseNormalMap;              // Use normal map
            i32 UseMetallicRoughnessMap;   // Use metallic-roughness texture
            i32 UseAOMap;                  // Use ambient occlusion map
            i32 UseEmissiveMap;            // Use emissive map
            i32 EnableIBL;                 // Enable IBL
            i32 ApplyGammaCorrection;      // Whether to apply gamma correction in this pass
            i32 AlphaCutoff;               // Alpha cutoff for transparency (repurposed from padding)
            
            static constexpr u32 GetSize() { return sizeof(PBRMaterialUBO); }
        };
        
        struct ModelUBO
        {
            glm::mat4 Model;
            glm::mat4 Normal;              // transpose(inverse(model))
            
            static constexpr u32 GetSize() { return sizeof(ModelUBO); }
        };
        
        struct AnimationUBO
        {
            static constexpr u32 MAX_BONES = 100;                    // Typical character rigs use 50-70 bones
            glm::mat4 BoneMatrices[MAX_BONES];
            
            static constexpr u32 GetSize() { return sizeof(AnimationUBO); }
        };
        
        struct IBLParametersUBO
        {
            f32 Roughness;
            f32 ExposureAdjustment;        // Renamed from _padding0 to serve actual purpose
            f32 IBLIntensity;              // Renamed from _padding1 to serve actual purpose  
            f32 IBLRotation;               // Environment rotation angle (repurposed from padding)
            
            static constexpr u32 GetSize() { return sizeof(IBLParametersUBO); }
        };
        
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

        static const char* GetMultiLightUBOLayout()
        {
            return R"(
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
    LightData u_Lights[32];
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
};)";
        }
        
        static const char* GetAnimationUBOLayout()
        {
            return R"(
layout(std140, binding = 4) uniform AnimationMatrices {
    mat4 u_BoneMatrices[100];
};)";
        }
        
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
    };
}
