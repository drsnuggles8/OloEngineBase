#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <vector>

namespace OloEngine
{
    struct GPUSceneHandle
    {
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();

        u32 m_Index = InvalidIndex;
        u32 m_Generation = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return m_Index != InvalidIndex && m_Generation != 0;
        }

        [[nodiscard]] auto operator==(const GPUSceneHandle&) const -> bool = default;
    };

    struct GPUSceneAllocationPolicy
    {
        // GPU Scene is committed once per view/frame. A removed slot cannot be
        // recycled until the frame-resource slot that could still reference it
        // has completed on the GPU.
        static constexpr u64 RetirementFrameCount = 2;

        [[nodiscard]] static constexpr u64 RetirementReadyFrame(u64 currentFrame)
        {
            constexpr u64 maxFrame = std::numeric_limits<u64>::max();
            return currentFrame > maxFrame - RetirementFrameCount
                       ? maxFrame
                       : currentFrame + RetirementFrameCount;
        }

        // Zero is the invalid generation. Returning it permanently retires a
        // max-generation slot instead of wrapping an ancient handle alive.
        [[nodiscard]] static constexpr u32 NextGeneration(u32 current)
        {
            return current == std::numeric_limits<u32>::max() ? 0u : current + 1u;
        }

        [[nodiscard]] static constexpr u32 GrowCapacity(u32 current, u32 required)
        {
            u32 capacity = std::max(current, 1u);
            while (capacity < required)
            {
                if (capacity > std::numeric_limits<u32>::max() / 2u)
                {
                    return required;
                }
                capacity *= 2u;
            }
            return capacity;
        }
    };

    enum GPUSceneInstanceFlag : u32
    {
        GPUSceneInstanceFlagActive = 1u << 0,
    };

    enum GPUSceneGeometryFlag : u32
    {
        GPUSceneGeometryFlagActive = 1u << 0,
    };

    enum class GPUSceneVertexFormat : u32
    {
        Unknown = 0,
        OloVertex = 1,
    };

    enum class GPUSceneIndexFormat : u32
    {
        Unknown = 0,
        UInt32 = 1,
    };

    // Three affine rows avoid uploading the invariant fourth matrix row. The
    // representation is intentionally shared with the std430 shader contract.
    struct alignas(16) GPUSceneTransform
    {
        glm::vec4 Row0{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec4 Row1{ 0.0f, 1.0f, 0.0f, 0.0f };
        glm::vec4 Row2{ 0.0f, 0.0f, 1.0f, 0.0f };
    };

    struct alignas(16) GPUSceneInstance
    {
        GPUSceneTransform CurrentTransform;
        GPUSceneTransform PreviousTransform;

        u32 GeometryIndex = GPUSceneHandle::InvalidIndex;
        u32 GeometryGeneration = 0;
        u32 MaterialIndex = GPUSceneHandle::InvalidIndex;
        u32 StableIndex = GPUSceneHandle::InvalidIndex;

        u32 VisibilityMask = 0;
        u32 Flags = 0;
        u32 Generation = 0;
        u32 Pad0 = 0;
    };

    struct alignas(16) GPUSceneGeometry
    {
        u32 VertexBufferIndex = RHI::ResourceHandle::InvalidIndex;
        u32 VertexBufferGeneration = 0;
        u32 IndexBufferIndex = RHI::ResourceHandle::InvalidIndex;
        u32 IndexBufferGeneration = 0;

        u64 VertexAddress = 0;
        u64 IndexAddress = 0;

        u32 VertexFormat = 0;
        u32 IndexFormat = 0;
        u32 FirstIndex = 0;
        u32 IndexCount = 0;

        i32 BaseVertex = 0;
        u32 VertexCount = 0;
        u32 Generation = 0;
        u32 Flags = 0;
    };

    static_assert(sizeof(GPUSceneTransform) == 48);
    static_assert(alignof(GPUSceneTransform) == 16);
    static_assert(sizeof(GPUSceneInstance) == 128);
    static_assert(alignof(GPUSceneInstance) == 16);
    static_assert(std::is_standard_layout_v<GPUSceneInstance>);
    static_assert(std::is_trivially_copyable_v<GPUSceneInstance>);
    static_assert(offsetof(GPUSceneInstance, CurrentTransform) == 0);
    static_assert(offsetof(GPUSceneInstance, PreviousTransform) == 48);
    static_assert(offsetof(GPUSceneInstance, GeometryIndex) == 96);
    static_assert(offsetof(GPUSceneInstance, VisibilityMask) == 112);
    static_assert(sizeof(GPUSceneGeometry) == 64);
    static_assert(alignof(GPUSceneGeometry) == 16);
    static_assert(std::is_standard_layout_v<GPUSceneGeometry>);
    static_assert(std::is_trivially_copyable_v<GPUSceneGeometry>);
    static_assert(offsetof(GPUSceneGeometry, VertexAddress) == 16);
    static_assert(offsetof(GPUSceneGeometry, VertexFormat) == 32);
    static_assert(offsetof(GPUSceneGeometry, BaseVertex) == 48);
    static_assert(offsetof(GPUSceneGeometry, Generation) == 56);

    struct GPUSceneDirtyRange
    {
        u32 m_FirstIndex = 0;
        u32 m_Count = 0;

        [[nodiscard]] auto operator==(const GPUSceneDirtyRange&) const -> bool = default;
    };

    enum class GPUSceneUnsupportedCategory : u32
    {
        Virtualized = 0,
        SoftwareRaster,
        Procedural,
        Terrain,
        Foliage,
        Particles,
        Fluids,
        Skinned,
        LegacyModel,
        LegacySubmesh,
        Tiles,
        Cloth,
        Count,
    };

    static constexpr sizet GPUSceneUnsupportedCategoryCount =
        static_cast<sizet>(GPUSceneUnsupportedCategory::Count);

    [[nodiscard]] constexpr const char* GetGPUSceneUnsupportedCategoryName(
        GPUSceneUnsupportedCategory category)
    {
        switch (category)
        {
            case GPUSceneUnsupportedCategory::Virtualized:
                return "Virtualized";
            case GPUSceneUnsupportedCategory::SoftwareRaster:
                return "Software raster";
            case GPUSceneUnsupportedCategory::Procedural:
                return "Procedural";
            case GPUSceneUnsupportedCategory::Terrain:
                return "Terrain";
            case GPUSceneUnsupportedCategory::Foliage:
                return "Foliage";
            case GPUSceneUnsupportedCategory::Particles:
                return "Particles";
            case GPUSceneUnsupportedCategory::Fluids:
                return "Fluids";
            case GPUSceneUnsupportedCategory::Skinned:
                return "Skinned";
            case GPUSceneUnsupportedCategory::LegacyModel:
                return "Legacy model";
            case GPUSceneUnsupportedCategory::LegacySubmesh:
                return "Legacy submesh";
            case GPUSceneUnsupportedCategory::Tiles:
                return "Tiles";
            case GPUSceneUnsupportedCategory::Cloth:
                return "Cloth";
            case GPUSceneUnsupportedCategory::Count:
                break;
        }
        return "Unknown";
    }

    struct GPUSceneFrameStats
    {
        u32 m_LiveInstances = 0;
        u32 m_InstanceSlotCount = 0;
        u32 m_InstanceBufferCapacity = 0;
        u32 m_LiveGeometries = 0;
        u32 m_GeometrySlotCount = 0;
        u32 m_GeometryBufferCapacity = 0;
        u32 m_FreeInstanceSlots = 0;
        u32 m_FreeGeometrySlots = 0;
        u32 m_RetiredInstanceSlots = 0;
        u32 m_RetiredGeometrySlots = 0;
        u32 m_BufferGrowthEvents = 0;
        u32 m_UnsupportedTotal = 0;
        u64 m_UploadBytes = 0;
        f64 m_ExtractionTimeMs = 0.0;
        std::array<u32, GPUSceneUnsupportedCategoryCount> m_UnsupportedCounts{};
    };

    struct GPUSceneFrameUpdate
    {
        std::vector<GPUSceneDirtyRange> m_InstanceDirtyRanges;
        std::vector<GPUSceneDirtyRange> m_GeometryDirtyRanges;
        GPUSceneFrameStats m_Stats;
    };

    struct GPUSceneGeometryKey
    {
        u64 m_VertexBuffer = 0;
        u64 m_IndexBuffer = 0;
        u32 m_SubmeshIndex = 0;

        [[nodiscard]] auto operator<=>(const GPUSceneGeometryKey&) const = default;
    };

    struct GPUSceneInstanceKey
    {
        u64 m_EntityId = 0;
        GPUSceneGeometryKey m_Geometry;
        u64 m_InstanceId = 0;

        [[nodiscard]] auto operator<=>(const GPUSceneInstanceKey&) const = default;
    };

    struct GPUSceneGeometryInput
    {
        RHI::ResourceHandle m_VertexBuffer;
        RHI::ResourceHandle m_IndexBuffer;
        u64 m_VertexAddress = 0;
        u64 m_IndexAddress = 0;
        u32 m_VertexFormat = 0;
        u32 m_IndexFormat = 0;
        u32 m_FirstIndex = 0;
        u32 m_IndexCount = 0;
        i32 m_BaseVertex = 0;
        u32 m_VertexCount = 0;
        u32 m_Flags = 0;

        [[nodiscard]] auto operator==(const GPUSceneGeometryInput&) const -> bool = default;
    };

    struct GPUSceneInstanceInput
    {
        glm::mat4 m_WorldTransform{ 1.0f };
        u32 m_MaterialIndex = std::numeric_limits<u32>::max();
        u32 m_VisibilityMask = std::numeric_limits<u32>::max();
        u32 m_Flags = 0;
    };
} // namespace OloEngine
