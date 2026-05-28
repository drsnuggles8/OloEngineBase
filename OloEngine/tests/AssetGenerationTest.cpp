#include <gtest/gtest.h>
#include "OloEngine/Asset/AssetManager/AssetManagerBase.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace OloEngine;

// Minimal concrete subclass to test non-virtual generation counter methods
class StubAssetManager final : public AssetManagerBase
{
  public:
    // Expose protected method for testing
    using AssetManagerBase::IncrementAssetGeneration;

    void Shutdown() noexcept override {}
    AssetType GetAssetType(AssetHandle) const noexcept override
    {
        return AssetType::None;
    }
    Ref<Asset> GetAsset(AssetHandle) override
    {
        return nullptr;
    }
    AsyncAssetResult<Asset> GetAssetAsync(AssetHandle) override
    {
        return {};
    }
    AssetMetadata GetAssetMetadata(AssetHandle) const noexcept override
    {
        return {};
    }
    void AddMemoryOnlyAsset(Ref<Asset>) override {}
    [[nodiscard]] bool ReloadData(AssetHandle) override
    {
        return false;
    }
    void ReloadDataAsync(AssetHandle) override {}
    [[nodiscard]] bool EnsureCurrent(AssetHandle) override
    {
        return true;
    }
    [[nodiscard]] bool EnsureAllLoadedCurrent() override
    {
        return true;
    }
    [[nodiscard]] bool IsAssetHandleValid(AssetHandle) const noexcept override
    {
        return false;
    }
    Ref<Asset> GetMemoryAsset(AssetHandle) const override
    {
        return nullptr;
    }
    [[nodiscard]] bool IsAssetLoaded(AssetHandle) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool IsAssetValid(AssetHandle) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool IsAssetMissing(AssetHandle) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool IsMemoryAsset(AssetHandle) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool IsPhysicalAsset(AssetHandle) const noexcept override
    {
        return false;
    }
    void RemoveAsset(AssetHandle) override {}
    void RegisterDependency(AssetHandle, AssetHandle) override {}
    void DeregisterDependency(AssetHandle, AssetHandle) override {}
    void DeregisterDependencies(AssetHandle) override {}
    std::unordered_set<AssetHandle> GetDependencies(AssetHandle) const override
    {
        return {};
    }
    void SyncWithAssetThread() noexcept override {}
    std::unordered_set<AssetHandle> GetAllAssetsWithType(AssetType) const override
    {
        return {};
    }
    [[nodiscard]] std::unordered_map<AssetHandle, Ref<Asset>> GetLoadedAssets() const override
    {
        return {};
    }
    void ForEachLoadedAsset(const std::function<bool(AssetHandle, const Ref<Asset>&)>&) const override {}
};

class AssetGenerationTest : public ::testing::Test
{
  protected:
    StubAssetManager mgr;
};

// --- Generation counter tests ---

TEST_F(AssetGenerationTest, UnknownHandleReturnsZero)
{
    AssetHandle h{ 42 };
    EXPECT_EQ(mgr.GetAssetGeneration(h), 0u);
}

TEST_F(AssetGenerationTest, IncrementFromZero)
{
    AssetHandle h{ 100 };
    EXPECT_EQ(mgr.GetAssetGeneration(h), 0u);
    mgr.IncrementAssetGeneration(h);
    EXPECT_EQ(mgr.GetAssetGeneration(h), 1u);
}

TEST_F(AssetGenerationTest, MultipleIncrements)
{
    AssetHandle h{ 200 };
    mgr.IncrementAssetGeneration(h);
    mgr.IncrementAssetGeneration(h);
    mgr.IncrementAssetGeneration(h);
    EXPECT_EQ(mgr.GetAssetGeneration(h), 3u);
}

