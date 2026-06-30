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
            Ref<InstanceBuffer> OutputBuffer;  // bind at SSBO_INSTANCE_DATA = 15 before the draw
            Ref<StorageBuffer> IndirectBuffer; // pass to glDrawElementsIndirect
            u32 InputCount = 0;                // pre-cull count, recorded for the profiler
        };

        // Per-frame Hi-Z occlusion inputs (#431). When `Enabled` and a valid
        // HZB is supplied via `SetOcclusion()`, `Cull()` dispatches the
        // frustum + occlusion compute (`InstanceOcclusionCull.comp`) instead of
        // the frustum-only one, additionally rejecting instances whose
        // screen-space bounds are fully behind the retained depth pyramid.
        //
        // The HZB is built from the PREVIOUS frame's scene depth (this frame's
        // depth does not exist yet at submission time), so bounds are
        // reprojected with `PrevViewProjection` — see InstanceOcclusionCull.comp.
        struct HZBOcclusionInputs
        {
            bool Enabled = false;
            u32 HZBTextureID = 0; // max-reduction pyramid (last frame)
            u32 MipCount = 0;
            glm::mat4 PrevViewProjection{ 1.0f }; // last frame's view-projection
            glm::vec2 HZBSize{ 0.0f };            // HZB texture size in texels (power-of-2)
            glm::vec2 HZBUVFactor{ 1.0f };        // viewport / HZB size
            f32 DepthBias = 0.0f;                 // device-Z slack before a reject fires

            [[nodiscard]] bool IsUsable() const
            {
                return Enabled && HZBTextureID != 0 && MipCount > 0;
            }
        };

        // Result of a two-phase phase-1 cull (#431 Stage 2). Phase 1 (dispatched
        // at submission, against the PREVIOUS frame's HZB) writes the survivors +
        // their indirect command (drawn first) AND appends frustum-visible but
        // occluded instances to a reject buffer. `DispatchPhase2()` later re-tests
        // that reject buffer against the CURRENT frame's HZB and fills the
        // phase-2 survivor/indirect buffers (the disocclusion recovery that
        // removes one-frame popping).
        struct TwoPhaseCullResult
        {
            Ref<InstanceBuffer> Phase1Output;   // bind at SSBO_INSTANCE_DATA (15) for the phase-1 draw
            Ref<StorageBuffer> Phase1Indirect;  // phase-1 glDrawElementsIndirect args
            Ref<InstanceBuffer> Phase2Output;   // bind at 15 for the phase-2 draw
            Ref<StorageBuffer> Phase2Indirect;  // phase-2 glDrawElementsIndirect args
            Ref<StorageBuffer> RejectedBuffer;  // compacted occluded-in-phase-1 InstanceData[]
            Ref<StorageBuffer> RejectedCounter; // single-uint reject count (phase-2 dispatch bound)
            u32 InputCount = 0;                 // pre-cull count (= phase-2 worst-case dispatch size)
            u32 IndexCount = 0;
            u32 BaseIndex = 0;
            glm::vec4 LocalBoundingSphere{ 0.0f };
            f32 RadiusExpansion = 1.0f;
        };

        GPUFrustumCuller();
        ~GPUFrustumCuller();

        // Lazy-init the compute shader on first call so the engine startup
        // sequence (which initialises shaders after Renderer3D::Init) doesn't
        // depend on this object's construction order.
        void EnsureInitialised();

        // Reset the pool cursor so the next `Cull()` gets slot 0. Buffers
        // remain allocated (lifetime = engine, not = frame). Also clears the
        // per-frame occlusion inputs — a frame that never calls SetOcclusion()
        // falls back to frustum-only culling automatically.
        void BeginFrame();

        // Supply this frame's Hi-Z occlusion inputs. Call once after
        // BeginFrame() and before any Cull(); subsequent Cull() calls switch to
        // the frustum + occlusion compute when `inputs.IsUsable()`. Cheap — just
        // stashes the struct.
        void SetOcclusion(const HZBOcclusionInputs& inputs)
        {
            m_Occlusion = inputs;
        }
        [[nodiscard]] bool IsOcclusionActive() const
        {
            return m_Occlusion.IsUsable() && m_OcclusionCullShader;
        }

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

        // Two-phase phase 1 (#431 Stage 2). Dispatched at submission against the
        // per-frame occlusion inputs (SetOcclusion → previous frame's HZB):
        // frustum-tests every instance, draws survivors via Phase1*, and appends
        // frustum-visible-but-occluded instances to the reject buffer for phase
        // 2. Requires IsOcclusionActive(); falls back to a plain frustum/single
        // cull semantics when no HZB is available (no rejects, all survive).
        TwoPhaseCullResult CullTwoPhasePhase1(std::span<const InstanceData> instances,
                                              u32 indexCount, u32 baseIndex,
                                              const glm::vec4& localBoundingSphere,
                                              f32 radiusExpansion);

        // Two-phase phase 2 (#431 Stage 2). Re-tests `result`'s reject buffer
        // against `currentHZB` (this frame's depth pyramid, no reprojection) and
        // fills Phase2Output / Phase2Indirect with the instances that turned out
        // visible. Call at execute time, after the current-frame HZB is built.
        void DispatchPhase2(const TwoPhaseCullResult& result, const HZBOcclusionInputs& currentHZB);

        // Number of pool slots currently in use this frame. Useful for the
        // RendererProfiler "Instanced Draws" tab and parity tests.
        [[nodiscard]] u32 GetFrameUsedSlotCount() const
        {
            return m_NextSlot;
        }
        [[nodiscard]] u32 GetPoolCapacity() const
        {
            return static_cast<u32>(m_Pool.size());
        }

      private:
        struct PoolSlot
        {
            Ref<StorageBuffer> InputBuffer;    // binding 16 — CPU writes InstanceData[]
            Ref<InstanceBuffer> OutputBuffer;  // binding 15 — compute writes survivors
            Ref<StorageBuffer> IndirectBuffer; // binding 17 — DrawElementsIndirectCommand
            u32 Capacity = 0;                  // in instances

            // Two-phase additions (#431 Stage 2), allocated lazily by
            // EnsureTwoPhaseBuffers() only when the slot drives a two-phase cull
            // so the frustum-only / single-phase paths pay no extra VRAM.
            Ref<StorageBuffer> RejectedBuffer;  // binding 18 (write) / 16 (phase-2 read)
            Ref<StorageBuffer> RejectedCounter; // binding 19 — single uint
            Ref<InstanceBuffer> Phase2Output;   // binding 15 (phase-2) — phase-2 survivors
            Ref<StorageBuffer> Phase2Indirect;  // binding 17 (phase-2)
            u32 TwoPhaseCapacity = 0;           // in instances
        };

        PoolSlot& AcquireSlot(u32 requiredCapacity);
        void EnsureSlotCapacity(PoolSlot& slot, u32 requiredCapacity) const;
        void EnsureTwoPhaseBuffers(PoolSlot& slot, u32 requiredCapacity) const;

        Ref<ComputeShader> m_CullShader;
        // Frustum + Hi-Z occlusion variant (#431). Null until EnsureInitialised
        // loads it; a load failure leaves it null and Cull() degrades to the
        // frustum-only path even when occlusion inputs are supplied.
        Ref<ComputeShader> m_OcclusionCullShader;
        HZBOcclusionInputs m_Occlusion;
        std::vector<PoolSlot> m_Pool;
        u32 m_NextSlot = 0;
        bool m_Initialised = false;
    };
} // namespace OloEngine
