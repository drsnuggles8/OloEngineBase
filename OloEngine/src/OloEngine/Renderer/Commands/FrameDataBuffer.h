#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Threading/Mutex.h"
#include <glm/glm.hpp>
#include <vector>
#include <atomic>
#include <array>
#include <cstring>

namespace OloEngine
{
    // Maximum number of worker threads for parallel command generation
    static constexpr u32 MAX_FRAME_DATA_WORKERS = 16;

    // Size of per-worker scratch buffer (in matrix count)
    static constexpr u32 WORKER_SCRATCH_BONE_CAPACITY = 256;      // ~16KB per worker
    static constexpr u32 WORKER_SCRATCH_TRANSFORM_CAPACITY = 512; // ~32KB per worker

    // RenderState table capacity — max unique render states per frame
    static constexpr u16 MAX_RENDER_STATES_PER_FRAME = 256;

    // MaterialData table capacity — max unique material configs per frame
    // Raised 1024 -> 4096 (issue #524): draws_unique_10000 stress scene overflowed
    // the old cap every frame. ~150 bytes/entry, so 4096 costs ~600KB, cheap
    // relative to the crowd/instancing stalls the old cap caused.
    static constexpr u16 MAX_MATERIAL_DATA_PER_FRAME = 4096;

    /**
     * @brief Per-worker scratch buffer for thread-local bone/transform accumulation
     *
     * Workers write to their local scratch during parallel command generation.
     * At merge time, scratch buffers are compacted into the main buffer.
     */
    struct alignas(OLO_PLATFORM_CACHE_LINE_SIZE) WorkerScratchBuffer
    {
        // Bone matrices scratch
        std::vector<glm::mat4> bones;
        u32 boneCount = 0;

        // Transform matrices scratch
        std::vector<glm::mat4> transforms;
        u32 transformCount = 0;

        // Offset mapping: local offset -> global offset (set after merge)
        u32 globalBoneOffset = 0;
        u32 globalTransformOffset = 0;

        WorkerScratchBuffer()
        {
            bones.reserve(WORKER_SCRATCH_BONE_CAPACITY);
            transforms.reserve(WORKER_SCRATCH_TRANSFORM_CAPACITY);
        }

        void Reset()
        {
            boneCount = 0;
            transformCount = 0;
            globalBoneOffset = 0;
            globalTransformOffset = 0;
            // Don't clear vectors - just reset counts to reuse memory
        }
    };

    /**
     * @brief Frame-local staging buffer for variable-length render data
     *
     * This buffer stores bone matrices and instance transforms for the current frame.
     * Data is allocated linearly and reset at the start of each frame.
     *
     * Supports parallel command generation via per-worker scratch buffers:
     * - Workers allocate from thread-local scratch (no synchronization)
     * - At frame end, scratch buffers are merged into main buffer
     * - Command offsets are updated to reflect final positions
     *
     * Commands store offset+count into this buffer instead of std::vector or std::span,
     * enabling POD command structures that can be sorted efficiently.
     *
     * Usage:
     *   1. Call Reset() at the start of each frame (in BeginScene)
     *   2. AllocateBoneMatrices() / AllocateTransforms() return offsets
     *   3. Write data to the buffer using GetBoneMatrixPtr() / GetTransformPtr()
     *   4. Commands reference data by offset+count
     *   5. At dispatch time, retrieve data using GetBoneMatrixPtr() / GetTransformPtr()
     */
    class FrameDataBuffer
    {
      public:
        // Raised 4096 -> 16384 (issue #524): anim_crowd_200 (200 skinned foxes) overflowed
        // the old cap every frame, and the resulting unthrottled error log spam stalled
        // the editor main thread. ~1MB for bones — cheap relative to that stall.
        static constexpr sizet DEFAULT_BONE_CAPACITY = 16384;
        static constexpr sizet DEFAULT_TRANSFORM_CAPACITY = 65536; // ~4MB for transforms; doubled to cover the parallel prev-transform stream
                                                                   // that CommandBucket auto-batching now allocates alongside the current
                                                                   // stream (~32k current + ~32k prev with headroom for non-batched draws).
        // Raised 16384 -> 262144 (issue #524): draws_instanced_200000 — one
        // InstancedMeshComponent with 200,000 instances, the GPU-cull path's own
        // documented design point (SubmitGPUCulledInstanced sizes its transform SSBO
        // to the full pre-cull count; only this stream was still capped) — measured
        // overflowing even the first 65536 bump when actually run in the editor.
        // 262144 clears 200k with ~30% headroom. ~1MB i32 stream.
        static constexpr sizet DEFAULT_ENTITY_ID_CAPACITY = 262144;

