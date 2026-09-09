#pragma once

// The hierarchical CLI surface, derived from a catalogue (issue #1125).
//
// `olo_scene_list_entities` becomes `oloctl scene list-entities`. The derivation
// is mechanical and total — every entry in the catalogue lands somewhere, and no
// entry needs a rule written for it — because the moment one command needs a
// hand-maintained mapping, oloctl has become a second command table and the
// property this issue is about is gone.
//
// THE RULES, in order:
//
//   Group   1. The command's toolset, lowercased, with `_` and `.` as `-`.
//           2. Otherwise the first segment of its name (splitting on `_` and `.`,
//              after dropping a leading `olo`), when at least two segments remain.
//           3. Otherwise "misc".
//   Command The name, minus a leading `olo` segment, minus the group's segments
//           when the name repeats them, joined with `-`. Empty (a name that was
//           nothing but its group) becomes the group itself.
//
// Rule 2 exists for a command with no toolset; rule 3 for a single-segment name
// with no toolset. Neither is reachable from today's registry, and both are
// there so a future command cannot fail to appear.
//
// COLLISIONS ARE KEPT, NOT RESOLVED. Two commands deriving the same group +
// command pair stay in the tree and Resolve() reports Ambiguous with both
// registry names, because inventing a tie-break (append a digit, prefer the
// earlier registration) would silently point `oloctl scene open` at whichever
// one won. `oloctl call <registry name>` addresses either without ambiguity, and
// OloCtlCommandTreeTest asserts the real registered surface has no collisions.

#include "OloCtl/CommandCatalogue.h"

#include <cstddef>
#include <string>
#include <vector>

namespace OloCtl
{
    struct TreeLeaf
    {
        std::string Command;        // the leaf spelling, e.g. "list-entities"
        std::size_t EntryIndex = 0; // index into Catalogue::Entries
    };

    struct TreeGroup
    {
        std::string Name; // e.g. "scene"
        std::vector<TreeLeaf> Leaves;
    };

    // Group + command spelling for one catalogue entry, as the tree derives them.
    // Exposed so `oloctl catalogue` can report the mapping for every command
    // without walking the groups.
    struct DerivedPath
    {
        std::string Group;
        std::string Command;
    };

    [[nodiscard]] DerivedPath DerivePath(const CatalogueEntry& entry);

    class CommandTree
    {
      public:
        enum class ResolveStatus
        {
            Ok,
            UnknownGroup,
            UnknownCommand,
            Ambiguous,
        };

        struct Resolution
        {
            ResolveStatus Status = ResolveStatus::UnknownGroup;
            std::size_t EntryIndex = 0;
            // Registry names of every entry that matched, when Status is Ambiguous.
            std::vector<std::string> Candidates;
        };

        // Build the tree over `catalogue`, which MUST outlive the tree: leaves hold
        // indices into its Entries, and the tree keeps a pointer to it.
        //
        // The catalogue is held rather than passed back in per call on purpose. It
        // used to be a Resolve() parameter, which let a caller hand in a DIFFERENT
        // catalogue from the one the indices were built against — and the indices
        // are used unchecked, so a shorter second catalogue would be read out of
        // bounds. Binding the pair at construction removes the mismatch instead of
        // guarding against it.
        [[nodiscard]] static CommandTree Build(const Catalogue& catalogue);

        // The catalogue this tree indexes into.
        [[nodiscard]] const Catalogue& Commands() const
        {
            return *m_Catalogue;
        }

        // The entry one leaf names.
        [[nodiscard]] const CatalogueEntry& EntryAt(std::size_t index) const
        {
            return m_Catalogue->Entries[index];
        }

        [[nodiscard]] const std::vector<TreeGroup>& Groups() const
        {
            return m_Groups;
        }

        [[nodiscard]] const TreeGroup* FindGroup(const std::string& group) const;

        [[nodiscard]] Resolution Resolve(const std::string& group, const std::string& command) const;

        // Every (group, command) pair that more than one command derived, as
        // "group command" strings. Empty on a healthy surface; the CLI prints these
        // to stderr on every run rather than only when one is hit, because an
        // ambiguous pair is a defect in the registry that a user cannot see.
        [[nodiscard]] const std::vector<std::string>& AmbiguousPaths() const
        {
            return m_AmbiguousPaths;
        }

      private:
        // Never null after Build(); a default-constructed CommandTree is not a
        // thing the API hands out.
        const Catalogue* m_Catalogue = nullptr;
        std::vector<TreeGroup> m_Groups;
        std::vector<std::string> m_AmbiguousPaths;
    };
} // namespace OloCtl
