#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/LightCulling/LightGrid.h"
#include "OloEngine/Renderer/LightCulling/LightCullingBuffer.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Dispatches the light culling compute shader for Forward+ rendering.
    //
    // Reads depth buffer + light SSBOs, writes per-tile light index lists and
    // grid (offset, count). Called between the depth pre-pass and
    // the color pass within SceneRenderPass::Execute().
    class LightCullingPass
    {
    public:
        LightCullingPass() = default;
        ~LightCullingPass() = default;

        void Initialize();
        void Reload();

        // Dispatch the light culling compute shader
        void Dispatch(LightGrid& grid,
                      const LightCullingBuffer& lightBuffer,
                      const glm::mat4& viewMatrix,
                      const glm::mat4& projectionMatrix,
                      u32 depthTextureID);

        [[nodiscard]] bool IsValid() const;

    private:
        Ref<ComputeShader> m_CullingShader;
    };
} // namespace OloEngine
