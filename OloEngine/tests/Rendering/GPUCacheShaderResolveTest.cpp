// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GPUCacheShaderResolveTest.cpp — the GPU-visible half of the paged-cache
// substrate (issue #704).
//
// Three contracts, each needing a real GL 4.6 device (SKIPs cleanly headless):
//
//  1. Acceptance criterion 2: a compute shader resolves an object's allocation
//     with NO CPU involvement. A GPUPagedCache in HostMirrored backing is
//     populated/evicted/cleared on the CPU; GPUCacheResolve_Probe.comp then
//     probes the mirrored hash table and walks the mirrored page-node chains
//     for a mixed batch of present / absent / erased keys, and every field it
//     reports — including an ORDER-SENSITIVE chain checksum — must equal the
//     CPU-side ground truth. The checksum matters: a set-style comparison
//     passes on a walk that visits the right pages in the wrong order
//     (docs/agent-rules/gpu-scan-compaction.md §2), and chain order IS the
//     data layout.
//
//  2. The DeviceMapped atom arena: AllocateObject's payload writes land in
//     device memory and read back bit-identical through the device path.
//
//  3. GPUCircularBuffer: staged copies survive wrap-around under fence range
//     locks (the substrate's persistent-mapped upload ring, which
//     VirtualMeshRegistry's page uploads now ride).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "GPUCacheInspector.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/GPUCache/GPUCachePolicy.h"
#include "OloEngine/Renderer/GPUCache/GPUCircularBuffer.h"
#include "OloEngine/Renderer/GPUCache/GPUPagedCache.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <string_view>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    using Cache = GPUPagedCache<u64, u32, LRUPolicy>;
    using Inspector = GPUPagedCacheInspector<Cache>;

    // The probe shader needs 64-bit integer arithmetic. Ask the driver BEFORE
    // creating the shader: a failed compute compile is a modal assert dialog
    // in Debug, not a return value (gpu-scan-compaction.md §6).
    bool DriverSupportsInt64()
    {
        GLint extensionCount = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
        for (GLint i = 0; i < extensionCount; ++i)
        {
            const auto* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (name != nullptr && std::string_view(name) == "GL_ARB_gpu_shader_int64")
            {
                return true;
            }
        }
        return false;
    }

    // CPU mirror of the probe shader's binding-2 block. The static_asserts pin
    // the std430 contract GPUCacheResolve.glsl documents — if the Entry layout
    // drifts (a policy with a differently-sized Handle, a reordered field),
    // they fail the BUILD instead of the dispatch silently misreading memory.
    inline constexpr u32 kMaxQueries = 256;

    struct GpuResolveResult
    {
        u32 m_Found;
        u32 m_TotalElementCount;
        u32 m_StartPage;
        u32 m_EndPage;
        u32 m_PageCount;
        u32 m_LastPage;
        u32 m_ChainChecksum;
        u32 m_Pad;
    };

    struct ResolveIoBlock
    {
        u32 m_QueryCount;
        u32 m_Pad0;
        u32 m_Pad1;
        u32 m_Pad2;
        u64 m_QueryKeys[kMaxQueries];
        GpuResolveResult m_Results[kMaxQueries];
    };

    static_assert(sizeof(GpuResolveResult) == 32);
    static_assert(sizeof(ResolveIoBlock) == 16 + kMaxQueries * 8 + kMaxQueries * 32);
    static_assert(offsetof(ResolveIoBlock, m_QueryKeys) == 16);
    static_assert(offsetof(ResolveIoBlock, m_Results) == 16 + kMaxQueries * 8);
    static_assert(sizeof(Cache::ObjectAllocation) == 16,
                  "the GLSL OloGpuCacheEntry mirror assumes a 4-byte LRU policy handle");
    static_assert(sizeof(GPUHashMap<u64, Cache::ObjectAllocation>::Entry) == 24,
                  "the GLSL OloGpuCacheEntry mirror assumes an 8-byte key + 16-byte allocation");

    // The CPU-side ground truth the GPU must reproduce, computed through the
    // cache's public lookup + the inspector's chain walk.
    GpuResolveResult CpuResolve(Cache& cache, u64 key)
    {
        GpuResolveResult expected{};
        expected.m_StartPage = Cache::kInvalidPage;
        expected.m_EndPage = Cache::kInvalidPage;
        expected.m_LastPage = Cache::kInvalidPage;

        Cache::ObjectAllocation alloc;
        if (!cache.Find(key, alloc))
        {
            return expected;
        }
        expected.m_Found = 1;
        expected.m_TotalElementCount = alloc.m_TotalElementCount;
        expected.m_StartPage = alloc.m_StartPage;
        expected.m_EndPage = alloc.m_EndPage;
        for (const u32 page : Inspector::ChainPages(cache, key))
        {
            expected.m_ChainChecksum = expected.m_ChainChecksum * 33u + page;
            expected.m_LastPage = page;
            ++expected.m_PageCount;
        }
        return expected;
    }
} // namespace

