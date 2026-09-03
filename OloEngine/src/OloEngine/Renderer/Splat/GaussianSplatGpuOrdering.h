#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
// Included rather than forward-declared: Ref<T>::DecRef needs the complete type
// to reach RefCounted, so a forward declaration compiles the header and fails
// at every destructor.
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Splat/GaussianSplatCloud.h"
#include "OloEngine/Renderer/Splat/GaussianSplatView.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace OloEngine::GaussianSplat
{
    // The per-view ordering pass on the GPU (issue #1038): the cull, the LOD
    // test and the back-to-front sort that `BuildViewOrdering` does on the CPU,
    // as three compute dispatch groups feeding an indirect draw.
    //
    // WHY THIS EXISTS. The CPU pass is the one measurement that decided
    // ADR 0018 against making splats a runtime asset type: 16.6 ms at 500 k
    // splats on one thread, a whole 60 Hz frame. This is that decision's
    // precondition, so the numbers it produces are the numbers that reopen the
    // question. ADR 0018 section 5.2 records them, and they are what reversed that decision.
    //
    // NOTHING IS READ BACK ON THE FRAME PATH. The survivor count reaches the
    // draw through a GPU-written `DrawArraysIndirectCommand`, never through the
    // CPU. `ReadbackOrdering` exists only so the parity test can compare
    // against the CPU reference, and it stalls the pipeline by design.
    //
    // THE SHADERS LIVE UNDER assets/shaders/tests/. That is the same deliberate
    // choice the rasteriser made (ADR 0018 section 4): a splat pass claims no
    // binding in the engine's production UBO/SSBO namespace while it is a
    // prototype, and shaders under tests/ are excluded from the binding
    // validator that enforces that namespace. Promoting this to a real pass
    // means claiming bindings, which is a decision the ADR reserves.
    class GpuViewOrdering
    {
      public:
        GpuViewOrdering() = default;
        ~GpuViewOrdering();

        GpuViewOrdering(const GpuViewOrdering&) = delete;
        GpuViewOrdering& operator=(const GpuViewOrdering&) = delete;

        // Compiles the two compute shaders. Returns false (and leaves the
        // object unusable) if either fails, so a caller without a GL 4.6
        // context skips rather than asserting.
        [[nodiscard]] auto Initialize() -> bool;

        [[nodiscard]] auto IsReady() const -> bool
        {
            return m_Ready;
        }

        // Uploads `cloud` and sizes the per-view buffers. The sort array is
        // padded to a power of two of at least 512, because the bitonic network
        // is only defined on a power-of-two array and its shared-memory pass
        // owns a 512-element tile.
        void SetCloud(const SplatCloud& cloud);

        // One frame's ordering: cull + LOD + sort. Leaves the draw order in the
        // order buffer and the instance count in the indirect buffer.
        void BuildOrdering(const glm::mat4& view,
                           const glm::mat4& projection,
                           const glm::vec2& viewportPixels,
                           const ViewSettings& settings);

        // Binds the splat records, the order buffer and the shared view UBO,
        // then issues `glDrawArraysIndirect`. The caller owns blend and depth
        // state and must have the splat shader bound.
        void DrawIndirect();

        // TEST ONLY: stalls the GPU and returns what BuildOrdering produced, in
        // the same shape BuildViewOrdering fills on the CPU.
        void ReadbackOrdering(ViewOrdering& out) const;

        [[nodiscard]] auto PaddedCapacity() const -> u32
        {
            return m_PaddedCapacity;
        }
        [[nodiscard]] auto SplatCount() const -> u32
        {
            return m_SplatCount;
        }

        // Number of compute dispatches the last BuildOrdering issued, split
        // into the cull and the two sort modes. Reported by the measurement so
        // "the sort is N dispatches" is observed rather than recomputed.
        struct DispatchCounts
        {
            u32 Cull = 0;
            u32 SortGlobal = 0;
            u32 SortLocal = 0;
        };
        [[nodiscard]] auto LastDispatchCounts() const -> const DispatchCounts&
        {
            return m_Dispatches;
        }

        // Smallest power of two that is >= `count` and >= 512.
        [[nodiscard]] static auto PaddedCapacityFor(u32 count) -> u32;

      private:
        void ReleaseBindings() const;

        bool m_Ready = false;
        u32 m_Vao = 0;
        u32 m_SplatCount = 0;
        u32 m_PaddedCapacity = 0;
        DispatchCounts m_Dispatches;

        Ref<ComputeShader> m_CullShader;
        Ref<ComputeShader> m_SortShader;
        Ref<StorageBuffer> m_SplatBuffer;
        Ref<StorageBuffer> m_OrderBuffer;
        Ref<StorageBuffer> m_KeyBuffer;
        Ref<StorageBuffer> m_StatsBuffer;
        Ref<StorageBuffer> m_IndirectBuffer;
        Ref<UniformBuffer> m_CullUniforms;
        Ref<UniformBuffer> m_SortUniforms;
    };
} // namespace OloEngine::GaussianSplat
