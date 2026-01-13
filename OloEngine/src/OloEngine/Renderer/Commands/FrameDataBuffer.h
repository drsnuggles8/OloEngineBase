#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Threading/Mutex.h"
#include <glm/glm.hpp>
#include <vector>
#include <atomic>
#include <array>
#include <thread>
#include <unordered_map>

namespace OloEngine
{
    // Maximum number of worker threads for parallel command generation
    static constexpr u32 MAX_FRAME_DATA_WORKERS = 16;

    // Size of per-worker scratch buffer (in matrix count)
    static constexpr u32 WORKER_SCRATCH_BONE_CAPACITY = 256;      // ~16KB per worker
    static constexpr u32 WORKER_SCRATCH_TRANSFORM_CAPACITY = 512; // ~32KB per worker

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
        static constexpr sizet DEFAULT_BONE_CAPACITY = 4096;      // ~256KB for bones
        static constexpr sizet DEFAULT_TRANSFORM_CAPACITY = 8192; // ~512KB for transforms

        FrameDataBuffer(sizet boneCapacity = DEFAULT_BONE_CAPACITY,
                        sizet transformCapacity = DEFAULT_TRANSFORM_CAPACITY);

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
        // Thread-Local Scratch Buffer API for Parallel Command Generation
        // ====================================================================

        /**
         * @brief Prepare for parallel command generation
         *
         * Resets all worker scratch buffers. Call at the start of each frame.
         */
        void PrepareForParallelSubmission();

        /**
         * @brief Register current thread as a worker and get scratch buffer
         * @return Pair of (workerIndex, scratchBuffer pointer)
         * @deprecated Use GetScratchBufferByIndex() with explicit worker index from ParallelFor
         */
        [[deprecated("Use GetScratchBufferByIndex() with explicit worker index from ParallelFor")]]
        std::pair<u32, WorkerScratchBuffer*> RegisterAndGetScratchBuffer();

        /**
         * @brief Get scratch buffer by explicit worker index (no thread ID lookup)
         * @param workerIndex The worker index (typically from ParallelFor contextIndex)
         * @return Pair of (workerIndex, scratchBuffer pointer)
         * @note This is the optimized path that avoids std::thread::id lookup and mutex contention
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

        /**
         * @brief Get worker index for current thread (-1 if not registered)
         * @deprecated Use explicit worker index from ParallelFor instead
         */
        [[deprecated("Use explicit worker index from ParallelFor instead")]]
        i32 GetCurrentWorkerIndex() const;

      private:
        std::vector<glm::mat4> m_BoneMatrices;
        std::vector<glm::mat4> m_Transforms;

        u32 m_BoneMatrixOffset = 0; // Current allocation offset
        u32 m_TransformOffset = 0;  // Current allocation offset

        mutable FMutex m_BoneMutex;
        mutable FMutex m_TransformMutex;

        // ====================================================================
        // Thread-Local Scratch Buffer Storage
        // ====================================================================

        std::array<WorkerScratchBuffer, MAX_FRAME_DATA_WORKERS> m_WorkerScratchBuffers;
        std::unordered_map<std::thread::id, u32> m_ThreadToWorkerIndex;
        mutable FMutex m_WorkerMapMutex;
        std::atomic<u32> m_NextWorkerIndex{ 0 };
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

        static FrameDataBuffer& Get();

      private:
        static FrameDataBuffer* s_Buffer;
    };

} // namespace OloEngine