        FrameDataBuffer(sizet boneCapacity = DEFAULT_BONE_CAPACITY,
                        sizet transformCapacity = DEFAULT_TRANSFORM_CAPACITY,
                        sizet entityIDCapacity = DEFAULT_ENTITY_ID_CAPACITY);

        // Non-copyable
        FrameDataBuffer(const FrameDataBuffer&) = delete;
        FrameDataBuffer& operator=(const FrameDataBuffer&) = delete;

        /**
         * @brief Reset the buffer for a new frame
         *
         * Doesn't free memory, just resets allocation offsets.
         * Call at the start of BeginScene().
         */
        void Reset();

        /**
         * @brief Allocate space for bone matrices
         * @param count Number of bone matrices to allocate
         * @return Offset into the bone matrix buffer (in matrix units, not bytes)
         *
         * Returns UINT32_MAX if allocation fails (out of capacity).
         */
        u32 AllocateBoneMatrices(u32 count);

        /**
         * @brief Allocate space for instance transforms
         * @param count Number of transforms to allocate
         * @return Offset into the transform buffer (in matrix units, not bytes)
         *
         * Returns UINT32_MAX if allocation fails (out of capacity).
         */
        u32 AllocateTransforms(u32 count);

        /**
         * @brief Get pointer to bone matrix at offset
         * @param offset Offset returned by AllocateBoneMatrices
         * @return Pointer to the first matrix at this offset
         */
        glm::mat4* GetBoneMatrixPtr(u32 offset);
        const glm::mat4* GetBoneMatrixPtr(u32 offset) const;

        /**
         * @brief Get pointer to transform at offset
         * @param offset Offset returned by AllocateTransforms
         * @return Pointer to the first matrix at this offset
         */
        glm::mat4* GetTransformPtr(u32 offset);
        const glm::mat4* GetTransformPtr(u32 offset) const;

        /**
         * @brief Write bone matrices to allocated space
         * @param offset Offset returned by AllocateBoneMatrices
         * @param data Source bone matrix data
         * @param count Number of matrices to write
         */
        void WriteBoneMatrices(u32 offset, const glm::mat4* data, u32 count);

        /**
         * @brief Write transforms to allocated space
         * @param offset Offset returned by AllocateTransforms
         * @param data Source transform data
         * @param count Number of matrices to write
         */
        void WriteTransforms(u32 offset, const glm::mat4* data, u32 count);

        /**
         * @brief Allocate space for per-instance entity IDs
         * @param count Number of i32 entity IDs to allocate
         * @return Offset into the entity ID buffer (in i32 units, not bytes)
         *
         * Returns UINT32_MAX if allocation fails (out of capacity). Used by
         * CommandBucket auto-batching to preserve per-source EntityID across
         * the N-into-1 DrawMeshInstanced collapse so editor picking still
         * resolves to the original entity.
         */
        u32 AllocateEntityIDs(u32 count);

        /**
         * @brief Get pointer to entity IDs at offset
         */
        i32* GetEntityIDPtr(u32 offset);
        const i32* GetEntityIDPtr(u32 offset) const;

        /**
         * @brief Write entity IDs to allocated space
         */
        void WriteEntityIDs(u32 offset, const i32* data, u32 count);

        sizet GetEntityIDCount() const
        {
            return m_EntityIDOffset;
        }
        sizet GetEntityIDCapacity() const
        {
            return m_EntityIDs.size();
        }

        // ====================================================================
        // Per-instance Color (vec4) and Custom (f32) streams
        //
        // Used by Renderer3D::DrawMeshInstanced(InstanceData) so that
        // InstancedMeshComponent's per-instance tint + free float reach the
        // GPU. Allocated lazily — when the InstanceData overload writes them,
        // the dispatcher reads them. Default-construction (no allocation)
        // leaves InstanceData.Color = (1,1,1,1) and InstanceData.Custom = 0
        // in the SSBO upload.
        // ====================================================================
        // Raised 16384 -> 262144 (issue #524), matching DEFAULT_ENTITY_ID_CAPACITY —
        // same 200k-instance GPU-cull draw populates all three streams together.
        static constexpr sizet DEFAULT_COLOR_CAPACITY = 262144;
        static constexpr sizet DEFAULT_CUSTOM_CAPACITY = 262144;

