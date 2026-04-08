#include "OloEnginePCH.h"
#include "MeshCache.h"
#include "OloEngine/Serialization/MeshBinarySerializer.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Project/Project.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace OloEngine
{
    namespace
    {
        constexpr const char* kMeshCacheSubdir = "cache/mesh";
        constexpr const char* kAnimationCacheSubdir = "cache/animation";

        // Returns the project asset directory if a project is active, or falls back
        // to the CWD-relative "assets/" for headless / test scenarios.
        std::filesystem::path GetAssetRoot()
        {
            if (Project::GetActive())
            {
                return Project::GetAssetDirectory();
            }
            return "assets";
        }

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
            auto dir = GetAssetRoot() / kMeshCacheSubdir;
            EnsureDirectoryExists(dir);
            return dir;
        }

        std::filesystem::path GetAnimationCacheDirectory()
        {
            auto dir = GetAssetRoot() / kAnimationCacheSubdir;
            EnsureDirectoryExists(dir);
            return dir;
        }

        std::filesystem::path GetMeshCachePath(const std::filesystem::path& sourcePath, const std::string& prefix)
        {
            // Sanitize prefix to filename-safe characters only
            std::string safe;
            safe.reserve(prefix.size());
            for (char c : prefix)
            {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-')
                {
                    safe += c;
                }
            }
            return GetCacheDirectory() / (safe + HashSourcePath(sourcePath) + ".omesh");
        }

        std::filesystem::path GetAnimationCachePath(const std::filesystem::path& sourcePath)
        {
            return GetAnimationCacheDirectory() / (HashSourcePath(sourcePath) + ".oanim");
        }

        bool IsMeshCacheValid(const std::filesystem::path& sourcePath, const std::string& prefix)
        {
            auto cachePath = GetMeshCachePath(sourcePath, prefix);

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

        Ref<MeshSource> LoadMeshFromCache(const std::filesystem::path& sourcePath, const std::string& prefix)
        {
            OLO_PROFILE_FUNCTION();

            if (!IsMeshCacheValid(sourcePath, prefix))
            {
                return nullptr;
            }

            auto cachePath = GetMeshCachePath(sourcePath, prefix);
            OLO_CORE_INFO("MeshCache: Loading mesh from cache '{}'", cachePath.string());
            return MeshBinarySerializer::Read(cachePath);
        }

        bool SaveMeshToCache(const std::filesystem::path& sourcePath, const MeshSource& meshSource, const std::string& prefix)
        {
            OLO_PROFILE_FUNCTION();

            u64 sourceTimestamp = GetSourceTimestamp(sourcePath);
            if (sourceTimestamp == 0)
            {
                OLO_CORE_WARN("MeshCache: Cannot get timestamp for source '{}', skipping cache", sourcePath.string());
                return false;
            }

            auto cachePath = GetMeshCachePath(sourcePath, prefix);
            OLO_CORE_INFO("MeshCache: Saving mesh to cache '{}'", cachePath.string());
            return MeshBinarySerializer::Write(cachePath, meshSource, sourceTimestamp);
        }

        std::vector<Ref<AnimationClip>> LoadAnimationsFromCache(const std::filesystem::path& sourcePath)
        {
            OLO_PROFILE_FUNCTION();

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
            OLO_PROFILE_FUNCTION();

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

            auto meshDir = GetAssetRoot() / kMeshCacheSubdir;
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

            auto animDir = GetAssetRoot() / kAnimationCacheSubdir;
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

        void InvalidateCache(const std::filesystem::path& sourcePath, const std::string& prefix)
        {
            std::error_code ec;

            if (prefix.empty())
            {
                // No prefix — remove all variant cache files for this source.
                // Extract the hash portion from the filename to match against.
                auto hashStr = HashSourcePath(sourcePath);
                auto cacheDir = GetCacheDirectory();
                if (std::filesystem::exists(cacheDir, ec))
                {
                    bool allRemoved = true;
                    for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec))
                    {
                        if (ec)
                        {
                            allRemoved = false;
                            break;
                        }
                        if (!entry.is_regular_file(ec))
                        {
                            if (ec)
                            {
                                allRemoved = false;
                                ec.clear();
                            }
                            continue;
                        }
                        if (entry.path().extension() == ".omesh" &&
                            entry.path().stem().string().ends_with(hashStr))
                        {
                            std::filesystem::remove(entry.path(), ec);
                            if (ec)
                            {
                                allRemoved = false;
                                ec.clear();
                            }
                        }
                    }
                    if (ec)
                    {
                        allRemoved = false;
                    }
                    if (allRemoved)
                    {
                        OLO_CORE_INFO("MeshCache: Invalidated all mesh cache variants for '{}'", sourcePath.string());
                    }
                    else
                    {
                        OLO_CORE_WARN("MeshCache: Some cache files could not be removed for '{}': {}", sourcePath.string(), ec.message());
                    }
                }
            }
            else
            {
                auto meshPath = GetMeshCachePath(sourcePath, prefix);
                if (std::filesystem::exists(meshPath, ec))
                {
                    std::filesystem::remove(meshPath, ec);
                    if (!ec)
                    {
                        OLO_CORE_INFO("MeshCache: Invalidated mesh cache for '{}'", sourcePath.string());
                    }
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

            accumulateDirectorySize(GetAssetRoot() / kMeshCacheSubdir);
            accumulateDirectorySize(GetAssetRoot() / kAnimationCacheSubdir);

            return totalSize;
        }

    } // namespace MeshCache

} // namespace OloEngine
