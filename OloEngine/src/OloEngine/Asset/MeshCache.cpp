#include "OloEnginePCH.h"
#include "MeshCache.h"
#include "OloEngine/Serialization/MeshBinarySerializer.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Animation/AnimationClip.h"

#include <fstream>

namespace OloEngine
{
    namespace
    {
        constexpr const char* kMeshCacheDirectory = "assets/cache/mesh";
        constexpr const char* kAnimationCacheDirectory = "assets/cache/animation";

        void EnsureDirectoryExists(const std::filesystem::path& dir)
        {
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec))
            {
                std::filesystem::create_directories(dir, ec);
                if (ec)
                {
                    OLO_CORE_ERROR("MeshCache: Failed to create directory '{}': {}", dir.string(), ec.message());
                }
            }
        }

        // Hash a source path to produce a deterministic cache filename.
        // Uses FNV-1a 64-bit for cross-platform stability (std::hash is
        // implementation-defined and may differ across compilers/platforms).
        std::string HashSourcePath(const std::filesystem::path& sourcePath)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(sourcePath, ec);
            auto pathStr = ec ? sourcePath.generic_string() : canonical.generic_string();

            constexpr u64 FNV_OFFSET_BASIS = 14695981039346656037ULL;
            constexpr u64 FNV_PRIME = 1099511628211ULL;

            u64 hash = FNV_OFFSET_BASIS;
            for (char c : pathStr)
            {
                hash ^= static_cast<u64>(static_cast<unsigned char>(c));
                hash *= FNV_PRIME;
            }

            return fmt::format("{:016X}", hash);
        }

        // Get source file's last-write-time as a u64 for timestamp comparison.
        u64 GetSourceTimestamp(const std::filesystem::path& sourcePath)
        {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(sourcePath, ec);
            if (ec)
            {
                return 0;
            }
            return static_cast<u64>(ftime.time_since_epoch().count());
        }
    } // anonymous namespace

    namespace MeshCache
    {
        std::filesystem::path GetCacheDirectory()
        {
            std::filesystem::path dir(kMeshCacheDirectory);
            EnsureDirectoryExists(dir);
            return dir;
        }

        std::filesystem::path GetAnimationCacheDirectory()
        {
            std::filesystem::path dir(kAnimationCacheDirectory);
            EnsureDirectoryExists(dir);
            return dir;
        }

        std::filesystem::path GetMeshCachePath(const std::filesystem::path& sourcePath)
        {
            return GetCacheDirectory() / (HashSourcePath(sourcePath) + ".omesh");
        }

        std::filesystem::path GetAnimationCachePath(const std::filesystem::path& sourcePath)
        {
            return GetAnimationCacheDirectory() / (HashSourcePath(sourcePath) + ".oanim");
        }

        bool IsMeshCacheValid(const std::filesystem::path& sourcePath)
        {
            auto cachePath = GetMeshCachePath(sourcePath);

            std::error_code ec;
            if (!std::filesystem::exists(cachePath, ec))
            {
                return false;
            }

            u64 cachedTimestamp = 0;
            if (!MeshBinarySerializer::ReadTimestamp(cachePath, cachedTimestamp))
            {
                return false;
            }

            u64 sourceTimestamp = GetSourceTimestamp(sourcePath);
            return (sourceTimestamp != 0) && (cachedTimestamp == sourceTimestamp);
        }

        bool IsAnimationCacheValid(const std::filesystem::path& sourcePath)
        {
            auto cachePath = GetAnimationCachePath(sourcePath);

            std::error_code ec;
            if (!std::filesystem::exists(cachePath, ec))
            {
                return false;
            }

            u64 cachedTimestamp = 0;
            if (!AnimationBinarySerializer::ReadTimestamp(cachePath, cachedTimestamp))
            {
                return false;
            }

            u64 sourceTimestamp = GetSourceTimestamp(sourcePath);
            return (sourceTimestamp != 0) && (cachedTimestamp == sourceTimestamp);
        }

        Ref<MeshSource> LoadMeshFromCache(const std::filesystem::path& sourcePath)
        {
            OLO_PROFILE_FUNCTION();

            if (!IsMeshCacheValid(sourcePath))
            {
                return nullptr;
            }

            auto cachePath = GetMeshCachePath(sourcePath);
            OLO_CORE_INFO("MeshCache: Loading mesh from cache '{}'", cachePath.string());
            return MeshBinarySerializer::Read(cachePath);
        }

        bool SaveMeshToCache(const std::filesystem::path& sourcePath, const MeshSource& meshSource)
        {
            OLO_PROFILE_FUNCTION();

            u64 sourceTimestamp = GetSourceTimestamp(sourcePath);
            if (sourceTimestamp == 0)
            {
                OLO_CORE_WARN("MeshCache: Cannot get timestamp for source '{}', skipping cache", sourcePath.string());
                return false;
            }

            auto cachePath = GetMeshCachePath(sourcePath);
            OLO_CORE_INFO("MeshCache: Saving mesh to cache '{}'", cachePath.string());
            return MeshBinarySerializer::Write(cachePath, meshSource, sourceTimestamp);
        }

        std::vector<Ref<AnimationClip>> LoadAnimationsFromCache(const std::filesystem::path& sourcePath)
        {
            if (!IsAnimationCacheValid(sourcePath))
            {
                return {};
            }

            auto cachePath = GetAnimationCachePath(sourcePath);
            OLO_CORE_INFO("MeshCache: Loading animations from cache '{}'", cachePath.string());
            return AnimationBinarySerializer::Read(cachePath);
        }

        bool SaveAnimationsToCache(const std::filesystem::path& sourcePath, const std::vector<Ref<AnimationClip>>& clips)
        {
            u64 sourceTimestamp = GetSourceTimestamp(sourcePath);
            if (sourceTimestamp == 0)
            {
                OLO_CORE_WARN("MeshCache: Cannot get timestamp for source '{}', skipping cache", sourcePath.string());
                return false;
            }

            auto cachePath = GetAnimationCachePath(sourcePath);
            OLO_CORE_INFO("MeshCache: Saving animations to cache '{}'", cachePath.string());
            return AnimationBinarySerializer::Write(cachePath, clips, sourceTimestamp);
        }

        void ClearCache()
        {
            std::error_code ec;

            std::filesystem::path meshDir(kMeshCacheDirectory);
            if (std::filesystem::exists(meshDir, ec))
            {
                std::filesystem::remove_all(meshDir, ec);
                if (ec)
                {
                    OLO_CORE_ERROR("MeshCache: Failed to clear mesh cache: {}", ec.message());
                }
                else
                {
                    OLO_CORE_INFO("MeshCache: Cleared mesh cache");
                }
            }

            std::filesystem::path animDir(kAnimationCacheDirectory);
            if (std::filesystem::exists(animDir, ec))
            {
                std::filesystem::remove_all(animDir, ec);
                if (ec)
                {
                    OLO_CORE_ERROR("MeshCache: Failed to clear animation cache: {}", ec.message());
                }
                else
                {
                    OLO_CORE_INFO("MeshCache: Cleared animation cache");
                }
            }
        }

        void InvalidateCache(const std::filesystem::path& sourcePath)
        {
            std::error_code ec;

            auto meshPath = GetMeshCachePath(sourcePath);
            if (std::filesystem::exists(meshPath, ec))
            {
                std::filesystem::remove(meshPath, ec);
                if (!ec)
                {
                    OLO_CORE_INFO("MeshCache: Invalidated mesh cache for '{}'", sourcePath.string());
                }
            }

            auto animPath = GetAnimationCachePath(sourcePath);
            if (std::filesystem::exists(animPath, ec))
            {
                std::filesystem::remove(animPath, ec);
                if (!ec)
                {
                    OLO_CORE_INFO("MeshCache: Invalidated animation cache for '{}'", sourcePath.string());
                }
            }
        }

        u64 GetTotalCacheSize()
        {
            u64 totalSize = 0;

            auto const accumulateDirectorySize = [&](const std::filesystem::path& dir)
            {
                std::error_code existsEc;
                if (!std::filesystem::exists(dir, existsEc))
                {
                    return;
                }
                std::error_code iterEc;
                for (const auto& entry : std::filesystem::directory_iterator(dir, iterEc))
                {
                    std::error_code fileEc;
                    if (entry.is_regular_file(fileEc))
                    {
                        std::error_code sizeEc;
                        auto size = entry.file_size(sizeEc);
                        if (!sizeEc)
                        {
                            totalSize += size;
                        }
                    }
                }
            };

            accumulateDirectorySize(std::filesystem::path(kMeshCacheDirectory));
            accumulateDirectorySize(std::filesystem::path(kAnimationCacheDirectory));

            return totalSize;
        }

    } // namespace MeshCache

} // namespace OloEngine
