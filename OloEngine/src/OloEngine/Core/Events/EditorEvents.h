#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Events/Event.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include <string>
#include <format>
#include <chrono>
#include <filesystem>

namespace OloEngine
{
    /**
     * @brief Type of file system change for hot-reload events
     */
    enum class FileSystemChangeKind : u8
    {
        Created = 0,    ///< File or directory was created
        Modified = 1,   ///< File or directory was modified
        Deleted = 2,    ///< File or directory was deleted
        Renamed = 3     ///< File or directory was renamed/moved
    };

    /**
     * @brief File system event for asset hot-reload
     * 
     * Contains information about file system changes that may trigger
     * asset reloading or other editor responses.
     */
    struct FileSystemEvent
    {
        FileSystemChangeKind ChangeKind = FileSystemChangeKind::Modified;
        std::filesystem::path FilePath;           ///< Absolute or relative path to the changed file/directory
        std::filesystem::path OldFilePath;        ///< Previous path (only used for Renamed events)
        bool IsDirectory = false;       ///< True if the change affects a directory, false for files
        std::chrono::system_clock::time_point Timestamp; ///< When the file system change occurred
        
        /**
         * @brief Constructor for most file system events
         * @param changeKind Type of change that occurred
         * @param filePath Path to the affected file or directory
         * @param isDirectory Whether the path refers to a directory
         */
        explicit FileSystemEvent(FileSystemChangeKind changeKind, const std::filesystem::path& filePath, bool isDirectory = false)
            : ChangeKind(changeKind), FilePath(filePath), IsDirectory(isDirectory), Timestamp(std::chrono::system_clock::now())
        {
        }
        
        /**
         * @brief Constructor for rename events
         * @param oldPath Previous path before rename
         * @param newPath New path after rename
         * @param isDirectory Whether the path refers to a directory
         */
        explicit FileSystemEvent(const std::filesystem::path& oldPath, const std::filesystem::path& newPath, bool isDirectory = false)
            : ChangeKind(FileSystemChangeKind::Renamed), FilePath(newPath), OldFilePath(oldPath), IsDirectory(isDirectory), Timestamp(std::chrono::system_clock::now())
        {
        }
        
        /**
         * @brief Default constructor
         */
        FileSystemEvent() : Timestamp(std::chrono::system_clock::now()) {}
    };

    /**
     * @brief Engine event fired after an asset has been successfully reloaded/replaced
     *
     * This integrates with the engine's Event system so editor/runtime layers can
     * listen for asset changes and react (e.g., refresh inspectors, rebind resources).
     */
    class AssetReloadedEvent : public Event
    {
    public:
        AssetReloadedEvent(AssetHandle handle, AssetType type, const std::filesystem::path& path)
            : m_Handle(handle), m_Type(type), m_Path(path) {}

        // Event API
        EVENT_CLASS_TYPE(AssetReloaded)
        EVENT_CLASS_CATEGORY(EventCategory::Application)

        // Payload accessors
        AssetHandle GetHandle() const { return m_Handle; }
        AssetType GetAssetType() const { return m_Type; }
        const std::filesystem::path& GetPath() const { return m_Path; }

        std::string ToString() const override
        {
            return std::format("AssetReloadedEvent: handle={}, type={}, path={}", 
                static_cast<u64>(m_Handle), 
                static_cast<int>(m_Type), 
                m_Path.string());
        }

    private:
        AssetHandle m_Handle = 0;
        AssetType m_Type = AssetType::None;
        std::filesystem::path m_Path;
    };

}