TEST_F(AssetGenerationTest, IndependentHandles)
{
    AssetHandle a{ 300 };
    AssetHandle b{ 301 };
    mgr.IncrementAssetGeneration(a);
    mgr.IncrementAssetGeneration(a);
    mgr.IncrementAssetGeneration(b);
    EXPECT_EQ(mgr.GetAssetGeneration(a), 2u);
    EXPECT_EQ(mgr.GetAssetGeneration(b), 1u);
}

TEST_F(AssetGenerationTest, ZeroHandleTracked)
{
    AssetHandle h{ 0 };
    mgr.IncrementAssetGeneration(h);
    EXPECT_EQ(mgr.GetAssetGeneration(h), 1u);
}

// --- Concurrency tests (exercise the FSharedMutex backing the counter) ---

// Many writers hammering the same handle must not lose updates: the exclusive
// lock in IncrementAssetGeneration has to serialise the read-modify-write.
TEST_F(AssetGenerationTest, ConcurrentIncrementsSameHandleNoLostUpdates)
{
    AssetHandle h{ 1000 };
    constexpr int threadCount = 8;
    constexpr int incrementsPerThread = 5000;

    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (int t = 0; t < threadCount; ++t)
    {
        threads.emplace_back([this, h]
                             {
            for (int i = 0; i < incrementsPerThread; ++i)
            {
                mgr.IncrementAssetGeneration(h);
            } });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    EXPECT_EQ(mgr.GetAssetGeneration(h), static_cast<u32>(threadCount * incrementsPerThread));
}

// Writers touching distinct handles must each land their full count, with no
// cross-handle interference even under heavy contention on the shared map.
TEST_F(AssetGenerationTest, ConcurrentIncrementsDistinctHandles)
{
    constexpr int threadCount = 8;
    constexpr int incrementsPerThread = 5000;

    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (int t = 0; t < threadCount; ++t)
    {
        threads.emplace_back([this, t]
                             {
            AssetHandle h{ static_cast<u64>(2000 + t) };
            for (int i = 0; i < incrementsPerThread; ++i)
            {
                mgr.IncrementAssetGeneration(h);
            } });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    for (int t = 0; t < threadCount; ++t)
    {
        AssetHandle h{ static_cast<u64>(2000 + t) };
        EXPECT_EQ(mgr.GetAssetGeneration(h), static_cast<u32>(incrementsPerThread));
    }
}

// Concurrent shared (read) locks must coexist with exclusive (write) locks
// without deadlock or data race; readers only ever observe a monotonically
// non-decreasing value bounded by the final count.
TEST_F(AssetGenerationTest, ConcurrentReadersAndWriter)
{
    AssetHandle h{ 3000 };
    constexpr int writes = 20000;
    constexpr int readerCount = 4;
    std::atomic<bool> done{ false };
    std::atomic<int> readyCount{ 0 };

    std::vector<std::thread> readers;
    readers.reserve(readerCount);
    for (int r = 0; r < readerCount; ++r)
    {
        readers.emplace_back([this, h, &done, &readyCount]
                             {
            // Signal readiness, then spin until every reader is live so reads are
            // guaranteed to overlap the writer loop below.
            readyCount.fetch_add(1, std::memory_order_relaxed);
            while (readyCount.load(std::memory_order_relaxed) < readerCount)
            {
                std::this_thread::yield();
            }

            u32 last = 0;
            while (!done.load(std::memory_order_relaxed))
            {
                u32 gen = mgr.GetAssetGeneration(h);
                EXPECT_GE(gen, last);
                EXPECT_LE(gen, static_cast<u32>(writes));
                last = gen;
            } });
    }

    // Don't start writing until all readers are running.
    while (readyCount.load(std::memory_order_relaxed) < readerCount)
    {
        std::this_thread::yield();
    }

    for (int i = 0; i < writes; ++i)
    {
        mgr.IncrementAssetGeneration(h);
    }
    done.store(true, std::memory_order_relaxed);

    for (auto& th : readers)
    {
        th.join();
    }

    EXPECT_EQ(mgr.GetAssetGeneration(h), static_cast<u32>(writes));
}
