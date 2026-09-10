#include "OloCtl/CommandTree.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace OloCtl
{
    namespace
    {
        // Split on the two separators the registry's names actually use: `_` for a
        // native command and `.` for one bridged from an external MCP server
        // (`ext.<alias>.<tool>`). Empty segments are dropped, so a doubled or
        // trailing separator cannot produce an empty path element. Lowercasing
        // happens here so a name and a toolset that differ only in case land on the
        // same spelling, which is what a shell user will type.
        std::vector<std::string> Segments(const std::string& name)
        {
            std::vector<std::string> segments;
            std::string current;
            for (const char c : name)
            {
                if (c == '_' || c == '.')
                {
                    if (!current.empty())
                        segments.push_back(std::exchange(current, std::string{}));
                    continue;
                }
                current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            if (!current.empty())
                segments.push_back(std::move(current));
            return segments;
        }

        std::string Join(const std::vector<std::string>& segments, std::size_t from)
        {
            std::string out;
            for (std::size_t i = from; i < segments.size(); ++i)
            {
                if (!out.empty())
                    out.push_back('-');
                out += segments[i];
            }
            return out;
        }
    } // namespace

    DerivedPath DerivePath(const CatalogueEntry& entry)
    {
        std::vector<std::string> segments = Segments(entry.Name);
        // The `olo_` prefix is on every native command and carries no information
        // once you are already typing `oloctl`.
        if (!segments.empty() && segments.front() == "olo")
            segments.erase(segments.begin());

        DerivedPath path;
        std::vector<std::string> groupSegments;
        if (!entry.Toolset.empty())
        {
            groupSegments = Segments(entry.Toolset);
            path.Group = Join(groupSegments, 0);
        }
        if (path.Group.empty())
        {
            if (segments.size() >= 2)
            {
                groupSegments = { segments.front() };
                path.Group = segments.front();
            }
            else
            {
                groupSegments.clear();
                path.Group = "misc";
            }
        }

        // Drop the group's segments off the front of the name when the name repeats
        // them, so `olo_scene_list_entities` in toolset `scene` is `scene
        // list-entities` rather than `scene scene-list-entities`. Only a full
        // prefix match counts: a partial one would mangle an unrelated name that
        // happens to start with the same letters.
        std::size_t from = 0;
        if (!groupSegments.empty() && segments.size() > groupSegments.size() &&
            std::equal(groupSegments.begin(), groupSegments.end(), segments.begin()))
        {
            from = groupSegments.size();
        }

        path.Command = Join(segments, from);
        // A name that was nothing but its group (`olo_capability` in toolset
        // `gateway` is not this, but `olo_scene` in toolset `scene` would be) still
        // needs a leaf to be reachable at all.
        if (path.Command.empty())
            path.Command = path.Group;
        return path;
    }

    CommandTree CommandTree::Build(const Catalogue& catalogue)
    {
        CommandTree tree;
        tree.m_Catalogue = &catalogue;
        for (std::size_t index = 0; index < catalogue.Entries.size(); ++index)
        {
            const DerivedPath path = DerivePath(catalogue.Entries[index]);

            auto group = std::find_if(tree.m_Groups.begin(), tree.m_Groups.end(),
                                      [&path](const TreeGroup& candidate)
                                      { return candidate.Name == path.Group; });
            if (group == tree.m_Groups.end())
            {
                tree.m_Groups.push_back(TreeGroup{ path.Group, {} });
                group = std::prev(tree.m_Groups.end());
            }

            const bool alreadyThere = std::any_of(group->Leaves.begin(), group->Leaves.end(),
                                                  [&path](const TreeLeaf& leaf)
                                                  { return leaf.Command == path.Command; });
            if (alreadyThere)
            {
                const std::string collision = path.Group + ' ' + path.Command;
                if (std::find(tree.m_AmbiguousPaths.begin(), tree.m_AmbiguousPaths.end(), collision) ==
                    tree.m_AmbiguousPaths.end())
                {
                    tree.m_AmbiguousPaths.push_back(collision);
                }
            }
            // Appended regardless of the collision: dropping the loser would hide a
            // registered command, and Resolve() needs both to report Ambiguous
            // honestly.
            group->Leaves.push_back(TreeLeaf{ path.Command, index });
        }

        std::sort(tree.m_Groups.begin(), tree.m_Groups.end(),
                  [](const TreeGroup& a, const TreeGroup& b)
                  { return a.Name < b.Name; });
        for (TreeGroup& group : tree.m_Groups)
        {
            std::sort(group.Leaves.begin(), group.Leaves.end(),
                      [](const TreeLeaf& a, const TreeLeaf& b)
                      { return a.Command < b.Command; });
        }
        std::sort(tree.m_AmbiguousPaths.begin(), tree.m_AmbiguousPaths.end());
        return tree;
    }

    const TreeGroup* CommandTree::FindGroup(const std::string& group) const
    {
        const auto it = std::find_if(m_Groups.begin(), m_Groups.end(),
                                     [&group](const TreeGroup& candidate)
                                     { return candidate.Name == group; });
        return it == m_Groups.end() ? nullptr : &*it;
    }

    CommandTree::Resolution CommandTree::Resolve(const std::string& group, const std::string& command) const
    {
        Resolution resolution;
        const TreeGroup* found = FindGroup(group);
        if (found == nullptr)
        {
            resolution.Status = ResolveStatus::UnknownGroup;
            return resolution;
        }

        for (const TreeLeaf& leaf : found->Leaves)
        {
            if (leaf.Command != command)
                continue;
            resolution.Candidates.push_back(m_Catalogue->Entries[leaf.EntryIndex].Name);
            resolution.EntryIndex = leaf.EntryIndex;
        }

        if (resolution.Candidates.empty())
            resolution.Status = ResolveStatus::UnknownCommand;
        else if (resolution.Candidates.size() > 1)
            resolution.Status = ResolveStatus::Ambiguous;
        else
            resolution.Status = ResolveStatus::Ok;
        return resolution;
    }
} // namespace OloCtl
