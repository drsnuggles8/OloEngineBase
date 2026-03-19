#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include <vector>

namespace OloEngine
{
    // @brief Manages GPU light data SSBOs for Forward+ rendering.
    //
    // Packs scene point lights and spot lights into separate SSBOs
    // that the light culling compute shader reads from. Directional lights
    // bypass culling and are still handled via UBO (they affect all tiles).
    class LightCullingBuffer
    {
    public:
        LightCullingBuffer() = default;
        ~LightCullingBuffer() = default;

        void Initialize(u32 maxPointLights = 1024, u32 maxSpotLights = 256);

        // Upload light arrays to GPU SSBOs
        void Update(const std::vector<GPUPointLight>& pointLights,
                    const std::vector<GPUSpotLight>& spotLights);

        void Bind() const;
        void Unbind() const;

        [[nodiscard]] u32 GetPointLightCount() const { return m_PointLightCount; }
        [[nodiscard]] u32 GetSpotLightCount() const { return m_SpotLightCount; }
        [[nodiscard]] const Ref<StorageBuffer>& GetPointLightSSBO() const { return m_PointLightSSBO; }
        [[nodiscard]] const Ref<StorageBuffer>& GetSpotLightSSBO() const { return m_SpotLightSSBO; }
        [[nodiscard]] bool IsInitialized() const { return m_Initialized; }

    private:
        Ref<StorageBuffer> m_PointLightSSBO;
        Ref<StorageBuffer> m_SpotLightSSBO;
        u32 m_PointLightCount = 0;
        u32 m_SpotLightCount = 0;
        u32 m_MaxPointLights = 1024;
        u32 m_MaxSpotLights = 256;
        bool m_Initialized = false;
    };
} // namespace OloEngine
