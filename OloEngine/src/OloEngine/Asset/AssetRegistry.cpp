#include "OloEnginePCH.h"
#include "AssetRegistry.h"

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <utility>

namespace OloEngine
{
    // Static empty metadata for invalid lookups
    static const AssetMetadata s_EmptyMetadata{};

    bool AssetRegistry::IsCacheArtefact(const std::filesystem::path& path)
    {
        // `**/cache/**` in .gitignore: a DIRECTORY named cache at any depth. The filename itself
        // is not a directory, so a file called "cache" is not matched (and neither is it by git).
        // Case-insensitive, as git is here (core.ignorecase).
        const std::filesystem::path parent = path.parent_path();
        for (const std::filesystem::path& component : parent)
        {
            const std::string name = component.string();
            if (name.size() == 5 && std::ranges::equal(name, std::string_view{ "cache" }, [](char a, char b)
                                                       { return std::tolower(static_cast<unsigned char>(a)) == b; }))
                return true;
        }
        return false;
    }

    AssetHandle AssetRegistry::CacheArtefactHandle(const std::filesystem::path& path)
    {
        // Over the generic (forward-slash) spelling, so Windows and Linux derive the same handle
        // for the same project-relative path.
        const std::string key = path.generic_string();
        const u64 hash = Hash::FNV1a64(key.data(), key.size());
        return AssetHandle(hash == 0 ? 1 : hash);
    }

    AssetHandle AssetRegistry::HandleForNewEntry(const std::filesystem::path& path)
    {
        return IsCacheArtefact(path) ? CacheArtefactHandle(path) : AssetHandle();
    }

    bool AssetRegistry::IsInFlightCookFile(const std::filesystem::path& path)
    {
        // "<stem>.tmp<16 hex>.png": the inner extension starts ".tmp" (Model.cpp CookRgbaToAssetCache).
        return IsCacheArtefact(path) && path.stem().extension().string().starts_with(".tmp");
    }

    bool AssetRegistry::IsAcceptableHandleForPath(AssetHandle handle, const std::filesystem::path& path, const char* caller)
    {
        if (!IsCacheArtefact(path))
            return true;
        if (const AssetHandle derived = CacheArtefactHandle(path); handle != derived)
        {
            OLO_CORE_ERROR("AssetRegistry::{} - refused {} under handle {}: it is a cache artefact, whose handle must be "
                           "the one derived from its path ({}). Any other handle would be lost on the next load, "
                           "because cache artefacts are never written to the registry file (#1664).",
                           caller, path.generic_string(), static_cast<u64>(handle), static_cast<u64>(derived));
            return false;
        }
        return true;
    }

