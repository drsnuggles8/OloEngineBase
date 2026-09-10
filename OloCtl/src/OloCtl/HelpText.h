#pragma once

// `--help`, rendered from the catalogue's schemas (issue #1125).
//
// Every word about a command — its summary, its options, their types and
// descriptions — comes out of the InputSchema the command declared. There is no
// help text stored here for any command, which is what makes `--help` correct
// for a command this build of oloctl has never heard of.
//
// Only the frame is static: the usage line, the connection options, and the
// four built-in subcommands, which belong to the CLI rather than the registry.

#include "OloCtl/CommandCatalogue.h"
#include "OloCtl/CommandTree.h"

#include <string>

namespace OloCtl
{
    // Usage, global options, built-in subcommands, then the group table.
    [[nodiscard]] std::string RenderRootHelp(const Catalogue& catalogue, const CommandTree& tree);

    // The commands in one group, each with a one-line summary from its description.
    [[nodiscard]] std::string RenderGroupHelp(const Catalogue& catalogue, const TreeGroup& group);

    // One command in full: description, authority class, every option with its
    // declared type, whether it is required, its enum values and its description.
    [[nodiscard]] std::string RenderCommandHelp(const CatalogueEntry& entry, const DerivedPath& path);

    // The usage frame with no catalogue behind it, for `oloctl --help` when the
    // editor cannot be reached. Says plainly that the command list is missing and
    // why, rather than printing an empty tree that looks like an editor with no
    // commands.
    [[nodiscard]] std::string RenderOfflineHelp(const std::string& connectionError);
} // namespace OloCtl
