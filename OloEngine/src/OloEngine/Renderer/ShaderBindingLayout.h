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
        static constexpr u32 UBO_USER_0        = 5;  // User-defined buffer 0
        static constexpr u32 UBO_USER_1        = 6;  // User-defined buffer 1
        static constexpr u32 UBO_USER_2        = 7;  // User-defined buffer 2
        
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
        
        struct MaterialUBO
        {
            glm::vec4 Ambient;
            glm::vec4 Diffuse;
            glm::vec4 Specular;            // w = shininess
            glm::vec4 Emissive;
            i32 UseTextureMaps;
            i32 _padding[3];
            
            static constexpr u32 GetSize() { return sizeof(MaterialUBO); }
        };
        
        struct ModelUBO
        {
            glm::mat4 Model;
            glm::mat4 Normal;              // transpose(inverse(model))
            
            static constexpr u32 GetSize() { return sizeof(ModelUBO); }
        };
        
        struct AnimationUBO
        {
            static constexpr u32 MAX_BONES = 100;
            glm::mat4 BoneMatrices[MAX_BONES];
            
            static constexpr u32 GetSize() { return sizeof(AnimationUBO); }
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
        
        static const char* GetMaterialUBOLayout()
        {
            return R"(
layout(std140, binding = 2) uniform MaterialProperties {
    vec4 u_MaterialAmbient;
    vec4 u_MaterialDiffuse;
    vec4 u_MaterialSpecular;
    vec4 u_MaterialEmissive;
    int u_UseTextureMaps;
    int _padding[3];
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
    };
}
