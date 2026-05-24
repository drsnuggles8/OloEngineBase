#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Physics3D/MeshCookingFactory.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

using namespace OloEngine;

namespace
{
    // Picks a unique scratch directory under the system temp area so two
    // test runs in parallel (or a leaked previous run) cannot collide.
    std::filesystem::path MakeUniqueScratchDir(const char* tag)
    {
        const auto base = std::filesystem::temp_directory_path() / "OloEngineTests" / tag;
        std::error_code ec;
        std::filesystem::remove_all(base, ec); // best-effort cleanup of leftovers
        std::filesystem::create_directories(base);
        return base;
    }

    void TouchFile(const std::filesystem::path& path)
    {
        std::ofstream{ path, std::ios::binary } << "x";
    }

    void SetMtime(const std::filesystem::path& path, std::filesystem::file_time_type t)
    {
        std::filesystem::last_write_time(path, t);
    }
}

// IsCacheValid is the boundary the cooker uses to decide whether to reuse a
// previously cooked .omc file. Wiring it into CookMeshType means that any bug
// in IsCacheValid will silently leak stale physics colliders into running
// scenes — so the boundary is worth pinning down with direct tests.
TEST(MeshCookingFactoryCache, ReturnsTrueWhenCacheNewerThanSource)
{
    const auto dir = MakeUniqueScratchDir("CacheNewer");
    const auto sourcePath = dir / "source.glb";
    const auto cachePath = dir / "cache.omc";

    TouchFile(sourcePath);
    TouchFile(cachePath);

    // Force the source to be older than the cache by exactly two seconds — the
    // filesystem's mtime resolution is comfortably below that on all supported
    // platforms.
    const auto now = std::filesystem::file_time_type::clock::now();
    SetMtime(sourcePath, now - std::chrono::seconds(2));
    SetMtime(cachePath, now);

    MeshCookingFactory factory(dir);
    EXPECT_TRUE(factory.IsCacheValid(cachePath, sourcePath));
}

TEST(MeshCookingFactoryCache, ReturnsFalseWhenSourceNewerThanCache)
{
    const auto dir = MakeUniqueScratchDir("CacheStale");
    const auto sourcePath = dir / "source.glb";
    const auto cachePath = dir / "cache.omc";

    TouchFile(sourcePath);
    TouchFile(cachePath);

    // Cache was written, then the source was modified after — the canonical
    // stale-cache shape we want to detect.
    const auto now = std::filesystem::file_time_type::clock::now();
    SetMtime(cachePath, now - std::chrono::seconds(2));
    SetMtime(sourcePath, now);

    MeshCookingFactory factory(dir);
    EXPECT_FALSE(factory.IsCacheValid(cachePath, sourcePath));
}

TEST(MeshCookingFactoryCache, ReturnsFalseWhenCacheMissing)
{
    const auto dir = MakeUniqueScratchDir("CacheMissing");
    const auto sourcePath = dir / "source.glb";
    const auto cachePath = dir / "cache.omc";

    TouchFile(sourcePath);
    // cache file intentionally not created

    MeshCookingFactory factory(dir);
    EXPECT_FALSE(factory.IsCacheValid(cachePath, sourcePath));
}

TEST(MeshCookingFactoryCache, ReturnsFalseWhenSourceMissing)
{
    const auto dir = MakeUniqueScratchDir("SourceMissing");
    const auto sourcePath = dir / "source.glb";
    const auto cachePath = dir / "cache.omc";

    TouchFile(cachePath);
    // source file intentionally not created — we want IsCacheValid to reject
    // this rather than report the cache as valid, since the caller has no way
    // to verify the source was unchanged.

    MeshCookingFactory factory(dir);
    EXPECT_FALSE(factory.IsCacheValid(cachePath, sourcePath));
}

TEST(MeshCookingFactoryCache, ReturnsTrueWhenTimestampsAreEqual)
{
    const auto dir = MakeUniqueScratchDir("CacheEqual");
    const auto sourcePath = dir / "source.glb";
    const auto cachePath = dir / "cache.omc";

    TouchFile(sourcePath);
    TouchFile(cachePath);

    const auto t = std::filesystem::file_time_type::clock::now();
    SetMtime(sourcePath, t);
    SetMtime(cachePath, t);

    // The cooker's contract is `cacheTime >= sourceTime`: equal timestamps mean
    // the cache is still authoritative.
    MeshCookingFactory factory(dir);
    EXPECT_TRUE(factory.IsCacheValid(cachePath, sourcePath));
}