    bool AssetRegistry::AddAsset(const AssetMetadata& metadata)
    {
        if (metadata.Handle == 0)
        {
            OLO_CORE_WARN("AssetRegistry::AddAsset - Invalid asset handle");
            return false;
        }
        if (!IsAcceptableHandleForPath(metadata.Handle, metadata.FilePath, "AddAsset"))
            return false;

        TUniqueLock<FSharedMutex> lock(m_Mutex);

        // Check for existing asset with same handle
        if (auto handleIt = m_AssetMetadata.find(metadata.Handle); handleIt != m_AssetMetadata.end())
        {
            // A cooked file moved out of cache/ keeps its derived handle under an authored path.
            // The cook writing the file again at the old path derives that same handle; replacing
            // the authored entry with it would point every reference at the regenerated copy.
            if (IsCacheArtefact(metadata.FilePath) && handleIt->second.FilePath != metadata.FilePath)
            {
                OLO_CORE_ERROR("AssetRegistry::AddAsset - refused {}: its derived handle {} already belongs to {}, "
                               "which was moved out of the cache (#1664); the cache artefact stays unregistered",
                               metadata.FilePath.string(), metadata.Handle, handleIt->second.FilePath.string());
                return false;
            }
            // Handle already exists - log warning about potential overwrite
            if (handleIt->second.FilePath != metadata.FilePath || handleIt->second.Type != metadata.Type)
            {
                OLO_CORE_WARN("AssetRegistry::AddAsset - Handle {} already exists for different asset (existing: {}, new: {}). Overwriting existing asset.",
                              metadata.Handle, handleIt->second.FilePath.string(), metadata.FilePath.string());
            }
            else
            {
                OLO_CORE_WARN("AssetRegistry::AddAsset - Handle {} already exists for same asset. Updating metadata.", metadata.Handle);
            }
        }

        // Check for existing asset with same path (if path is provided)
        if (!metadata.FilePath.empty())
        {
            auto pathIt = m_PathToHandle.find(metadata.FilePath);
            if (pathIt != m_PathToHandle.end() && pathIt->second != metadata.Handle)
            {
                OLO_CORE_WARN("AssetRegistry::AddAsset - Path {} already mapped to different handle {} (new handle: {}). Overwriting existing path mapping.",
                              metadata.FilePath.string(), pathIt->second, metadata.Handle);
            }
        }

        // Update main storage
        m_AssetMetadata[metadata.Handle] = metadata;

        // Update path lookup if path is valid
        if (!metadata.FilePath.empty())
        {
            m_PathToHandle[metadata.FilePath] = metadata.Handle;
        }

        // Update handle counter to maintain monotonic sequence and avoid duplicates. Not for a
        // cache artefact: its handle is a hash, and letting it move the counter would make the
        // handle of the next authored asset depend on which cache files this machine has.
        if (!IsCacheArtefact(metadata.FilePath))
        {
            u64 currentCounter = m_HandleCounter.load(std::memory_order_relaxed);
            if (metadata.Handle >= currentCounter)
            {
                m_HandleCounter.store(metadata.Handle + 1, std::memory_order_relaxed);
            }
        }
        return true;
    }

    bool AssetRegistry::RemoveAsset(AssetHandle handle)
    {
        TUniqueLock<FSharedMutex> lock(m_Mutex);

        auto it = m_AssetMetadata.find(handle);
        if (it == m_AssetMetadata.end())
            return false;

        // Remove from path lookup
        if (!it->second.FilePath.empty())
        {
            m_PathToHandle.erase(it->second.FilePath);
        }

        // Remove from main storage
        m_AssetMetadata.erase(it);
        return true;
    }

    AssetMetadata AssetRegistry::GetMetadata(AssetHandle handle) const
    {
        OLO_PROFILE_FUNCTION();

        TSharedLock<FSharedMutex> lock(m_Mutex);

        auto it = m_AssetMetadata.find(handle);
        return (it != m_AssetMetadata.end()) ? it->second : s_EmptyMetadata;
    }

