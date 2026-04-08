#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <filesystem>
#include <vector>

namespace OloEngine
{
    class MeshSource;
    class AnimationClip;

    // Manages the binary mesh/animation cache under assets/cache/mesh/.
    // Mirrors the shader cache pattern: check source timestamp, skip Assimp if cached.
    namespace MeshCache
    {
        // Returns the cache directory path (assets/cache/mesh/).
        // Creates the directory if it doesn't exist.
        std::filesystem::path GetCacheDirectory();

        // Returns the animation cache directory path (assets/cache/animation/).
        std::filesystem::path GetAnimationCacheDirectory();

        // Compute cache file path from source file path.
        // e.g. "models/fox.gltf" → "assets/cache/mesh/<hash>.omesh"
        // Optional prefix differentiates cache namespaces (e.g. "anim_" for animated models).
        std::filesystem::path GetMeshCachePath(const std::filesystem::path& sourcePath, const std::string& prefix = {});
        std::filesystem::path GetAnimationCachePath(const std::filesystem::path& sourcePath);

        // Check if a valid cache exists for the given source file.
        // Returns true if cached file exists AND source timestamp matches.
        bool IsMeshCacheValid(const std::filesystem::path& sourcePath, const std::string& prefix = {});
        bool IsAnimationCacheValid(const std::filesystem::path& sourcePath);

        // Load a MeshSource from cache. Returns nullptr if cache is invalid/missing.
        Ref<MeshSource> LoadMeshFromCache(const std::filesystem::path& sourcePath, const std::string& prefix = {});

        // Save a MeshSource to cache after import.
        bool SaveMeshToCache(const std::filesystem::path& sourcePath, const MeshSource& meshSource, const std::string& prefix = {});

        // Load AnimationClips from cache. Returns empty if cache is invalid/missing.
        std::vector<Ref<AnimationClip>> LoadAnimationsFromCache(const std::filesystem::path& sourcePath);

        // Save AnimationClips to cache after import.
        bool SaveAnimationsToCache(const std::filesystem::path& sourcePath, const std::vector<Ref<AnimationClip>>& clips);

        // Delete all cached files (e.g. for "clear cache" editor action).
        void ClearCache();

        // Delete the cached files for a specific source file (reimport trigger).
        void InvalidateCache(const std::filesystem::path& sourcePath, const std::string& prefix = {});

        // Returns the total size of all cached mesh + animation files in bytes.
        u64 GetTotalCacheSize();

    } // namespace MeshCache

} // namespace OloEngine
