#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <memory>

namespace OloEngine
{
    // Cached snapshot of a single directory in the asset tree.
    // Built once and refreshed on demand — no per-frame filesystem I/O.
    struct DirectoryInfo
    {
        std::filesystem::path RelativePath; // Relative to asset root
        std::string Name;                   // Cached leaf name
        DirectoryInfo* Parent = nullptr;

        std::vector<std::unique_ptr<DirectoryInfo>> SubDirectories; // Sorted alphabetically
        std::vector<std::filesystem::path> Files;                   // Sorted: alpha by filename

        bool NeedsRefresh = false;
    };

    // Manages a cached directory tree rooted at the project's asset directory.
    class DirectoryTree
    {
      public:
        // Build (or rebuild) the entire tree from disk.
        void Build(const std::filesystem::path& assetRoot);

        // Refresh only the subtree rooted at the given node.
        // If node is null, refreshes the entire tree.
        void RefreshSubtree(DirectoryInfo* node = nullptr);

        // Mark a directory (and its ancestors) as needing refresh.
        // relativePath is relative to the asset root.
        void MarkDirty(const std::filesystem::path& relativePath);

        // Find a DirectoryInfo by its relative path (e.g. "textures/characters").
        // Returns nullptr if not found.
        [[nodiscard]] DirectoryInfo* FindDirectory(const std::filesystem::path& relativePath) const;

        // Recursive substring search across the entire tree.
        // Returns file paths whose filename contains 'query' (case-insensitive).
        // Also returns directory paths whose name matches.
        [[nodiscard]] std::vector<std::filesystem::path> Search(const std::string& query) const;

        [[nodiscard]] DirectoryInfo* GetRoot() const { return m_Root.get(); }
        [[nodiscard]] const std::filesystem::path& GetAssetRoot() const { return m_AssetRoot; }

      private:
        void ScanDirectory(DirectoryInfo& dir) const;
        void SearchRecursive(const DirectoryInfo& dir, const std::string& queryLower,
                             std::vector<std::filesystem::path>& results) const;
        DirectoryInfo* FindRecursive(DirectoryInfo* node, const std::filesystem::path& relativePath) const;

        std::filesystem::path m_AssetRoot;
        std::unique_ptr<DirectoryInfo> m_Root;
    };

} // namespace OloEngine
