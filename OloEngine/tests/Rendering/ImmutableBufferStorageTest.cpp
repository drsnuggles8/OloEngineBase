#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// ImmutableBufferStorageTest — GPU contract test (skips cleanly without GL 4.5+).
//
// Pins what VertexBuffer::CreateImmutable / IndexBuffer::CreateImmutable mint on
// the GL backend: immutable storage with NO storage flags, holding the data it
// was given (#1608).
//
// The flags are the point. Create(data, size) is mutable storage
// (glNamedBufferData), and the NVIDIA driver keeps a resident system-memory copy
// of every mutable buffer: the Bernese coat's 1.5 GB groom rest stream cost
// 1.5 GB of the test process's working set on top of its VRAM, and the breed
// evidence suite crossed the 6 GB per-process ceiling on it. Measured on the
// same stream, even GL_DYNAMIC_STORAGE_BIT brings the copy back, so "immutable"
// here means flags 0, and a regression to either form shows up below.
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <glad/gl.h>

#include <array>
#include <cstring>

// OLO_TEST_LAYER: L1

namespace
{
    using namespace OloEngine;

    struct StorageFacts
    {
        GLint Immutable = GL_FALSE;
        GLint Flags = -1;
        GLint64 Size = 0;
    };

    [[nodiscard]] StorageFacts StorageOf(u32 buffer)
    {
        StorageFacts facts;
        ::glGetNamedBufferParameteriv(buffer, GL_BUFFER_IMMUTABLE_STORAGE, &facts.Immutable);
        ::glGetNamedBufferParameteriv(buffer, GL_BUFFER_STORAGE_FLAGS, &facts.Flags);
        ::glGetNamedBufferParameteri64v(buffer, GL_BUFFER_SIZE, &facts.Size);
        return facts;
    }

    constexpr std::array<f32, 8> kVertices{ 0.5f, -1.0f, 2.0f, 3.25f, -4.0f, 5.5f, 6.0f, 7.75f };
    constexpr std::array<u32, 6> kIndices{ 0u, 1u, 2u, 2u, 3u, 0u };
} // namespace

TEST(ImmutableBufferStorage, CreateImmutableVertexBufferHasNoStorageFlagsAndKeepsItsData)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const auto vertex = VertexBuffer::CreateImmutable(kVertices.data(), static_cast<u32>(sizeof(kVertices)));
    ASSERT_TRUE(vertex);
    const StorageFacts facts = StorageOf(vertex->GetBufferHandle());
    EXPECT_EQ(facts.Immutable, GL_TRUE) << "mutable storage keeps a resident system-memory copy (#1608)";
    EXPECT_EQ(facts.Flags, 0) << "any storage flag (even GL_DYNAMIC_STORAGE_BIT) brings the copy back";
    EXPECT_EQ(facts.Size, static_cast<GLint64>(sizeof(kVertices)));

    std::array<f32, kVertices.size()> read{};
    ::glGetNamedBufferSubData(vertex->GetBufferHandle(), 0, sizeof(read), read.data());
    EXPECT_EQ(std::memcmp(read.data(), kVertices.data(), sizeof(kVertices)), 0);
}

TEST(ImmutableBufferStorage, CreateImmutableIndexBufferHasNoStorageFlagsAndKeepsItsData)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const auto index = IndexBuffer::CreateImmutable(kIndices.data(), static_cast<u32>(kIndices.size()));
    ASSERT_TRUE(index);
    EXPECT_EQ(index->GetCount(), static_cast<u32>(kIndices.size()));
    const StorageFacts facts = StorageOf(index->GetBufferHandle());
    EXPECT_EQ(facts.Immutable, GL_TRUE);
    EXPECT_EQ(facts.Flags, 0);
    EXPECT_EQ(facts.Size, static_cast<GLint64>(sizeof(kIndices)));

    std::array<u32, kIndices.size()> read{};
    ::glGetNamedBufferSubData(index->GetBufferHandle(), 0, sizeof(read), read.data());
    EXPECT_EQ(read, kIndices);
}

// THE CONTROL: Create(data, size) is still the mutable, refillable buffer every
// other caller relies on. If this flipped, a SetData caller would be refused.
TEST(ImmutableBufferStorage, CreateWithDataStaysMutableAndRefillable)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto vertex = VertexBuffer::Create(static_cast<const void*>(kVertices.data()), static_cast<u32>(sizeof(kVertices)));
    ASSERT_TRUE(vertex);
    EXPECT_EQ(StorageOf(vertex->GetBufferHandle()).Immutable, GL_FALSE);

    std::array<f32, kVertices.size()> refill{};
    refill.fill(9.0f);
    vertex->SetData({ refill.data(), static_cast<u32>(sizeof(refill)) });
    std::array<f32, kVertices.size()> read{};
    ::glGetNamedBufferSubData(vertex->GetBufferHandle(), 0, sizeof(read), read.data());
    EXPECT_EQ(std::memcmp(read.data(), refill.data(), sizeof(refill)), 0) << "SetData did not refill the buffer";
}