TEST(GPUCacheShaderResolveTest, ComputeShaderResolvesAllocationsWithNoCpuRoundTrip)
{
    OLO_ENSURE_GPU_OR_SKIP();
    if (!DriverSupportsInt64())
    {
        GTEST_SKIP() << "GL_ARB_gpu_shader_int64 not supported by this driver";
    }

    Cache cache;
    ASSERT_TRUE(cache.Create(4, 32, GPUCacheBacking::HostMirrored))
        << "HostMirrored backing failed on a live device";

    // A population with every directory state the shader can meet: single- and
    // multi-page objects, an out-of-address-order chain (built below by reusing
    // freed pages), a cleared-but-present object, and a tombstone from an
    // explicit erase. Note this deliberately stays well inside the 32-page
    // capacity, so no policy eviction occurs — eviction's effect on the
    // directory is covered headless by GPUPagedCacheTest.
    const std::vector<u32> small = { 1, 2, 3 };
    const std::vector<u32> large(11, 7); // 3 pages
    ASSERT_TRUE(cache.AllocateObject(100, small.data(), small.size()));
    ASSERT_TRUE(cache.AllocateObject(200, large.data(), large.size()));
    ASSERT_TRUE(cache.AllocateObject(300, small.data(), small.size()));
    ASSERT_TRUE(cache.AllocateObject(400, large.data(), large.size()));
    cache.ClearObject(300);      // present, zero elements, chain kept
    cache.DeallocateObject(100); // tombstone + frees a page for the reuse below
    // Reuse the freed pages out of order so at least one chain is not
    // address-ascending — the order-sensitive checksum has to prove the GPU
    // walks CHAIN order, not index order.
    const std::vector<u32> reuse(6, 9); // 2 pages from the freed/fragmented set
    ASSERT_TRUE(cache.AllocateObject(500, reuse.data(), reuse.size()));

    std::vector<u64> queries;
    for (const u64 key : { 100ull, 200ull, 300ull, 400ull, 500ull }) // live + tombstoned
    {
        queries.push_back(key);
    }
    for (u64 key = 9000; key < 9040; ++key) // never inserted
    {
        queries.push_back(key);
    }
    ASSERT_LE(queries.size(), static_cast<sizet>(kMaxQueries));

    auto io = std::make_unique<ResolveIoBlock>();
    std::memset(io.get(), 0, sizeof(ResolveIoBlock));
    io->m_QueryCount = static_cast<u32>(queries.size());
    std::memcpy(io->m_QueryKeys, queries.data(), queries.size() * sizeof(u64));

    auto ioBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(ResolveIoBlock)), 2,
                                          StorageBufferUsage::DynamicCopy);
    ioBuffer->SetData(io.get(), static_cast<u32>(sizeof(ResolveIoBlock)), 0);

    auto probe = ComputeShader::Create("assets/shaders/tests/GPUCacheResolve_Probe.comp");
    ASSERT_TRUE(probe && probe->IsValid()) << "GPUCacheResolve_Probe.comp failed to compile";

    probe->Bind();
    cache.BindLookup(/*hashMapBinding=*/0, /*pageNodesBinding=*/1);
    ioBuffer->Bind();
    const u32 groups = (io->m_QueryCount + 63u) / 64u;
    RenderCommand::DispatchCompute(groups, 1, 1);
    RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

    auto readback = std::make_unique<ResolveIoBlock>();
    ioBuffer->GetData(readback.get(), static_cast<u32>(sizeof(ResolveIoBlock)), 0);

    for (sizet i = 0; i < queries.size(); ++i)
    {
        const GpuResolveResult& actual = readback->m_Results[i];
        const GpuResolveResult expected = CpuResolve(cache, queries[i]);
        EXPECT_EQ(actual.m_Found, expected.m_Found) << "key " << queries[i];
        EXPECT_EQ(actual.m_TotalElementCount, expected.m_TotalElementCount) << "key " << queries[i];
        EXPECT_EQ(actual.m_StartPage, expected.m_StartPage) << "key " << queries[i];
        EXPECT_EQ(actual.m_EndPage, expected.m_EndPage) << "key " << queries[i];
        EXPECT_EQ(actual.m_PageCount, expected.m_PageCount) << "key " << queries[i];
        EXPECT_EQ(actual.m_LastPage, expected.m_LastPage) << "key " << queries[i];
        EXPECT_EQ(actual.m_ChainChecksum, expected.m_ChainChecksum)
            << "key " << queries[i] << " — the GPU walked a different page ORDER than the CPU chain";
    }

    // At least one queried chain must be genuinely out of address order, or
    // the checksum assertion above proved nothing about walk order.
    bool sawNonAscendingChain = false;
    for (const u64 key : { 200ull, 300ull, 400ull, 500ull })
    {
        const std::vector<u32> chain = Inspector::ChainPages(cache, key);
        for (sizet i = 1; i < chain.size(); ++i)
        {
            sawNonAscendingChain = sawNonAscendingChain || chain[i] != chain[i - 1] + 1;
        }
    }
    EXPECT_TRUE(sawNonAscendingChain) << "test population never produced a fragmented chain — "
                                         "the order-sensitive checksum was exercised vacuously";

    // GL hygiene (§6.4): leave no SSBO of ours on a shared binding point.
    ioBuffer->Unbind();
    probe->Unbind();
    cache.Destroy(); // deleting the buffers detaches them from bindings 0/1
}

