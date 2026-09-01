#pragma once

#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <memory>

namespace OloEngine
{
    // The GL 4.6 SSBO namespace is already full. GPU Scene deliberately aliases
    // two pass-local legacy instance slots and is bound only immediately before
    // a consumer; #994 will own that pass integration. It is never a global
    // sticky binding.
    struct GPUSceneBindingLayout
    {
        static constexpr u32 Instances = ShaderBindingLayout::SSBO_INSTANCE_DATA;
        static constexpr u32 Geometries = ShaderBindingLayout::SSBO_INSTANCE_CULL_INPUT;
        static_assert(Instances < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS);
        static_assert(Geometries < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS);
    };

    // CPU authority for stable GPU-scene identities. Extraction is staged and
    // committed in key order so registry iteration order cannot affect slots.
    class GPUScene
    {
      public:
        GPUScene();
        ~GPUScene();

        GPUScene(const GPUScene&) = delete;
        auto operator=(const GPUScene&) -> GPUScene& = delete;
        GPUScene(GPUScene&&) noexcept;
        auto operator=(GPUScene&&) noexcept -> GPUScene&;

        void BeginExtraction(u64 ownerToken, const glm::vec3& renderOrigin);
        void ExtractGeometry(const GPUSceneGeometryKey& key, const GPUSceneGeometryInput& input);
        void ExtractInstance(const GPUSceneInstanceKey& key, const GPUSceneInstanceInput& input);
        void ReportUnsupported(GPUSceneUnsupportedCategory category, u32 count = 1);
        [[nodiscard]] GPUSceneFrameUpdate EndExtraction();

        // GPU resources are explicit so CPU-only tools/tests can use the
        // registry without a renderer context. Resize preserves the RHI
        // object identity and the backend retires old storage frame-safely.
        void InitializeGPU(u32 initialInstanceCapacity = 64, u32 initialGeometryCapacity = 64);
        void Upload();
        // Pass-local aliases: call immediately before the consuming dispatch or
        // draw. Allocation/growth releases the aliases before pass setup.
        void Bind() const;
        void Shutdown();
        [[nodiscard]] bool HasGPUResources() const;
        [[nodiscard]] RHI::ResourceHandle GetInstanceBufferHandle() const;
        [[nodiscard]] RHI::ResourceHandle GetGeometryBufferHandle() const;

        [[nodiscard]] GPUSceneHandle FindGeometry(const GPUSceneGeometryKey& key) const;
        [[nodiscard]] GPUSceneHandle FindInstance(const GPUSceneInstanceKey& key) const;
        [[nodiscard]] bool IsGeometryHandleLive(GPUSceneHandle handle) const;
        [[nodiscard]] bool IsInstanceHandleLive(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneGeometry* GetGeometryRecord(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneInstance* GetInstanceRecord(GPUSceneHandle handle) const;
        [[nodiscard]] const GPUSceneFrameUpdate& GetLastFrameUpdate() const;

        // Invalidates every live handle while retaining slot generations. This
        // is safe for scene reloads and renderer restarts: stale handles cannot
        // become valid merely because allocation starts again at slot zero.
        void Reset();

      private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace OloEngine
