#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Test that the StorageBuffer and ComputeShader interfaces compile and have correct API surface.
// No OpenGL context is available in unit tests, so we only verify the type hierarchy and
// static API (no Create() calls that would require a live GL context).

#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Core/Ref.h"

using namespace OloEngine;

// ---------------------------------------------------------------------------
// StorageBuffer interface checks
// ---------------------------------------------------------------------------

TEST(StorageBufferTest, StorageBufferIsRefCounted)
{
    // Verify StorageBuffer inherits from RefCounted so Ref<StorageBuffer> compiles
    static_assert(std::is_base_of_v<RefCounted, StorageBuffer>, "StorageBuffer must be RefCounted");
    SUCCEED();
}

TEST(StorageBufferTest, StorageBufferHasPureVirtualAPI)
{
    // Verify the class is abstract (cannot be instantiated)
    static_assert(std::is_abstract_v<StorageBuffer>, "StorageBuffer must be abstract");
    SUCCEED();
}

// ---------------------------------------------------------------------------
// ComputeShader interface checks
// ---------------------------------------------------------------------------

TEST(ComputeShaderTest, ComputeShaderIsAbstract)
{
    static_assert(std::is_abstract_v<ComputeShader>, "ComputeShader must be abstract");
    SUCCEED();
}

TEST(ComputeShaderTest, ComputeShaderInheritsRendererResource)
{
    static_assert(std::is_base_of_v<RendererResource, ComputeShader>,
                  "ComputeShader must inherit from RendererResource");
    SUCCEED();
}

TEST(ComputeShaderTest, ComputeShaderAssetType)
{
    EXPECT_EQ(ComputeShader::GetStaticType(), AssetType::ComputeShader);
}

// ---------------------------------------------------------------------------
// MemoryBarrierFlags checks
// ---------------------------------------------------------------------------

TEST(MemoryBarrierFlagsTest, FlagCombination)
{
    auto combined = MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate;
    EXPECT_NE(static_cast<u32>(combined), 0u);
    EXPECT_EQ(static_cast<u32>(combined & MemoryBarrierFlags::ShaderStorage),
              static_cast<u32>(MemoryBarrierFlags::ShaderStorage));
}
