#include "OloEnginePCH.h"
#include "DirectoryTree.h"

#include <algorithm>

namespace OloEngine
{
    void DirectoryTree::Build(const std::filesystem::path& assetRoot)
    {
        OLO_PROFILE_FUNCTION();

        m_AssetRoot = assetRoot;
        m_Root = std::make_unique<DirectoryInfo>();
        m_Root->RelativePath = "";
        m_Root->Name = assetRoot.filename().string();
        m_Root->Parent = nullptr;

        ScanDirectory(*m_Root);
    }

    void DirectoryTree::RefreshSubtree(DirectoryInfo* node)
    {
        OLO_PROFILE_FUNCTION();

        if (!node)
            node = m_Root.get();

        if (!node)
            return;

        node->SubDirectories.clear();
        node->Files.clear();
        node->NeedsRefresh = false;
        ScanDirectory(*node);
    }

    void DirectoryTree::MarkDirty(const std::filesystem::path& relativePath)
    {
        // Mark the specified directory and all ancestors as needing refresh
        if (auto* dir = FindDirectory(relativePath))
        {
            for (auto* d = dir; d != nullptr; d = d->Parent)
                d->NeedsRefresh = true;
        }
        else if (m_Root)
        {
            // Directory not in tree — find the closest known ancestor and mark it + all ancestors dirty
            bool anyFound = false;
            for (auto ancestor = relativePath.parent_path(); !ancestor.empty() && ancestor != "."; ancestor = ancestor.parent_path())
            {
                if (auto* dir = FindDirectory(ancestor))
                {
                    for (auto* d = dir; d != nullptr; d = d->Parent)
                        d->NeedsRefresh = true;
                    anyFound = true;
                    break;
                }
            }
            if (!anyFound)
                m_Root->NeedsRefresh = true;
        }
    }

    DirectoryInfo* DirectoryTree::FindDirectory(const std::filesystem::path& relativePath)
    {
        if (!m_Root)
            return nullptr;

        if (relativePath.empty() || relativePath == ".")
            return m_Root.get();

        return FindRecursive(m_Root.get(), relativePath);
    }

    std::vector<std::filesystem::path> DirectoryTree::Search(const std::string& query) const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<std::filesystem::path> results;
        if (!m_Root || query.empty())
            return results;

        std::string queryLower = query;
        std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });

        SearchRecursive(*m_Root, queryLower, results);
        return results;
    }

    void DirectoryTree::ScanDirectory(DirectoryInfo& dir) const
    {
        std::filesystem::path absolutePath = m_AssetRoot / dir.RelativePath;

        std::error_code ec;
        if (!std::filesystem::exists(absolutePath, ec) || !std::filesystem::is_directory(absolutePath, ec))
            return;

        auto end = std::filesystem::directory_iterator();
        for (auto it = std::filesystem::directory_iterator(absolutePath, ec); !ec && it != end; it.increment(ec))
        {
            bool isDir = it->is_directory(ec);
            if (ec)
            {
                ec.clear();
                continue;
            }

            if (isDir)
            {
                auto child = std::make_unique<DirectoryInfo>();
                child->RelativePath = std::filesystem::relative(it->path(), m_AssetRoot, ec);
                if (ec)
                {
                    ec.clear();
                    continue;
                }
                child->Name = it->path().filename().string();
                child->Parent = &dir;
                ScanDirectory(*child);
                dir.SubDirectories.push_back(std::move(child));
            }
            else
            {
                dir.Files.push_back(it->path());
            }
        }

        // Sort subdirectories alphabetically (case-insensitive)
        std::sort(dir.SubDirectories.begin(), dir.SubDirectories.end(),
                  [](const std::unique_ptr<DirectoryInfo>& a, const std::unique_ptr<DirectoryInfo>& b)
                  {
                      std::string aLower = a->Name;
                      std::string bLower = b->Name;
                      std::transform(aLower.begin(), aLower.end(), aLower.begin(),
                                     [](unsigned char c)
                                     { return static_cast<char>(std::tolower(c)); });
                      std::transform(bLower.begin(), bLower.end(), bLower.begin(),
                                     [](unsigned char c)
                                     { return static_cast<char>(std::tolower(c)); });
                      return aLower < bLower;
                  });

        // Sort files alphabetically (case-insensitive, by filename)
        std::sort(dir.Files.begin(), dir.Files.end(),
                  [](const std::filesystem::path& a, const std::filesystem::path& b)
                  {
                      std::string aName = a.filename().string();
                      std::string bName = b.filename().string();
                      std::transform(aName.begin(), aName.end(), aName.begin(),
                                     [](unsigned char c)
                                     { return static_cast<char>(std::tolower(c)); });
                      std::transform(bName.begin(), bName.end(), bName.begin(),
                                     [](unsigned char c)
                                     { return static_cast<char>(std::tolower(c)); });
                      return aName < bName;
                  });
    }

    void DirectoryTree::SearchRecursive(const DirectoryInfo& dir, const std::string& queryLower,
                                        std::vector<std::filesystem::path>& results) const
    {
        // Check subdirectories
        for (const auto& subDir : dir.SubDirectories)
        {
            std::string nameLower = subDir->Name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });

            if (nameLower.find(queryLower) != std::string::npos)
                results.push_back(m_AssetRoot / subDir->RelativePath);

            // Always recurse into subdirectories
            SearchRecursive(*subDir, queryLower, results);
        }

        // Check files
        for (const auto& file : dir.Files)
        {
            std::string nameLower = file.filename().string();
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });

            if (nameLower.find(queryLower) != std::string::npos)
                results.push_back(file);
        }
    }

    DirectoryInfo* DirectoryTree::FindRecursive(DirectoryInfo* node, const std::filesystem::path& relativePath)
    {
        if (node->RelativePath == relativePath)
            return node;

        for (auto& child : node->SubDirectories)
        {
            if (auto* found = FindRecursive(child.get(), relativePath))
                return found;
        }

        return nullptr;
    }

} // namespace OloEngine
