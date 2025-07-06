// Test example demonstrating Phase 1.2: Array Resource Support
// This file shows how to use the new ArrayResource system

#include "OloEngine.h"
#include "OloEngine/Renderer/ArrayResource.h"

namespace OloEngine
{
    /**
     * @brief Example demonstrating array resource usage
     */
    class ArrayResourceExample
    {
    private:
        Ref<Shader> m_Shader;
        Ref<StorageBufferArray> m_MaterialBufferArray;
        Ref<Texture2DArray> m_TextureArray;
        
    public:
        void Initialize()
        {
            // Create a shader (assuming it has array bindings)
            m_Shader = Shader::Create("assets/shaders/ArrayExample.glsl");
            
            // Get the resource registry from the shader
            auto& registry = m_Shader->GetResourceRegistry();
            
            // Create an array of storage buffers for materials
            m_MaterialBufferArray = registry.CreateArrayResource<StorageBuffer>("u_MaterialBuffers", 0, 16);
            
            // Create an array of textures for diffuse maps
            m_TextureArray = registry.CreateArrayResource<Texture2D>("u_DiffuseTextures", 16, 32);
            
            // Add individual storage buffers to the array
            for (u32 i = 0; i < 8; ++i)
            {
                // Create material data structure
                struct MaterialData
                {
                    glm::vec4 diffuseColor;
                    glm::vec4 specularColor;
                    f32 shininess;
                    f32 padding[3];
                };
                
                MaterialData material;
                material.diffuseColor = glm::vec4(1.0f, 0.5f, 0.2f, 1.0f);
                material.specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                material.shininess = 32.0f;
                
                auto buffer = StorageBuffer::Create(material);
                m_MaterialBufferArray->SetResource(i, buffer);
            }
            
            // Add individual textures to the array
            for (u32 i = 0; i < 16; ++i)
            {
                std::string texturePath = "assets/textures/material_" + std::to_string(i) + ".png";
                auto texture = Texture2D::Create(texturePath);
                m_TextureArray->SetResource(i, texture);
            }
            
            // Apply all array bindings
            registry.ApplyBindings();
            
            OLO_CORE_INFO("ArrayResource example initialized with {0} materials and {1} textures",
                         m_MaterialBufferArray->GetResourceCount(),
                         m_TextureArray->GetResourceCount());
        }
        
        void Render()
        {
            // Bind the shader
            m_Shader->Bind();
            
            // Array resources are automatically bound when ApplyBindings() is called
            // Individual array elements can be accessed in the shader using indices
            
            // Example: Draw multiple objects with different materials
            for (u32 i = 0; i < m_MaterialBufferArray->GetResourceCount(); ++i)
            {
                // Set material index uniform
                m_Shader->SetInt("u_MaterialIndex", i);
                
                // Draw object with this material
                // RenderCommand::DrawIndexed(...);
            }
        }
        
        void UpdateMaterial(u32 index, const glm::vec4& newColor)
        {
            if (index >= m_MaterialBufferArray->GetResourceCount())
                return;
                
            // Get the specific buffer from the array
            auto buffer = m_MaterialBufferArray->GetResource(index);
            if (buffer)
            {
                // Update the material data
                struct MaterialData
                {
                    glm::vec4 diffuseColor;
                    glm::vec4 specularColor;
                    f32 shininess;
                    f32 padding[3];
                };
                
                MaterialData updatedMaterial;
                updatedMaterial.diffuseColor = newColor;
                updatedMaterial.specularColor = glm::vec4(1.0f);
                updatedMaterial.shininess = 32.0f;
                
                buffer->SetData(updatedMaterial);
            }
        }
        
        void Shutdown()
        {
            if (m_MaterialBufferArray)
                m_MaterialBufferArray->Clear();
            if (m_TextureArray)
                m_TextureArray->Clear();
        }
    };
}

/*
Example GLSL shader that would work with this code:

#version 450 core

// Array of storage buffers for materials
layout(std430, binding = 0) restrict readonly buffer MaterialBuffer0 { MaterialData u_Material0; };
layout(std430, binding = 1) restrict readonly buffer MaterialBuffer1 { MaterialData u_Material1; };
// ... up to binding 15

// Array of textures
layout(binding = 16) uniform sampler2D u_DiffuseTextures[32];

// Material index to select which buffer/texture to use
uniform int u_MaterialIndex = 0;

struct MaterialData
{
    vec4 diffuseColor;
    vec4 specularColor;
    float shininess;
    float padding[3];
};

void main()
{
    // Access material data based on index
    MaterialData material;
    switch(u_MaterialIndex)
    {
        case 0: material = u_Material0; break;
        case 1: material = u_Material1; break;
        // ... handle other cases
        default: material = u_Material0; break;
    }
    
    // Sample texture from array
    vec4 texColor = texture(u_DiffuseTextures[u_MaterialIndex], texCoord);
    
    // Use material and texture data for lighting calculations
    gl_FragColor = texColor * material.diffuseColor;
}
*/
