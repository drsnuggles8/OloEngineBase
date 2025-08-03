#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ShaderConstants.h"
#include <vector>
#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * @brief Light buffer for managing multiple lights in shaders
     * 
     * This class manages an array of lights that can be uploaded to
     * the GPU as a uniform buffer object for efficient multi-light rendering.
     */
    class LightBuffer
    {
    public:
        /**
         * @brief Individual light data structure for GPU upload
         */
        struct LightData
        {
            glm::vec4 Position;         // Position in world space (w = 1.0 for point/spot, 0.0 for directional)
            glm::vec4 Direction;        // Direction for directional/spot lights
            glm::vec4 Color;            // Light color and intensity (w = intensity)
            glm::vec4 AttenuationParams; // (constant, linear, quadratic, range)
            glm::vec4 SpotParams;       // (inner_cutoff, outer_cutoff, falloff, type)
            
            static constexpr u32 GetSize() { return sizeof(LightData); }
        };

        /**
         * @brief Light buffer UBO structure
         */
        struct LightBufferUBO
        {
            i32 LightCount;                                           // Number of active lights
            i32 _padding[1];                                          // Reduced padding - only need 4 bytes for 16-byte alignment
            LightData Lights[ShaderConstants::MAX_LIGHTS];           // Array of light data
            
            static constexpr u32 GetSize() { return sizeof(LightBufferUBO); }
        };

    public:
        LightBuffer();
        ~LightBuffer() = default;

        /**
         * @brief Add a light to the buffer
         * @param light Light to add
         * @return true if light was added successfully, false if buffer is full
         */
        bool AddLight(const Light& light);

        /**
         * @brief Remove a light from the buffer
         * @param index Index of light to remove
         */
        void RemoveLight(u32 index);

        /**
         * @brief Clear all lights from the buffer
         */
        void Clear();

        /**
         * @brief Update light data at specific index
         * @param index Index of light to update
         * @param light New light data
         */
        void UpdateLight(u32 index, const Light& light);

        /**
         * @brief Get number of lights in buffer
         */
        u32 GetLightCount() const { return m_LightCount; }

        /**
         * @brief Check if buffer is full
         */
        bool IsFull() const { return m_LightCount >= ShaderConstants::MAX_LIGHTS; }

        /**
         * @brief Upload light data to GPU
         */
        void UploadToGPU();

        /**
         * @brief Bind the light buffer to the shader
         */
        void Bind();

        /**
         * @brief Get the uniform buffer object
         */
        const AssetRef<UniformBuffer>& GetUBO() const { return m_UBO; }

        /**
         * @brief Get light data at index
         */
        const LightData& GetLightData(u32 index) const;

        /**
         * @brief Set ambient light color
         */
        void SetAmbientLight(const glm::vec3& ambient) { m_AmbientLight = ambient; }

        /**
         * @brief Get ambient light color
         */
        const glm::vec3& GetAmbientLight() const { return m_AmbientLight; }

    private:
        u32 m_LightCount = 0;
        LightBufferUBO m_BufferData;
        AssetRef<UniformBuffer> m_UBO;
        glm::vec3 m_AmbientLight = glm::vec3(0.03f);

        /**
         * @brief Convert engine Light to GPU LightData
         */
        void ConvertLightToData(const Light& light, LightData& data);
    };

    /**
     * @brief Multi-light renderer for handling multiple light sources
     */
    class MultiLightRenderer
    {
    public:
        MultiLightRenderer();
        ~MultiLightRenderer() = default;

        /**
         * @brief Initialize the multi-light system
         */
        void Initialize();

        /**
         * @brief Add a light to the scene
         * @param light Light to add
         * @return Index of added light, or -1 if failed
         */
        i32 AddLight(const Light& light);

        /**
         * @brief Remove a light from the scene
         * @param index Index of light to remove
         */
        void RemoveLight(u32 index);

        /**
         * @brief Update light at index
         * @param index Index of light to update
         * @param light New light data
         */
        void UpdateLight(u32 index, const Light& light);

        /**
         * @brief Clear all lights
         */
        void ClearLights();

        /**
         * @brief Begin rendering with multi-light setup
         */
        void BeginRender();

        /**
         * @brief End rendering and upload light data
         */
        void EndRender();

        /**
         * @brief Get the light buffer
         */
        LightBuffer& GetLightBuffer() { return m_LightBuffer; }
        const LightBuffer& GetLightBuffer() const { return m_LightBuffer; }

        /**
         * @brief Set view position for lighting calculations
         */
        void SetViewPosition(const glm::vec3& viewPos) { m_ViewPosition = viewPos; }

        /**
         * @brief Get view position
         */
        const glm::vec3& GetViewPosition() const { return m_ViewPosition; }

    private:
        LightBuffer m_LightBuffer;
        glm::vec3 m_ViewPosition = glm::vec3(0.0f);
        bool m_Initialized = false;
    };
}