    AssetMetadata AssetRegistry::GetMetadata(const std::filesystem::path& path) const
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);

        if (auto it = m_PathToHandle.find(path); it != m_PathToHandle.end())
        {
            auto metaIt = m_AssetMetadata.find(it->second);
            if (metaIt != m_AssetMetadata.end())
                return metaIt->second;
        }

        return s_EmptyMetadata;
    }

    bool AssetRegistry::Exists(AssetHandle handle) const
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);
        return m_AssetMetadata.find(handle) != m_AssetMetadata.end();
    }

    bool AssetRegistry::Exists(const std::filesystem::path& path) const
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);
        return m_PathToHandle.find(path) != m_PathToHandle.end();
    }

    AssetHandle AssetRegistry::GetHandleFromPath(const std::filesystem::path& path) const
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);

        auto it = m_PathToHandle.find(path);
        return (it != m_PathToHandle.end()) ? it->second : AssetHandle(0);
    }

    std::vector<AssetMetadata> AssetRegistry::GetAssetsOfType(AssetType type) const
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);

        std::vector<AssetMetadata> result;
        for (const auto& [handle, metadata] : m_AssetMetadata)
        {
            if (metadata.Type == type)
                result.push_back(metadata);
        }

        return result;
    }

    std::unordered_set<AssetHandle> AssetRegistry::GetAssetHandlesOfType(AssetType type) const
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);

        std::unordered_set<AssetHandle> result;
        for (const auto& [handle, metadata] : m_AssetMetadata)
        {
            if (metadata.Type == type)
                result.insert(handle);
        }

        return result;
    }

    std::vector<AssetMetadata> AssetRegistry::GetAllAssets() const
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);

        std::vector<AssetMetadata> result;
        result.reserve(m_AssetMetadata.size());

        for (const auto& [handle, metadata] : m_AssetMetadata)
        {
            result.push_back(metadata);
        }

        return result;
    }

    sizet AssetRegistry::GetAssetCount() const noexcept
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);
        return m_AssetMetadata.size();
    }

    void AssetRegistry::Clear()
    {
        TUniqueLock<FSharedMutex> lock(m_Mutex);
        m_AssetMetadata.clear();
        m_PathToHandle.clear();
        m_HandleCounter.store(1, std::memory_order_relaxed);
    }

    bool AssetRegistry::UpdateMetadata(AssetHandle handle, const AssetMetadata& metadata)
    {
        if (!IsAcceptableHandleForPath(handle, metadata.FilePath, "UpdateMetadata"))
            return false;

        TUniqueLock<FSharedMutex> lock(m_Mutex);

        auto it = m_AssetMetadata.find(handle);
        if (it == m_AssetMetadata.end())
        {
            OLO_CORE_WARN("AssetRegistry::UpdateMetadata - Asset handle {} not found", handle);
            return false;
        }

        // Remove old path mapping
        if (!it->second.FilePath.empty())
        {
            m_PathToHandle.erase(it->second.FilePath);
        }

        // Update metadata (preserve handle)
        AssetMetadata updatedMetadata = metadata;
        updatedMetadata.Handle = handle;
        it->second = updatedMetadata;

        // Add new path mapping (check for collision first)
        if (!updatedMetadata.FilePath.empty())
        {
            // Check if the new path is already mapped to a different handle
            if (auto pathIt = m_PathToHandle.find(updatedMetadata.FilePath); pathIt != m_PathToHandle.end() && pathIt->second != handle)
            {
                OLO_CORE_WARN("AssetRegistry::UpdateMetadata - Path {} already mapped to different handle {} (current handle: {}). Overwriting existing path mapping.",
                              updatedMetadata.FilePath.string(), pathIt->second, handle);
            }

            m_PathToHandle[updatedMetadata.FilePath] = handle;
        }
        return true;
    }

    AssetHandle AssetRegistry::GenerateHandle()
    {
        // No lock needed - GetNextHandle uses atomic operations for thread safety
        return GetNextHandle();
    }

    bool AssetRegistry::Serialize(const std::filesystem::path& filepath) const
    {
        try
        {
            // The bytes are built under the shared lock and written after it is
            // released: the write below syncs to disk, and SetMetadata (exclusive)
            // must not wait out a disk round-trip.
            std::ostringstream file(std::ios::out | std::ios::binary);
            {
                TSharedLock<FSharedMutex> lock(m_Mutex);

                // Entries in handle order: the map's iteration order depends on how it was
                // filled, so a session that re-registered the same assets wrote the same
                // registry in another order and dirtied the git-tracked file.
                //
                // Cache artefacts are left out (#1664): their files are git-ignored, so an entry
                // for one in the tracked registry names a file a fresh clone does not have. They
                // lose nothing by it, because their handle is derived from their path and every
                // manager that registers the file derives it again.
                TArray<const AssetMetadata*> ordered;
                ordered.Reserve(static_cast<i32>(m_AssetMetadata.size()));
                for (const auto& entry : m_AssetMetadata)
                {
                    if (!IsCacheArtefact(entry.second.FilePath))
                        ordered.Add(&entry.second);
                }

                // Write header
                const u32 version = FileVersion;
                file.write(reinterpret_cast<const char*>(&version), sizeof(version));

                // Write asset count
                const u32 assetCount = static_cast<u32>(ordered.Num());
                file.write(reinterpret_cast<const char*>(&assetCount), sizeof(assetCount));

                ordered.Sort([](const AssetMetadata* a, const AssetMetadata* b)
                             { return static_cast<u64>(a->Handle) < static_cast<u64>(b->Handle); });

                // Write each asset metadata. Status is not written: it is this process's load
                // state, and persisting it made every session that loaded an asset rewrite the
                // git-tracked registry (#1644).
                for (const AssetMetadata* entry : ordered)
                {
                    const AssetMetadata& metadata = *entry;
                    // Cast to fixed-width types for cross-platform compatibility
                    const uint64_t handleValue = static_cast<uint64_t>(metadata.Handle);
                    const uint32_t typeValue = static_cast<uint32_t>(std::to_underlying(metadata.Type));

                    file.write(reinterpret_cast<const char*>(&handleValue), sizeof(handleValue));
                    file.write(reinterpret_cast<const char*>(&typeValue), sizeof(typeValue));
                    // Note: LastWriteTime serialization temporarily skipped - std::filesystem::file_time_type is complex to serialize

                    // Write path string. Use generic_string() so the .oar is
                    // portable across platforms: on Windows path.string() returns
                    // backslash separators, which on Linux fs::path treats as
                    // literal filename characters (not separators), so subsequent
                    // fs::exists / fs::relative resolve incorrectly. generic_string
                    // always emits forward slashes regardless of OS.
                    const std::string pathStr = metadata.FilePath.generic_string();
                    const u32 pathLength = static_cast<u32>(pathStr.length());
                    file.write(reinterpret_cast<const char*>(&pathLength), sizeof(pathLength));
                    file.write(pathStr.c_str(), pathLength);
                }
            }

            // Written atomically: a failed write must not leave a truncated
            // registry, which would lose the handle of every asset in the project.
            // Durable for the same reason: the rename must not reach the disk first.
            return FileSystem::WriteFileAtomically(filepath, file.view(), { .Durable = true });
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetRegistry::Serialize - Exception: {}", e.what());
            return false;
        }
    }

    bool AssetRegistry::Deserialize(const std::filesystem::path& filepath)
    {
        try
        {
            std::ifstream file(filepath, std::ios::binary);
            if (!file.is_open())
            {
                OLO_CORE_WARN("AssetRegistry::Deserialize - File not found: {}", filepath.string());
                return false;
            }

            // Enable exceptions for better error detection on I/O failures
            file.exceptions(std::ifstream::failbit | std::ifstream::badbit);

            TUniqueLock<FSharedMutex> lock(m_Mutex);

            // Clear existing data
            m_AssetMetadata.clear();
            m_PathToHandle.clear();
            m_HandleCounter.store(1, std::memory_order_relaxed); // Reset handle counter to prevent stale handle values

            // Read header
            u32 version;
            file.read(reinterpret_cast<char*>(&version), sizeof(version));

            if (version != FileVersion)
            {
                OLO_CORE_ERROR("AssetRegistry::Deserialize - {}: format v{} is not supported (this build reads v{} only). "
                               "Restore it from version control, or let the editor rebuild it from the project "
                               "(the asset-directory rescan writes a fresh registry with newly assigned handles).",
                               filepath.string(), version, FileVersion);
                return false;
            }

            // Read asset count
            u32 assetCount;
            file.read(reinterpret_cast<char*>(&assetCount), sizeof(assetCount));

            // Validate assetCount to prevent excessive memory usage or infinite loops
            const u32 MAX_ASSET_COUNT = 1000000; // 1 million assets maximum
            if (assetCount > MAX_ASSET_COUNT)
            {
                OLO_CORE_ERROR("AssetRegistry::Deserialize - Invalid asset count: {} exceeds maximum {}", assetCount, MAX_ASSET_COUNT);
                return false;
            }

            // Read each asset metadata
            u32 droppedCacheEntries = 0;
            for (u32 i = 0; i < assetCount; ++i)
            {
                AssetMetadata metadata;

                // Fixed-width fields for cross-platform compatibility
                uint64_t handleValue;
                uint32_t typeValue;

                file.read(reinterpret_cast<char*>(&handleValue), sizeof(handleValue));
                file.read(reinterpret_cast<char*>(&typeValue), sizeof(typeValue));

                metadata.Handle = static_cast<AssetHandle>(handleValue);
                metadata.Type = static_cast<AssetType>(typeValue);
                // metadata.Status keeps its not-loaded default: nothing this process loaded yet.
                // Note: LastWriteTime deserialization temporarily skipped - will be refreshed from filesystem

                // Read path string
                u32 pathLength;
                file.read(reinterpret_cast<char*>(&pathLength), sizeof(pathLength));

                // Validate pathLength to prevent excessive memory allocation
                const u32 MAX_PATH_LENGTH = 32768; // 32KB limit for path strings
                if (pathLength > MAX_PATH_LENGTH)
                {
                    OLO_CORE_ERROR("AssetRegistry::Deserialize - Invalid path length: {} exceeds maximum {}", pathLength, MAX_PATH_LENGTH);
                    return false;
                }

                std::string pathStr(pathLength, '\0');
                file.read(pathStr.data(), pathLength);
                // Serialize() writes generic_string(), so paths are always forward-slashed.
                metadata.FilePath = pathStr;

                // A cache artefact in a registry FILE was written by a build from before #1664
                // (or by hand). Its handle is not the derived one, so keeping it would give the
                // file two handles depending on which registered it first. Drop it; the scan
                // registers the file again under its derived handle if the file is there.
                if (IsCacheArtefact(metadata.FilePath))
                {
                    ++droppedCacheEntries;
                    OLO_CORE_ERROR("AssetRegistry::Deserialize - {} registers the cache artefact {} (handle {}); cache "
                                   "artefacts are never stored in a registry file (#1664), so the entry is dropped. "
                                   "Save the registry to remove it from the file.",
                                   filepath.string(), metadata.FilePath.generic_string(),
                                   static_cast<u64>(metadata.Handle));
                    // Its handle was minted by the counter and something (a scene, a stale
                    // .omesh) may still hold it: keep the counter past it so the next authored
                    // asset does not inherit it and resolve as the wrong asset.
                    if (const u64 currentCounter = m_HandleCounter.load(std::memory_order_relaxed);
                        static_cast<u64>(metadata.Handle) >= currentCounter)
                    {
                        m_HandleCounter.store(static_cast<u64>(metadata.Handle) + 1, std::memory_order_relaxed);
                    }
                    continue;
                }

                // Refresh LastWriteTime from filesystem if file exists
                if (std::filesystem::exists(metadata.FilePath))
                {
                    metadata.LastWriteTime = std::filesystem::last_write_time(metadata.FilePath);
                }

                // Add to registry
                m_AssetMetadata[metadata.Handle] = metadata;
                if (!metadata.FilePath.empty())
                {
                    m_PathToHandle[metadata.FilePath] = metadata.Handle;
                }

                // Update handle counter
                u64 currentCounter = m_HandleCounter.load(std::memory_order_relaxed);
                if (metadata.Handle >= currentCounter)
                {
                    m_HandleCounter.store(metadata.Handle + 1, std::memory_order_relaxed);
                }
            }

            OLO_CORE_INFO("AssetRegistry deserialized from: {} ({} assets)", filepath.string(),
                          assetCount - droppedCacheEntries);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("AssetRegistry::Deserialize - Exception: {}", e.what());
            return false;
        }
    }

    bool AssetRegistry::Empty() const
    {
        TSharedLock<FSharedMutex> lock(m_Mutex);
        return m_AssetMetadata.empty();
    }

    AssetHandle AssetRegistry::GetNextHandle()
    {
        // Thread-safe atomic increment - no additional locking needed
        return m_HandleCounter.fetch_add(1, std::memory_order_relaxed);
    }

} // namespace OloEngine
