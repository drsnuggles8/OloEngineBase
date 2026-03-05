#include <gtest/gtest.h>
#include "OloEngine/Asset/AssetManager/AssetManagerBase.h"

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