        u32 AllocateColors(u32 count);
        glm::vec4* GetColorPtr(u32 offset);
        const glm::vec4* GetColorPtr(u32 offset) const;
        void WriteColors(u32 offset, const glm::vec4* data, u32 count);

        u32 AllocateCustoms(u32 count);
        f32* GetCustomPtr(u32 offset);
        const f32* GetCustomPtr(u32 offset) const;
        void WriteCustoms(u32 offset, const f32* data, u32 count);

        // Statistics
        sizet GetBoneMatrixCount() const
        {
            return m_BoneMatrixOffset;
        }
        sizet GetTransformCount() const
        {
            return m_TransformOffset;
        }
        sizet GetBoneMatrixCapacity() const
        {
            return m_BoneMatrices.size();
        }
        sizet GetTransformCapacity() const
        {
            return m_Transforms.size();
        }

        // ====================================================================
        // RenderState Table — per-frame deduplication of PODRenderState
        // ====================================================================

        /**
         * @brief Allocate (or reuse) a render state in the per-frame table
         * @param state The PODRenderState to store
         * @return u16 index into the table (deduplicated — identical states share an index)
         *
         * Thread-safe via mutex. Returns INVALID_RENDER_STATE_INDEX on overflow.
         */
        u16 AllocateRenderState(const PODRenderState& state);

        /**
         * @brief Look up a render state by index
         * @param index Index returned by AllocateRenderState
         * @return Reference to the stored PODRenderState
         */
        const PODRenderState& GetRenderState(u16 index) const;

        /**
         * @brief Current number of unique render states allocated this frame
         */
        u16 GetRenderStateCount() const
        {
            return m_RenderStateCount;
        }

        // ====================================================================
        // MaterialData Table — per-frame deduplication of PODMaterialData
        // ====================================================================

        /**
         * @brief Allocate (or reuse) material data in the per-frame table
         * @param data The PODMaterialData to store
         * @return u16 index into the table (deduplicated — identical data shares an index)
         *
         * Thread-safe via mutex. Returns INVALID_MATERIAL_DATA_INDEX on overflow.
         */
        u16 AllocateMaterialData(const PODMaterialData& data);

        /**
         * @brief Look up material data by index
         * @param index Index returned by AllocateMaterialData
         * @return Reference to the stored PODMaterialData
         */
        const PODMaterialData& GetMaterialData(u16 index) const;

        /**
         * @brief Current number of unique material data entries allocated this frame
         */
        u16 GetMaterialDataCount() const
        {
            return m_MaterialDataCount;
        }

        // ====================================================================
        // Thread-Local Scratch Buffer API for Parallel Command Generation
        // ====================================================================

        /**
         * @brief Prepare for parallel command generation
         *
         * Resets all worker scratch buffers. Call at the start of each frame.
         */
        void PrepareForParallelSubmission();

        /**
         * @brief Get scratch buffer by explicit worker index (no thread ID lookup)
         * @param workerIndex The worker index (typically from ParallelFor contextIndex)
         * @return Pair of (workerIndex, scratchBuffer pointer)
         */
        std::pair<u32, WorkerScratchBuffer*> GetScratchBufferByIndex(u32 workerIndex);

        /**
         * @brief Allocate bone matrices in worker's scratch buffer
         * @param workerIndex Worker thread index
         * @param count Number of bone matrices to allocate
         * @return Local offset within the scratch buffer
         */
        u32 AllocateBoneMatricesParallel(u32 workerIndex, u32 count);

        /**
         * @brief Allocate transforms in worker's scratch buffer
         * @param workerIndex Worker thread index
         * @param count Number of transforms to allocate
         * @return Local offset within the scratch buffer
         */
        u32 AllocateTransformsParallel(u32 workerIndex, u32 count);

        /**
         * @brief Write bone matrices to worker's scratch buffer
         * @param workerIndex Worker thread index
         * @param localOffset Local offset returned by AllocateBoneMatricesParallel
         * @param data Source bone matrix data
         * @param count Number of matrices to write
         */
        void WriteBoneMatricesParallel(u32 workerIndex, u32 localOffset, const glm::mat4* data, u32 count);

