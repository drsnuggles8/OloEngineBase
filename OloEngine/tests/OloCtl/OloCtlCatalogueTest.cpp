// OLO_TEST_LAYER: unit
//
// Parsing a command catalogue (issue #1125).
//
// oloctl learns everything it can do from this array, so the two properties that
// matter are: an entry it CAN read becomes a command, and an entry it CANNOT
// read is reported rather than dropped. The second is the one worth a test —
// a silently skipped entry looks exactly like a command that was never
// registered, which is the failure a generated frontend exists to rule out.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationCatalogue.h"
#include "OloCtl/CommandCatalogue.h"

#include <string>

using OloCtl::AuthorityClass;
using OloCtl::Catalogue;
using OloCtl::CatalogueEntry;
using OloCtl::ParseCatalogue;

using Json = nlohmann::json;

namespace
{
    Json MakeEntry(const std::string& name, bool projectWrite)
    {
        return Json{ { "name", name },
                     { "title", "Title of " + name },
                     { "description", "What " + name + " does." },
                     { "inputSchema", { { "type", "object" } } },
                     { "_meta", { { "io.oloengine/toolset", "scene" } } },
                     { "projectWrite", projectWrite } };
    }
} // namespace

TEST(OloCtlCatalogue, ReadsTheFieldsTheCliNeeds)
{
    const Catalogue catalogue = ParseCatalogue(Json::array({ MakeEntry("olo_scene_summary", false) }), "test");

    ASSERT_EQ(catalogue.Entries.size(), 1u);
    const CatalogueEntry& entry = catalogue.Entries.front();
    EXPECT_EQ(entry.Name, "olo_scene_summary");
    EXPECT_EQ(entry.Title, "Title of olo_scene_summary");
    EXPECT_EQ(entry.Toolset, "scene");
    EXPECT_EQ(entry.Authority, AuthorityClass::ReadOnly);
    EXPECT_TRUE(entry.ListedByHost);
    EXPECT_TRUE(catalogue.Rejected.empty());
    EXPECT_EQ(catalogue.Source, "test");
}

// The distinction the write gate is built on. `Unknown` is not a synonym for
// read-only: an editor whose catalogue predates the `projectWrite` field must
// make oloctl refuse, not assume.
TEST(OloCtlCatalogue, AnAbsentProjectWriteFlagIsUnknownRatherThanReadOnly)
{
    Json entry = MakeEntry("olo_scene_summary", false);
    entry.erase("projectWrite");

    const Catalogue catalogue = ParseCatalogue(Json::array({ entry }), "test");

    ASSERT_EQ(catalogue.Entries.size(), 1u);
    EXPECT_EQ(catalogue.Entries.front().Authority, AuthorityClass::Unknown);
}

TEST(OloCtlCatalogue, AProjectWriteFlagIsCarriedThrough)
{
    const Catalogue catalogue = ParseCatalogue(Json::array({ MakeEntry("olo_scene_open", true) }), "test");

    ASSERT_EQ(catalogue.Entries.size(), 1u);
    EXPECT_EQ(catalogue.Entries.front().Authority, AuthorityClass::ProjectWrite);
}

TEST(OloCtlCatalogue, AnUnreadableEntryIsReportedRatherThanDropped)
{
    Json entries = Json::array({ MakeEntry("olo_scene_summary", false),
                                 Json{ { "description", "no name" } },
                                 Json("a bare string"),
                                 MakeEntry("olo_scene_summary", false) });

    const Catalogue catalogue = ParseCatalogue(entries, "test");

    EXPECT_EQ(catalogue.Entries.size(), 1u);
    ASSERT_EQ(catalogue.Rejected.size(), 3u);
    EXPECT_EQ(catalogue.Rejected[0].Index, 1u);
    EXPECT_NE(catalogue.Rejected[0].Reason.find("name"), std::string::npos);
    EXPECT_EQ(catalogue.Rejected[1].Index, 2u);
    EXPECT_EQ(catalogue.Rejected[2].Index, 3u);
    EXPECT_EQ(catalogue.Rejected[2].Name, "olo_scene_summary");
    EXPECT_NE(catalogue.Rejected[2].Reason.find("duplicate"), std::string::npos);
}

TEST(OloCtlCatalogue, AWholeAnswerOfTheWrongShapeIsReportedRatherThanThrowing)
{
    const Catalogue catalogue = ParseCatalogue(Json{ { "tools", Json::array() } }, "test");

    EXPECT_TRUE(catalogue.Entries.empty());
    ASSERT_EQ(catalogue.Rejected.size(), 1u);
    EXPECT_NE(catalogue.Rejected.front().Reason.find("array"), std::string::npos);
}

// PRESENT but the wrong type is a rejection, not a default. Substituting the empty
// object schema would accept a description this build cannot represent and then bind
// every argument as untyped — a silent fallback dressed as a sensible default.
TEST(OloCtlCatalogue, ASchemaFieldOfTheWrongTypeIsRejectedRatherThanDefaulted)
{
    Json badInput = MakeEntry("olo_bad_input", false);
    badInput["inputSchema"] = "not an object";
    Json badOutput = MakeEntry("olo_bad_output", false);
    badOutput["outputSchema"] = Json::array({ 1, 2 });

    const Catalogue catalogue = ParseCatalogue(Json::array({ badInput, badOutput }), "test");

    EXPECT_TRUE(catalogue.Entries.empty());
    ASSERT_EQ(catalogue.Rejected.size(), 2u);
    EXPECT_EQ(catalogue.Rejected[0].Name, "olo_bad_input");
    EXPECT_NE(catalogue.Rejected[0].Reason.find("inputSchema"), std::string::npos);
    EXPECT_EQ(catalogue.Rejected[1].Name, "olo_bad_output");
    EXPECT_NE(catalogue.Rejected[1].Reason.find("outputSchema"), std::string::npos);
}

TEST(OloCtlCatalogue, AMissingInputSchemaBecomesTheEmptyObjectSchema)
{
    Json entry = MakeEntry("olo_screenshot", false);
    entry.erase("inputSchema");

    const Catalogue catalogue = ParseCatalogue(Json::array({ entry }), "test");

    ASSERT_EQ(catalogue.Entries.size(), 1u);
    EXPECT_EQ(catalogue.Entries.front().InputSchema, (Json{ { "type", "object" } }));
}

// The two spellings of the toolset `_meta` key live in different trees on
// purpose — the oloctl core compiles against nothing from the engine — so
// nothing but this assertion keeps them together. A rename on either side that
// misses the other would leave every command in the group `misc` with no error.
TEST(OloCtlCatalogue, TheToolsetMetaKeyMatchesTheOneTheRegistryEmits)
{
    OloEngine::Automation::AutomationCommand command;
    command.Name = "olo_fake_toolset_probe";
    command.Toolset = "render";
    command.InputSchema = Json{ { "type", "object" } };

    const Json described = OloEngine::Automation::DescribeForFrontend(command);
    const Catalogue catalogue = ParseCatalogue(Json::array({ described }), "test");

    ASSERT_EQ(catalogue.Entries.size(), 1u);
    EXPECT_EQ(catalogue.Entries.front().Toolset, "render")
        << "OloCtl's kToolsetMetaKey no longer matches Automation::kToolsetMetaKey";
    EXPECT_EQ(catalogue.Entries.front().Authority, AuthorityClass::ReadOnly)
        << "OloCtl's kProjectWriteKey no longer matches Automation::kProjectWriteKey";
}
