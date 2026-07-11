#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/LightCulling/LightGrid.h"
#include "OloEngine/Renderer/LightCulling/LightCullingBuffer.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Dispatches the clustered (froxel) light culling compute shader
    // for Forward+ rendering (issue #435).
    //
    // Reads the light SSBOs and writes per-cluster light index lists and the
    // cluster grid (offset, count). One workgroup per froxel cluster; no
    // depth input — every cluster in the fixed 3D grid is culled against the
    // scene lights. Called between the depth pre-pass and the color pass
    // within SceneRenderPass::Execute().
    class LightCullingPass
    {
      public:
        LightCullingPass() = default;
        ~LightCullingPass() = default;

        void Initialize();
        void Shutdown();
        void Reload();

        // Dispatch the light culling compute shader. nearPlane/farPlane are
        // the camera clip planes driving the exponential depth-slice mapping
        // (extract via ClusteredLighting::ExtractClipPlanes when only the
        // projection matrix is available).
        void Dispatch(LightGrid& grid,
                      const LightCullingBuffer& lightBuffer,
                      const glm::mat4& viewMatrix,
                      const glm::mat4& projectionMatrix,
                      f32 nearPlane,
                      f32 farPlane);

        [[nodiscard]] bool IsValid() const;

      private:
        Ref<ComputeShader> m_CullingShader;
    };
} // namespace OloEngine