        /**
         * @brief Write transforms to worker's scratch buffer
         * @param workerIndex Worker thread index
         * @param localOffset Local offset returned by AllocateTransformsParallel
         * @param data Source transform data
         * @param count Number of matrices to write
         */
        void WriteTransformsParallel(u32 workerIndex, u32 localOffset, const glm::mat4* data, u32 count);

        /**
         * @brief Merge all worker scratch buffers into main buffer
         *
         * Must be called on main thread after all workers complete.
         * Updates global offsets in scratch buffers for command offset remapping.
         */
        void MergeScratchBuffers();

        /**
         * @brief Convert worker-local bone offset to global offset
         * @param workerIndex Worker thread index
         * @param localOffset Local offset within worker's scratch
         * @return Global offset in main buffer (valid after MergeScratchBuffers)
         */
        u32 GetGlobalBoneOffset(u32 workerIndex, u32 localOffset) const;

        /**
         * @brief Convert worker-local transform offset to global offset
         * @param workerIndex Worker thread index
         * @param localOffset Local offset within worker's scratch
         * @return Global offset in main buffer (valid after MergeScratchBuffers)
         */
        u32 GetGlobalTransformOffset(u32 workerIndex, u32 localOffset) const;

      private:
        std::vector<glm::mat4> m_BoneMatrices;
        std::vector<glm::mat4> m_Transforms;
        std::vector<i32> m_EntityIDs;
        std::vector<glm::vec4> m_Colors;
        std::vector<f32> m_Customs;

        u32 m_BoneMatrixOffset = 0; // Current allocation offset
        u32 m_TransformOffset = 0;  // Current allocation offset
        u32 m_EntityIDOffset = 0;   // Current allocation offset (i32 stream)
        u32 m_ColorOffset = 0;      // Current allocation offset (vec4 stream)
        u32 m_CustomOffset = 0;     // Current allocation offset (f32 stream)

        mutable FMutex m_BoneMutex;
        mutable FMutex m_TransformMutex;
        mutable FMutex m_EntityIDMutex;
        mutable FMutex m_ColorMutex;
        mutable FMutex m_CustomMutex;

        bool m_BoneOverflowLogged = false;      // Once-per-frame overflow warning
        bool m_TransformOverflowLogged = false; // Once-per-frame overflow warning
        bool m_EntityIDOverflowLogged = false;  // Once-per-frame overflow warning
        bool m_ColorOverflowLogged = false;     // Once-per-frame overflow warning
        bool m_CustomOverflowLogged = false;    // Once-per-frame overflow warning

        // ====================================================================
        // RenderState Table Storage
        // ====================================================================

        std::array<PODRenderState, MAX_RENDER_STATES_PER_FRAME> m_RenderStates{};
        u16 m_RenderStateCount = 0;
        mutable FMutex m_RenderStateMutex;
        bool m_RenderStateOverflowLogged = false; // Once-per-frame overflow warning

        // ====================================================================
        // MaterialData Table Storage
        // ====================================================================

        std::vector<PODMaterialData> m_MaterialData;
        u16 m_MaterialDataCount = 0;
        mutable FMutex m_MaterialDataMutex;
        bool m_MaterialDataOverflowLogged = false; // Once-per-frame overflow warning

        // ====================================================================
        // Thread-Local Scratch Buffer Storage
        // ====================================================================

        std::array<WorkerScratchBuffer, MAX_FRAME_DATA_WORKERS> m_WorkerScratchBuffers;
        bool m_ParallelSubmissionActive = false;
    };

    /**
     * @brief Global frame data buffer manager
     *
     * Provides static access to the frame data buffer for the current frame.
     * The buffer is reset by Renderer3D::BeginScene().
     */
    class FrameDataBufferManager
    {
      public:
        static void Init();
        static void Shutdown();

        // True once Init() has run (and before Shutdown()). Lets a caller that
        // doesn't own the manager — e.g. a test fixture sharing the process
        // with a renderer that already brought it up — reuse it instead of
        // re-Init()-ing (which asserts).
        static bool IsInitialized()
        {
            return s_Buffer != nullptr;
        }

        static FrameDataBuffer& Get();

      private:
        static FrameDataBuffer* s_Buffer;
    };

} // namespace OloEngine
