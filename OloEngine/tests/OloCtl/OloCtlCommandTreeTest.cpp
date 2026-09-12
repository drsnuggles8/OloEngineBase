// OLO_TEST_LAYER: unit
//
// Deriving `oloctl scene list-entities` from `olo_scene_list_entities` (#1125).
//
// The derivation has to be TOTAL — every registered command must land at some
// reachable spelling, including ones whose shape nobody anticipated — and it has
// to be unambiguous over the surface that actually exists. Both are asserted
// here, the second against the real registered commands rather than a sample,
// because it is a property of the registry that only a sweep can check.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationCatalogue.h"
#include "Automation/AutomationRegistry.h"
#include "MCP/McpTools.h"
#include "OloCtl/CommandCatalogue.h"
#include "OloCtl/CommandTree.h"

#include <set>
#include <string>

using OloCtl::Catalogue;
using OloCtl::CatalogueEntry;
using OloCtl::CommandTree;
using OloCtl::DerivedPath;
using OloCtl::DerivePath;
using OloCtl::ParseCatalogue;
using OloCtl::TreeGroup;

using OloEngine::Automation::AutomationCommand;
using OloEngine::Automation::AutomationRegistry;
using OloEngine::Automation::DescribeCatalogue;

using Json = nlohmann::json;

namespace
{
    CatalogueEntry Entry(std::string name, std::string toolset)
    {
        CatalogueEntry entry;
        entry.Name = std::move(name);
        entry.Toolset = std::move(toolset);
        entry.InputSchema = Json{ { "type", "object" } };
        entry.Authority = OloCtl::AuthorityClass::ReadOnly;
        return entry;
    }

    Catalogue ProductionCatalogue(AutomationRegistry& registry)
    {
        OloEngine::MCP::RegisterBuiltinCommands(registry);
        return ParseCatalogue(DescribeCatalogue(*registry.Snapshot()), "production surface");
    }
} // namespace

TEST(OloCtlCommandTree, TheToolsetIsTheGroupAndTheNameLosesItsPrefix)
{
    const DerivedPath path = DerivePath(Entry("olo_scene_list_entities", "scene"));
    EXPECT_EQ(path.Group, "scene");
    EXPECT_EQ(path.Command, "list-entities");
}

TEST(OloCtlCommandTree, ANameThatDoesNotRepeatItsToolsetKeepsAllOfIt)
{
    // olo_screenshot lives in the camera toolset; nothing to strip.
    const DerivedPath path = DerivePath(Entry("olo_screenshot", "camera"));
    EXPECT_EQ(path.Group, "camera");
    EXPECT_EQ(path.Command, "screenshot");

    // A partial prefix match must not be stripped: `scen` is not `scene`.
    const DerivedPath partial = DerivePath(Entry("olo_scenery_list", "scene"));
    EXPECT_EQ(partial.Group, "scene");
    EXPECT_EQ(partial.Command, "scenery-list");
}

TEST(OloCtlCommandTree, ACommandWithNoToolsetFallsBackToItsOwnFirstSegment)
{
    const DerivedPath path = DerivePath(Entry("olo_physics_raycast", ""));
    EXPECT_EQ(path.Group, "physics");
    EXPECT_EQ(path.Command, "raycast");
}

TEST(OloCtlCommandTree, ASingleSegmentCommandWithNoToolsetStillLandsSomewhere)
{
    const DerivedPath path = DerivePath(Entry("olo_screenshot", ""));
    EXPECT_EQ(path.Group, "misc");
    EXPECT_EQ(path.Command, "screenshot");
}

// A bridged command from an external MCP server (#673) is named `ext.<alias>.<tool>`
// and carries no toolset. It has to be reachable too.
TEST(OloCtlCommandTree, ABridgedCommandIsReachable)
{
    const DerivedPath path = DerivePath(Entry("ext.files.read_file", ""));
    EXPECT_EQ(path.Group, "ext");
    EXPECT_EQ(path.Command, "files-read-file");
}

