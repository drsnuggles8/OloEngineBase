#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>
#include <vector>
#include <mutex>

namespace OloEngine
{
    /**
     * @brief Frame-local staging buffer for variable-length render data
     *
     * This buffer stores bone matrices and instance transforms for the current frame.
     * Data is allocated linearly and reset at the start of each frame.
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
        static constexpr sizet DEFAULT_BONE_CAPACITY = 4096;       // ~256KB for bones
        static constexpr sizet DEFAULT_TRANSFORM_CAPACITY = 8192;  // ~512KB for transforms

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
        sizet GetBoneMatrixCount() const { return m_BoneMatrixOffset; }
        sizet GetTransformCount() const { return m_TransformOffset; }
        sizet GetBoneMatrixCapacity() const { return m_BoneMatrices.size(); }
        sizet GetTransformCapacity() const { return m_Transforms.size(); }

      private:
        std::vector<glm::mat4> m_BoneMatrices;
        std::vector<glm::mat4> m_Transforms;

        u32 m_BoneMatrixOffset = 0;    // Current allocation offset
        u32 m_TransformOffset = 0;     // Current allocation offset

        mutable std::mutex m_BoneMutex;
        mutable std::mutex m_TransformMutex;
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
