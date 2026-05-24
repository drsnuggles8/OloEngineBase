#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Physics3D/MeshCookingFactory.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

using namespace OloEngine;

namespace
{
    // Picks a freshly-created, never-reused scratch directory under the system
    // temp area. The base parent stays as `temp/OloEngineTests/<tag>` for easy
    // post-mortem cleanup; each invocation appends a per-process timestamp + a
    // monotonically-increasing counter so concurrent `OloEngine-Tests.exe`
    // instances cannot delete or overwrite each other's working state.
    std::filesystem::path MakeUniqueScratchDir(const char* tag)
    {
        static std::atomic<u64> s_Counter{ 0 };
        const auto base = std::filesystem::temp_directory_path() / "OloEngineTests" / tag;

        const auto nanos = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto seq = s_Counter.fetch_add(1, std::memory_order_relaxed);

        std::ostringstream suffix;
        suffix << std::hex << nanos << "_" << seq;

        const auto unique = base / suffix.str();
        std::filesystem::create_directories(unique);
        return unique;
    }

    void TouchFile(const std::filesystem::path& path)
    {
        std::ofstream{ path, std::ios::binary } << "x";
    }

    void SetMtime(const std::filesystem::path& path, std::filesystem::file_time_type t)
    {
        std::filesystem::last_write_time(path, t);
    }
} // namespace

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