TEST(OloCtlCommandTree, ACollisionIsReportedAndBothCommandsStayReachableByName)
{
    Catalogue catalogue;
    catalogue.Entries.push_back(Entry("olo_render_set", "render"));
    catalogue.Entries.push_back(Entry("olo_set", "render"));

    const CommandTree tree = CommandTree::Build(catalogue);

    ASSERT_EQ(tree.AmbiguousPaths().size(), 1u);
    EXPECT_EQ(tree.AmbiguousPaths().front(), "render set");

    const CommandTree::Resolution resolution = tree.Resolve("render", "set");
    EXPECT_EQ(resolution.Status, CommandTree::ResolveStatus::Ambiguous);
    EXPECT_EQ(resolution.Candidates.size(), 2u);
    // Neither was dropped: both are still findable by their registry name, which
    // is what `oloctl call` uses.
    EXPECT_NE(catalogue.FindByName("olo_render_set"), nullptr);
    EXPECT_NE(catalogue.FindByName("olo_set"), nullptr);
}

TEST(OloCtlCommandTree, ResolutionDistinguishesAnUnknownGroupFromAnUnknownCommand)
{
    Catalogue catalogue;
    catalogue.Entries.push_back(Entry("olo_scene_summary", "scene"));
    const CommandTree tree = CommandTree::Build(catalogue);

    EXPECT_EQ(tree.Resolve("physics", "raycast").Status, CommandTree::ResolveStatus::UnknownGroup);
    EXPECT_EQ(tree.Resolve("scene", "nope").Status, CommandTree::ResolveStatus::UnknownCommand);
    EXPECT_EQ(tree.Resolve("scene", "summary").Status, CommandTree::ResolveStatus::Ok);
}

// ---- the real surface -------------------------------------------------------

// Every registered command must be typeable. A command that derived an empty
// group or an empty leaf would be invisible in the tree, and only `oloctl call`
// would reach it.
TEST(OloCtlCommandTree, EveryProductionCommandDerivesANonEmptySpelling)
{
    AutomationRegistry registry;
    const Catalogue catalogue = ProductionCatalogue(registry);
    ASSERT_GE(catalogue.Entries.size(), 80u) << "the production command surface failed to register";
    EXPECT_TRUE(catalogue.Rejected.empty()) << "oloctl could not read part of the registry's own catalogue";

    for (const CatalogueEntry& entry : catalogue.Entries)
    {
        const DerivedPath path = DerivePath(entry);
        EXPECT_FALSE(path.Group.empty()) << entry.Name;
        EXPECT_FALSE(path.Command.empty()) << entry.Name;
    }
}

// The ratchet. Two commands deriving the same `oloctl <group> <command>` makes
// that spelling ambiguous, and oloctl refuses it rather than picking. Adding a
// command that collides is the moment to rename it — before it ships, not after
// a user hits the refusal.
TEST(OloCtlCommandTree, NoTwoProductionCommandsDeriveTheSameSpelling)
{
    AutomationRegistry registry;
    const Catalogue catalogue = ProductionCatalogue(registry);
    const CommandTree tree = CommandTree::Build(catalogue);

    std::string collisions;
    for (const std::string& path : tree.AmbiguousPaths())
        collisions += "\n  oloctl " + path;
    EXPECT_TRUE(tree.AmbiguousPaths().empty())
        << "these CLI spellings are claimed by more than one registered command:" << collisions
        << "\nRename one of the colliding commands, or accept that `oloctl call <registry-name>` is the "
           "only way to reach them.";
}

// Every group must be typeable too: a group named like one of oloctl's own
// subcommands can never be reached by typing it.
TEST(OloCtlCommandTree, NoProductionGroupShadowsAnOloCtlSubcommand)
{
    AutomationRegistry registry;
    const Catalogue catalogue = ProductionCatalogue(registry);
    const CommandTree tree = CommandTree::Build(catalogue);

    // Keep in step with ReportCatalogue's shadow list in OloCtl/CliRunner.cpp.
    const std::set<std::string> reserved{ "help", "version", "catalogue", "call", "events" };
    for (const TreeGroup& group : tree.Groups())
        EXPECT_EQ(reserved.count(group.Name), 0u) << "group `" << group.Name << "` shadows an oloctl subcommand";
}
