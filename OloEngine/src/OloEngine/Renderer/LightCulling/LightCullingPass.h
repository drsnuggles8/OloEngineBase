#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/LightCulling/LightGrid.h"
#include "OloEngine/Renderer/LightCulling/LightCullingBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Dispatches the clustered (froxel) light culling compute shader
    // for Forward+ rendering (issue #435).
    //
    // Reads the light SSBOs and writes per-cluster light index lists and the
    // cluster grid (offset, count). With prepass depth it first compacts
    // occupied froxels and culls them indirectly; without depth it dispatches
    // every cluster in the fixed grid. Called between the optional depth
    // prepass and the color pass within SceneRenderPass::Execute().
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
                      f32 farPlane,
                      RHI::ResourceHandle sceneDepth,
                      bool depthPrepassAvailable);

        [[nodiscard]] bool IsValid() const;
        [[nodiscard]] bool WasLastDispatchDepthAware() const
        {
            return m_LastDispatchDepthAware;
        }

      private:
        Ref<ComputeShader> m_CullingShader;
        Ref<ComputeShader> m_DepthPrepareShader;
        Ref<ComputeShader> m_DepthAwareCullingShader;
        // LightCulling.comp's former bare uniforms (issue #691), at
        // UBO_LIGHT_CULLING. C++ twin: UBOStructures::LightCullingUBO.
        Ref<UniformBuffer> m_ParamsUBO;
        bool m_LastDispatchDepthAware = false;
    };
} // namespace OloEngine
