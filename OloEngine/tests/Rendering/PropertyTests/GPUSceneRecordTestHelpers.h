#pragma once

// Shared helpers for the GPU-scene record property tests (issues #992,
// #993): GPUSceneMaterialRecordPropertyTests.cpp,
// GPUSceneLightRecordPropertyTests.cpp and
// GPUSceneEnvironmentAndLifecyclePropertyTests.cpp. Not a test file.

#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests::GPUSceneRecordTesting
{
    inline constexpr u32 kImported = std::to_underlying(GPUSceneMaterialSource::Imported);
    inline constexpr u32 kEntityOverride = std::to_underlying(GPUSceneMaterialSource::EntityOverride);
    inline constexpr u32 kDirectional = std::to_underlying(GPUSceneLightType::Directional);
    inline constexpr u32 kPoint = std::to_underlying(GPUSceneLightType::Point);
    inline constexpr u32 kSpot = std::to_underlying(GPUSceneLightType::Spot);
    inline constexpr u32 kSphereArea = std::to_underlying(GPUSceneLightType::SphereArea);

    [[nodiscard]] inline GPUSceneMaterialKey ImportedMaterial(u64 owner, u32 slot = 0)
    {
        return GPUSceneMaterialKey{ .m_Owner = owner, .m_Slot = slot, .m_Source = kImported };
    }

    [[nodiscard]] inline GPUSceneTextureRef Texture(u32 index, u32 generation, u32 heapOffset)
    {
        return GPUSceneTextureRef{ .m_Handle = RHI::ResourceHandle{ index, generation },
                                   .m_HeapOffset = heapOffset };
    }

    [[nodiscard]] inline std::string Describe(const std::vector<GPUSceneDirtyRange>& ranges)
    {
        std::string text = "{";
        for (const GPUSceneDirtyRange& range : ranges)
        {
            text += " [" + std::to_string(range.m_FirstIndex) + ", +" + std::to_string(range.m_Count) + ")";
        }
        return text + " }";
    }

    [[nodiscard]] inline ::testing::AssertionResult DirtyRangesAre(const std::vector<GPUSceneDirtyRange>& actual,
                                                                   std::initializer_list<GPUSceneDirtyRange> expected)
    {
        const std::vector<GPUSceneDirtyRange> wanted(expected);
        if (actual == wanted)
        {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
               << "dirty ranges " << Describe(actual) << ", expected " << Describe(wanted);
    }

    [[nodiscard]] inline ::testing::AssertionResult SameVec4(const glm::vec4& actual, const glm::vec4& expected)
    {
        if (Math::BitwiseEqual(actual, expected))
        {
            return ::testing::AssertionSuccess();
        }
        return ::testing::AssertionFailure()
               << "(" << actual.x << ", " << actual.y << ", " << actual.z << ", " << actual.w << ") != ("
               << expected.x << ", " << expected.y << ", " << expected.z << ", " << expected.w << ")";
    }

    // The geometry every instance in these files references. Its own rules
    // live in GPUScenePropertyTests.cpp.
    inline const GPUSceneGeometryKey kGeometryKey{ .m_VertexBuffer = 11, .m_IndexBuffer = 12 };
    inline const GPUSceneGeometryInput kGeometry{ .m_VertexBuffer = RHI::ResourceHandle{ 1, 1 },
                                                  .m_IndexBuffer = RHI::ResourceHandle{ 2, 1 },
                                                  .m_IndexCount = 3,
                                                  .m_VertexCount = 3 };
} // namespace OloEngine::Tests::GPUSceneRecordTesting