TEST(GPUCacheShaderResolveTest, DeviceArenaRoundTripsObjectData)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Cache cache;
    ASSERT_TRUE(cache.Create(8, 16, GPUCacheBacking::HostMirrored));

    const std::vector<u32> data = { 0xDEAD0001u, 0xDEAD0002u, 0xDEAD0003u, 0xDEAD0004u, 0xDEAD0005u,
                                    0xDEAD0006u, 0xDEAD0007u, 0xDEAD0008u, 0xDEAD0009u, 0xDEAD000Au };
    ASSERT_TRUE(cache.AllocateObject(7, data.data(), data.size()));

    // The mapped arena is write-only for the CPU; the inspector reads back
    // through the device, so equality proves the payload actually landed in
    // the device buffer — not in some host shadow.
    EXPECT_EQ(Inspector::ReadObjectData(cache, 7), data);

    cache.Destroy();
}

TEST(GPUCacheShaderResolveTest, CircularBufferCopiesSurviveWrapUnderFences)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u64 kRingBytes = 64;
    constexpr u32 kChunkBytes = 16;
    constexpr u32 kChunkCount = 32; // 8x the ring: forces 7 wrap-arounds

    GPUCircularBuffer ring;
    ASSERT_TRUE(ring.Create(kRingBytes));

    auto destination = StorageBuffer::Create(kChunkCount * kChunkBytes, 3, StorageBufferUsage::DynamicCopy);

    for (u32 chunk = 0; chunk < kChunkCount; ++chunk)
    {
        u32 payload[kChunkBytes / sizeof(u32)];
        for (u32 word = 0; word < std::size(payload); ++word)
        {
            payload[word] = chunk * 1000u + word;
        }

        u64 ringOffset = 0;
        u8* dst = ring.Reserve(kChunkBytes, ringOffset);
        ASSERT_NE(dst, nullptr);
        std::memcpy(dst, payload, kChunkBytes);
        RenderCommand::CopyBufferSubData(ring.GetDeviceHandle(), destination->GetRHIHandle(), ringOffset,
                                         static_cast<u64>(chunk) * kChunkBytes, kChunkBytes);
        ring.Commit(kChunkBytes);
    }

    std::vector<u32> readback(kChunkCount * kChunkBytes / sizeof(u32));
    destination->GetData(readback.data(), static_cast<u32>(readback.size() * sizeof(u32)), 0);

    for (u32 chunk = 0; chunk < kChunkCount; ++chunk)
    {
        for (u32 word = 0; word < kChunkBytes / sizeof(u32); ++word)
        {
            ASSERT_EQ(readback[chunk * (kChunkBytes / sizeof(u32)) + word], chunk * 1000u + word)
                << "chunk " << chunk << " word " << word
                << " — a wrap overwrote bytes an in-flight copy still needed";
        }
    }

    destination->Unbind();
    ring.Destroy();
}
