#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace OloEngine
{
    class FileStreamReader;

    /**
     * @brief Custom deleter for FileStreamReader to avoid incomplete type issues
     * 
     * This deleter ensures that FileStreamReader's destructor is called in the .cpp file
     * where the complete type definition is available, avoiding the need to include
     * the full FileStreamReader header in this file.
     */
    struct FileStreamReaderDeleter
    {
        void operator()(FileStreamReader* ptr) const;
    };

    /**
     * @brief Type alias for unique_ptr with custom deleter
     */
    using FileStreamReaderPtr = std::unique_ptr<FileStreamReader, FileStreamReaderDeleter>;

    /**
     * @brief Error codes for AssetPack loading operations
     */
    enum class AssetPackLoadError
    {
        None = 0,
        FileNotFound,
        FileOpenFailed,
        CorruptHeader,
        InvalidMagicNumber,
        UnsupportedVersion,
        CorruptIndex,
        MemoryAllocationFailed,
        UnexpectedEOF,
        IOError
    };

    /**
     * @brief Result structure for AssetPack load operations
     * 
     * Provides detailed error information for failed load operations,
     * including specific error codes and descriptive messages.
     */
    struct AssetPackLoadResult
    {
        bool Success = false;
        AssetPackLoadError ErrorCode = AssetPackLoadError::None;
        std::string ErrorMessage;
        f64 LoadTimeMs = 0.0;

        AssetPackLoadResult() = default;
        AssetPackLoadResult(bool success) : Success(success) {}
        AssetPackLoadResult(AssetPackLoadError error, const std::string& message)
            : Success(false), ErrorCode(error), ErrorMessage(message) {}

        operator bool() const { return Success; }
    };

    /**
     * @brief Runtime asset pack reader for loading assets from binary pack files
     * 
     * The AssetPack class provides runtime access to binary asset pack files,
     * allowing efficient loading of assets in shipping builds. It handles:
     * 
     * - Loading and validation of pack files
     * - Asset lookup and metadata retrieval
     * - Stream reader creation for asset data access
     * - Memory-efficient pack file management
     * 
     * ## Thread Safety
     * This class is NOT thread-safe. External synchronization is required
     * if accessing from multiple threads simultaneously.
     * 
     * ## Load/Unload Semantics
     * - Load() is idempotent: calling it multiple times with the same path
     *   will unload the current pack and reload from the new path
     * - Load() with a different path will automatically unload the current pack first
     * - Unload() can be called safely multiple times without error
     * - IsLoaded() provides current state information
     */
    class AssetPack final : public RefCounted
    {
    public:
        AssetPack() = default;
        
        static Ref<AssetPack> Create() { return Ref<AssetPack>::Create(); }        
        
        /**
         * @brief Load an asset pack from file with detailed error reporting
         * 
         * This method is idempotent - calling it multiple times will:
         * - If the same path: return success immediately if already loaded
         * - If different path: automatically unload current pack and load the new one
         * - If already loaded with same path: validate integrity and return success
         * 
         * Thread Safety: NOT thread-safe. Caller must ensure external synchronization.
         * 
         * @param path Path to the asset pack file (.olopack extension expected)
         * @return AssetPackLoadResult containing success status and detailed error information
         * 
         * @note Possible error conditions:
         * - AssetPackLoadError::FileNotFound: Specified file does not exist
         * - AssetPackLoadError::FileOpenFailed: File exists but cannot be opened
         * - AssetPackLoadError::CorruptHeader: File header is damaged or incomplete
         * - AssetPackLoadError::InvalidMagicNumber: Not a valid asset pack file
         * - AssetPackLoadError::UnsupportedVersion: Pack version not supported by this engine version
         * - AssetPackLoadError::CorruptIndex: Asset index table is damaged
         * - AssetPackLoadError::IOError: General I/O error during reading
         */
        AssetPackLoadResult Load(const std::filesystem::path& path);
        
        /**
         * @brief Legacy overload for backward compatibility
         * @deprecated Use the AssetPackLoadResult version for better error handling
         * @param path Path to the asset pack file
         * @return True if loading was successful
         */
        [[deprecated("Use Load() returning AssetPackLoadResult for better error handling")]]
        bool LoadLegacy(const std::filesystem::path& path);
        
        /**
         * @brief Unload the asset pack and free all resources
         * 
         * This method is safe to call multiple times. Subsequent calls after
         * the first unload operation will have no effect and will not generate errors.
         * 
         * After calling Unload():
         * - IsLoaded() will return false
         * - All asset lookup methods will return empty/invalid results
         * - Memory used by the pack index will be freed
         * 
         * Thread Safety: NOT thread-safe. Caller must ensure external synchronization.
         */
        void Unload();
        
        /**
         * @brief Check if the pack is currently loaded
         * @return True if pack is loaded
         */
        bool IsLoaded() const { return m_IsLoaded; }
        
        /**
         * @brief Check if an asset is available in this pack
         * @param handle Asset handle to check
         * @return True if asset is available
         */
        bool IsAssetAvailable(AssetHandle handle) const;
        
        /**
         * @brief Get the type of an asset in the pack
         * @param handle Asset handle
         * @return Asset type or AssetType::None if not found
         */
        AssetType GetAssetType(AssetHandle handle) const;
        
        /**
         * @brief Get asset information from the pack
         * @param handle Asset handle
         * @return Asset info if found, nullopt otherwise
         */
        std::optional<AssetPackFile::AssetInfo> GetAssetInfo(AssetHandle handle) const;
        
        /**
         * @brief Create a stream reader for reading asset data
         * @return Stream reader or nullptr if failed
         */
        FileStreamReaderPtr GetAssetStreamReader() const;
        
        /**
         * @brief Get all asset infos in the pack
         * @return Vector of asset infos
         */
        const std::vector<AssetPackFile::AssetInfo>& GetAllAssetInfos() const;
        
        /**
         * @brief Get all scene infos in the pack
         * @return Vector of scene infos
         */
        const std::vector<AssetPackFile::SceneInfo>& GetAllSceneInfos() const;
        
        /**
         * @brief Get the underlying asset pack file structure
         * @return Asset pack file structure
         */
        const AssetPackFile& GetAssetPackFile() const { return m_AssetPackFile; }
        
        /**
         * @brief Get the pack file path
         * @return Pack file path
         */
        const std::filesystem::path& GetPackPath() const { return m_PackPath; }
        
    private:
        // Delete copy constructor and copy assignment operator to prevent accidental copying
        AssetPack(const AssetPack&) = delete;
        AssetPack& operator=(const AssetPack&) = delete;
        
        AssetPackFile m_AssetPackFile;
        std::filesystem::path m_PackPath;
        bool m_IsLoaded = false;
    };

} // namespace OloEngine
