#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"

#include <glm/glm.hpp>
#include <span>
#include <vector>

namespace OloEngine
{
    class ComputeShader;
    class StorageBuffer;

    // @brief GPU-driven per-instance frustum culling for huge instance counts.
    //
    // For dense scatter / crowd / foliage scenes the CPU cull loop in
    // `Renderer3D::DrawMeshInstanced(span<glm::mat4>)` becomes the bottleneck
    // around 10k+ instances. This class moves the test to the GPU:
    //
    //   1. CPU uploads the full `InstanceData[]` to a private input SSBO
    //      (binding SSBO_INSTANCE_CULL_INPUT = 16).
    //   2. The cull compute (`InstanceFrustumCull.comp`) reads the input,
    //      derives the 6 frustum planes from the camera UBO's ViewProjection,
    //      transforms each instance's local bounding sphere to world space,
    //      and `atomicAdd`-appends survivors into an output SSBO bound at
    //      SSBO_INSTANCE_DATA = 15.
    //   3. The same atomic increment populates `instanceCount` in a 5-uint
    //      `DrawElementsIndirectCommand` SSBO bound at
    //      SSBO_INSTANCE_DRAW_INDIRECT = 17.
    //   4. `RendererAPI::DrawElementsIndirectRaw` reads the indirect command
    //      (no CPU round trip) and draws exactly the surviving instances.
    //
    // **Multiple GPU-cull submissions per frame**: each call returns a fresh
    // (input, output, indirect) triple from an internal pool. The output
    // buffer is a `Ref<InstanceBuffer>` that the command dispatcher binds at
    // SSBO_INSTANCE_DATA in place of `Renderer3D::s_Data.ModelInstanceBuffer`
    // — without the pool, two submissions in the same frame would clobber
    // each other's culled data at slot 15.
    //
    // **Frame lifetime**: pool slots become available again at
    // `BeginFrame()`. Buffers stay allocated across frames; their sizes grow
    // monotonically with the largest submission seen so far.
    class GPUFrustumCuller : public RefCounted
    {
      public:
        // Result returned by `Cull()` — the dispatcher binds these and calls
        // `DrawElementsIndirectRaw(vaoID, IndirectBufferID)`.
        struct CullResult
        {
            Ref<InstanceBuffer> OutputBuffer;     // bind at SSBO_INSTANCE_DATA = 15 before the draw
            Ref<StorageBuffer> IndirectBuffer;    // pass to glDrawElementsIndirect
            u32 InputCount = 0;                   // pre-cull count, recorded for the profiler
        };

        GPUFrustumCuller();
        ~GPUFrustumCuller();

        // Lazy-init the compute shader on first call so the engine startup
        // sequence (which initialises shaders after Renderer3D::Init) doesn't
        // depend on this object's construction order.
        void EnsureInitialised();

        // Reset the pool cursor so the next `Cull()` gets slot 0. Buffers
        // remain allocated (lifetime = engine, not = frame).
        void BeginFrame();

        // Dispatch one cull pass and return the result. The caller is
        // expected to attach `OutputBuffer` + `IndirectBuffer` to the
        // `DrawMeshInstancedCommand` it submits so the dispatcher can wire
        // them into the indirect draw.
        //
        // `localBoundingSphere` is the mesh's local-space sphere
        // (`mesh->GetBoundingSphere()`). `radiusExpansion` should match the
        // CPU cull path — 1.3 × 1.05 = 1.365 for static draws — so the two
        // paths produce identical survivor sets (verified by
        // GPUFrustumCullParityTest).
        CullResult Cull(std::span<const InstanceData> instances,
                        u32 indexCount, u32 baseIndex,
                        const glm::vec4& localBoundingSphere,
                        f32 radiusExpansion);

        // Number of pool slots currently in use this frame. Useful for the
        // RendererProfiler "Instanced Draws" tab and parity tests.
        [[nodiscard]] u32 GetFrameUsedSlotCount() const { return m_NextSlot; }
        [[nodiscard]] u32 GetPoolCapacity() const { return static_cast<u32>(m_Pool.size()); }

      private:
        struct PoolSlot
        {
            Ref<StorageBuffer> InputBuffer;     // binding 16 — CPU writes InstanceData[]
            Ref<InstanceBuffer> OutputBuffer;   // binding 15 — compute writes survivors
            Ref<StorageBuffer> IndirectBuffer;  // binding 17 — DrawElementsIndirectCommand
            u32 Capacity = 0;                   // in instances
        };

        PoolSlot& AcquireSlot(u32 requiredCapacity);
        void EnsureSlotCapacity(PoolSlot& slot, u32 requiredCapacity);

        Ref<ComputeShader> m_CullShader;
        std::vector<PoolSlot> m_Pool;
        u32 m_NextSlot = 0;
        bool m_Initialised = false;
    };
} // namespace OloEngine
