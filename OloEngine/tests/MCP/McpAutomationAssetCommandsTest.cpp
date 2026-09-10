// OLO_TEST_LAYER: unit

// The asset automation commands (issue #1128) against a real Project and a real
// EditorAssetManager in a throwaway directory -- the same bring-up
// FunctionalTest::EnableAssetManager uses, inlined here because these cases need
// the asset stack and none of the scene/runtime harness around it.
//
// The three cases the issue's acceptance criteria name are marked below. They
// are the reason this file exists; everything else guards a way one of them
// could pass while still losing somebody's data.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationAssetCommands.h"
#include "Automation/AutomationRegistry.h"
#include "MCP/McpServer.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include "OloEngine/Asset/AssetFileWatchPolicy.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "UndoRedo/EditorCommand.h"
#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>

namespace OloEngine::Automation::Tests
{
    namespace
    {
        using Json = nlohmann::json;

        // No editor and no transport, but a REAL CommandHistory: MarshalRead runs
        // the job inline, which is what the commands' own threading contract
        // allows (they marshal the registry reads and run the project scan between
        // them). The history is what makes olo_asset_move's undo entry reachable
        // without an editor -- and `HistoryAvailable` lets a case take it away
        // again, because a headless caller must get undoable:false rather than a
        // promise of a Ctrl-Z that does not exist.
        class AssetHost final : public IAutomationHost
        {
          public:
            AssetHost()
            {
                m_Context.GetCommandHistory = [this]() -> CommandHistory*
                { return HistoryAvailable ? &History : nullptr; };
            }

            [[nodiscard]] const MCP::EditorMcpContext& Context() const override
            {
                return m_Context;
            }

            CommandHistory History;
            bool HistoryAvailable = true;
            [[nodiscard]] bool IsCurrentCallCancelled() const override
            {
                return false;
            }
            [[nodiscard]] bool PublishArtifact(AutomationArtifact) override
            {
                return false;
            }

          protected:
            Json MarshalReadOnMainThread(const std::function<Json()>& job, std::chrono::milliseconds) override
            {
                return job();
            }
            void EmitProgressUpdate(f64, f64, const std::string&) const override {}

          private:
            MCP::EditorMcpContext m_Context;
        };

        void Write(const std::filesystem::path& path, const std::string& text)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(output) << "cannot create " << path.string();
            output << text;
        }

        std::string Read(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        }
    } // namespace

    class AutomationAssetCommandsTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Project = OloEngine::Tests::TempDir("assetcmd");
            std::error_code ec;
            std::filesystem::create_directories(m_Project / "Assets" / "Textures", ec);

            Write(TexturePath(), "not-really-a-png");
            Write(ScenePath(), "Scene: Test\n"
                               "Entities:\n"
                               "  - Entity: 900001\n"
                               "    TagComponent:\n"
                               "      Tag: Decal\n"
                               "    DecalComponent:\n"
                               "      AlbedoTexturePath: Assets/Textures/Checkerboard.png\n");

            Write(m_Project / "Test.oloproj", "Project:\n"
                                              "  Name: AssetCommandsTest\n"
                                              "  StartScene: \"\"\n"
                                              "  AssetDirectory: \"Assets\"\n"
                                              "  ScriptModulePath: \"\"\n");
            ASSERT_TRUE(Project::Load(m_Project / "Test.oloproj"));

            auto manager = Ref<EditorAssetManager>::Create();
            // startFileWatcher=false: the watcher thread outlives the case and
            // races the next one's manager (see EditorAssetManager::Initialize).
            manager->Initialize(false);
            Project::SetAssetManager(manager);

            RegisterAssetAuthoringCommands(m_Registry);
        }

        [[nodiscard]] std::filesystem::path TexturePath() const
        {
            return m_Project / "Assets" / "Textures" / "Checkerboard.png";
        }
        [[nodiscard]] std::filesystem::path ScenePath() const
        {
            return m_Project / "Assets" / "Scenes" / "Test.olo";
        }

        AutomationInvocation Call(const std::string& name, const Json& arguments)
        {
            return m_Registry.Invoke(m_Host, name, arguments, AutomationWriteConsent::Granted);
        }

        Json Success(const std::string& name, const Json& arguments)
        {
            auto result = Call(name, arguments);
            EXPECT_EQ(result.Outcome, AutomationInvocation::Status::Ok) << result.Message;
            EXPECT_FALSE(result.Result.IsError) << result.Result.Content.dump(2);
            return result.Result.StructuredContent;
        }

        std::filesystem::path m_Project;
        AssetHost m_Host;
        AutomationRegistry m_Registry;
    };

    // === Acceptance criterion 1 =============================================
    // "What references this asset" is answerable without opening the editor.
    TEST_F(AutomationAssetCommandsTest, ReferrersAreAnswerableWithoutTheEditor)
    {
        const Json result =
            Success("olo_asset_references", Json{ { "path", "Assets/Textures/Checkerboard.png" } });

        ASSERT_EQ(result.at("count").get<u32>(), 1u) << result.dump(2);
        const Json& reference = result.at("references").at(0);
        EXPECT_EQ(reference.at("key").get<std::string>(), "AlbedoTexturePath");
        EXPECT_EQ(reference.at("projectPath").get<std::string>(), "Assets/Scenes/Test.olo");
        EXPECT_EQ(reference.at("line").get<u32>(), 7u);

        // The boundary travels with the answer. An empty list must never be
        // readable as "nothing references this" on its own.
        const Json& coverage = result.at("coverage");
        EXPECT_GE(coverage.at("filesScanned").get<u32>(), 1u);
        EXPECT_FALSE(coverage.at("truncated").get<bool>());
        EXPECT_FALSE(coverage.at("note").get<std::string>().empty());
    }

    TEST_F(AutomationAssetCommandsTest, DependenciesAreTheOtherDirection)
    {
        const Json result = Success("olo_asset_references",
                                    Json{ { "path", "Assets/Scenes/Test.olo" }, { "direction", "dependencies" } });
        ASSERT_EQ(result.at("count").get<u32>(), 1u) << result.dump(2);
        EXPECT_EQ(result.at("references").at(0).at("key").get<std::string>(), "AlbedoTexturePath");
    }

    // === Acceptance criterion 2 =============================================
    // Moving an asset leaves every referring scene still resolving.
    TEST_F(AutomationAssetCommandsTest, MoveRewritesTheReferringSceneSoItStillResolves)
    {
        const Json imported = Success("olo_asset_import", Json{ { "path", "Assets/Textures/Checkerboard.png" } });
        const std::string handle = imported.at("handle").get<std::string>();
        ASSERT_NE(handle, "0");

        const Json moved = Success("olo_asset_move",
                                   Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                         { "destination", "Assets/Textures/Moved/Checkerboard.png" },
                                         { "createDirectories", true } });
        ASSERT_TRUE(moved.at("moved").get<bool>()) << moved.dump(2);
        EXPECT_EQ(moved.at("referencesRewritten").get<u32>(), 1u);
        // The handle is the asset's identity and every handle-shaped reference in
        // the project depends on it surviving. Removing and re-importing would
        // mint a fresh one and orphan them all silently.
        EXPECT_EQ(moved.at("handle").get<std::string>(), handle);

        EXPECT_FALSE(std::filesystem::exists(TexturePath()));
        EXPECT_TRUE(std::filesystem::exists(m_Project / "Assets" / "Textures" / "Moved" / "Checkerboard.png"));

        const std::string scene = Read(ScenePath());
        EXPECT_NE(scene.find("AlbedoTexturePath: Assets/Textures/Moved/Checkerboard.png"), std::string::npos)
            << "the reference must have been re-spelled; the scene is:\n"
            << scene;

        // And the proof that it RESOLVES, not merely that the text changed: ask
        // the index again at the new location and expect the same referrer back.
        const Json after = Success("olo_asset_references",
                                   Json{ { "path", "Assets/Textures/Moved/Checkerboard.png" } });
        EXPECT_EQ(after.at("count").get<u32>(), 1u) << after.dump(2);
    }

    // A move IS undoable -- every effect it has is a file write and a registry
    // key -- so the issue's "undo where the operation is undoable" applies, and
    // an undo has to take the REFERENCE REWRITES back too, not just the file.
    TEST_F(AutomationAssetCommandsTest, MoveIsOneUndoStepThatRestoresTheReferences)
    {
        const std::string sceneBefore = Read(ScenePath());
        const Json moved = Success("olo_asset_move",
                                   Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                         { "destination", "Assets/Textures/Moved/Checkerboard.png" },
                                         { "createDirectories", true } });
        ASSERT_TRUE(moved.at("undoable").get<bool>()) << moved.dump(2);
        ASSERT_TRUE(m_Host.History.CanUndo());

        m_Host.History.Undo();
        EXPECT_TRUE(std::filesystem::exists(TexturePath()));
        EXPECT_FALSE(std::filesystem::exists(m_Project / "Assets" / "Textures" / "Moved" / "Checkerboard.png"));
        EXPECT_EQ(Read(ScenePath()), sceneBefore)
            << "undoing a move must take the rewritten references back with it; leaving them pointing at the new "
               "location would be the same silent breakage the move exists to prevent";

        m_Host.History.Redo();
        EXPECT_FALSE(std::filesystem::exists(TexturePath()));
        EXPECT_NE(Read(ScenePath()).find("Assets/Textures/Moved/Checkerboard.png"), std::string::npos);
    }

    TEST_F(AutomationAssetCommandsTest, UndoRefusesWhenAReferringFileChangedUnderneath)
    {
        Success("olo_asset_move", Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                        { "destination", "Assets/Textures/Moved/Checkerboard.png" },
                                        { "createDirectories", true } });
        // Somebody edited the scene after the move. Undo must not clobber it.
        Write(ScenePath(), Read(ScenePath()) + "# a later hand edit\n");
        EXPECT_ANY_THROW(m_Host.History.Undo());
        EXPECT_NE(Read(ScenePath()).find("# a later hand edit"), std::string::npos)
            << "the hand edit must survive; a guard that writes anyway is worse than no guard";
    }

    TEST_F(AutomationAssetCommandsTest, AHeadlessHostReportsTheMoveAsNotUndoable)
    {
        m_Host.HistoryAvailable = false;
        const Json moved = Success("olo_asset_move",
                                   Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                         { "destination", "Assets/Textures/Moved/Checkerboard.png" },
                                         { "createDirectories", true } });
        EXPECT_TRUE(moved.at("moved").get<bool>());
        EXPECT_FALSE(moved.at("undoable").get<bool>())
            << "no editor history means no Ctrl-Z; claiming otherwise would be a promise the command cannot keep";
    }

    TEST_F(AutomationAssetCommandsTest, MoveLeavesTheRestOfTheSceneByteIdentical)
    {
        const std::string before = Read(ScenePath());
        Success("olo_asset_move", Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                        { "destination", "Assets/Textures/Moved/Checkerboard.png" },
                                        { "createDirectories", true } });
        std::string expected = before;
        const auto at = expected.find("Assets/Textures/Checkerboard.png");
        ASSERT_NE(at, std::string::npos);
        expected.replace(at, std::string("Assets/Textures/Checkerboard.png").size(),
                         "Assets/Textures/Moved/Checkerboard.png");
        EXPECT_EQ(Read(ScenePath()), expected)
            << "a move edits the reference span and nothing else -- re-emitting the file through a YAML writer "
               "would reformat it and destroy its comments and layout";
    }

    // === Acceptance criterion 3 =============================================
    // Deleting a referenced asset is refused by default and names its referrers.
    TEST_F(AutomationAssetCommandsTest, DeleteIsRefusedByDefaultAndNamesItsReferrers)
    {
        const Json result = Success("olo_asset_delete", Json{ { "path", "Assets/Textures/Checkerboard.png" } });

        EXPECT_FALSE(result.at("deleted").get<bool>());
        ASSERT_TRUE(result.at("refused").get<bool>()) << result.dump(2);
        EXPECT_EQ(result.at("reason").get<std::string>(), "referenced");
        ASSERT_EQ(result.at("referrerCount").get<u32>(), 1u);
        EXPECT_EQ(result.at("referrers").at(0).at("projectPath").get<std::string>(), "Assets/Scenes/Test.olo");
        EXPECT_TRUE(std::filesystem::exists(TexturePath())) << "a refusal must not have deleted anything";
    }

    TEST_F(AutomationAssetCommandsTest, ForcedDeleteProceedsAndNamesWhatItBroke)
    {
        const Json result =
            Success("olo_asset_delete", Json{ { "path", "Assets/Textures/Checkerboard.png" }, { "force", true } });

        ASSERT_TRUE(result.at("deleted").get<bool>()) << result.dump(2);
        EXPECT_FALSE(result.at("refused").get<bool>());
        EXPECT_TRUE(result.at("forced").get<bool>());
        // Counting is not enough: the caller has to be able to go and fix them.
        ASSERT_EQ(result.at("referencesBroken").get<u32>(), 1u);
        EXPECT_EQ(result.at("brokenReferences").at(0).at("line").get<u32>(), 7u);
        EXPECT_FALSE(std::filesystem::exists(TexturePath()));
    }

    TEST_F(AutomationAssetCommandsTest, DeletingAnUnreferencedAssetNeedsNoForce)
    {
        Write(m_Project / "Assets" / "Textures" / "Lonely.png", "nobody-points-here");
        const Json result = Success("olo_asset_delete", Json{ { "path", "Assets/Textures/Lonely.png" } });
        ASSERT_TRUE(result.at("deleted").get<bool>()) << result.dump(2);
        EXPECT_EQ(result.at("referencesBroken").get<u32>(), 0u);
    }

    // --- the ways a move could half-happen ----------------------------------

    // There is no directory command on this surface, so refusing a missing
    // parent outright would make "move this into a new folder" impossible rather
    // than two-step. Creating one for a typo'd path silently is the other
    // failure, so it is an opt-in -- and the refusal has to name the flag, or the
    // caller is left guessing.
    TEST_F(AutomationAssetCommandsTest, MoveRefusesAMissingDestinationDirectoryAndNamesTheFlag)
    {
        const auto result = Call("olo_asset_move",
                                 Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                       { "destination", "Assets/Textures/Nope/Checkerboard.png" } });
        ASSERT_TRUE(result.Result.IsError) << result.Result.Content.dump(2);
        EXPECT_NE(result.Result.Content.dump().find("createDirectories"), std::string::npos)
            << "the refusal must say how to proceed; message was: " << result.Result.Content.dump();
        EXPECT_TRUE(std::filesystem::exists(TexturePath()));
        EXPECT_FALSE(std::filesystem::exists(m_Project / "Assets" / "Textures" / "Nope"));
    }

    TEST_F(AutomationAssetCommandsTest, MoveRefusesAnExistingDestination)
    {
        Write(m_Project / "Assets" / "Textures" / "Taken.png", "already-here");
        const auto result = Call("olo_asset_move", Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                                         { "destination", "Assets/Textures/Taken.png" } });
        EXPECT_TRUE(result.Result.IsError) << result.Result.Content.dump(2);
        EXPECT_TRUE(std::filesystem::exists(TexturePath()));
        EXPECT_EQ(Read(m_Project / "Assets" / "Textures" / "Taken.png"), "already-here");
    }

    TEST_F(AutomationAssetCommandsTest, MoveRefusesADestinationOutsideTheAssetDirectory)
    {
        // A file written outside the asset directory is not an asset: the
        // registry will not track it and the watcher will not see it. Landing one
        // there looks like success and is not.
        const auto result = Call("olo_asset_move", Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                                         { "destination", "Outside.png" } });
        EXPECT_TRUE(result.Result.IsError) << result.Result.Content.dump(2);
        EXPECT_TRUE(std::filesystem::exists(TexturePath()));
    }

    TEST_F(AutomationAssetCommandsTest, MoveDoesNotTouchTheSceneWhenTheDestinationIsRejected)
    {
        const std::string before = Read(ScenePath());
        (void)Call("olo_asset_move",
                   Json{ { "path", "Assets/Textures/Checkerboard.png" }, { "destination", "Outside.png" } });
        EXPECT_EQ(Read(ScenePath()), before) << "validation happens before any file is written";
    }

    // --- import -------------------------------------------------------------

    TEST_F(AutomationAssetCommandsTest, ImportIsIdempotentAndSaysItCannotBeUndone)
    {
        const Json first = Success("olo_asset_import", Json{ { "path", "Assets/Textures/Checkerboard.png" } });
        const Json second = Success("olo_asset_import", Json{ { "path", "Assets/Textures/Checkerboard.png" } });
        EXPECT_EQ(first.at("handle").get<std::string>(), second.at("handle").get<std::string>());
        EXPECT_TRUE(second.at("alreadyRegistered").get<bool>());
        // The issue is explicit that an import is not undoable. Saying so in the
        // payload as well as in the declared metadata means an agent that reads
        // only the result still learns it.
        EXPECT_FALSE(first.at("undoable").get<bool>());
    }

    TEST_F(AutomationAssetCommandsTest, ImportDeclaresItselfIrreversible)
    {
        const auto snapshot = m_Registry.Snapshot();
        const AutomationCommand* command = AutomationRegistry::Find(*snapshot, "olo_asset_import");
        ASSERT_NE(command, nullptr);
        EXPECT_EQ(command->Undo, AutomationUndo::Irreversible)
            << "an import mutates the persisted registry; declaring anything else would be a promise the command "
               "cannot keep";
        EXPECT_TRUE(command->ProjectWrite);
    }

    TEST_F(AutomationAssetCommandsTest, QueryCommandsAreReadOnlyAndNeedNoConsent)
    {
        const auto snapshot = m_Registry.Snapshot();
        for (const char* name : { "olo_asset_get", "olo_asset_references" })
        {
            const AutomationCommand* command = AutomationRegistry::Find(*snapshot, name);
            ASSERT_NE(command, nullptr) << name;
            EXPECT_FALSE(command->ProjectWrite) << name << " reads only; gating it behind write consent would make "
                                                           "the safety check harder to reach than the delete it "
                                                           "protects";
            EXPECT_EQ(command->Undo, AutomationUndo::None) << name;
        }
    }

    TEST_F(AutomationAssetCommandsTest, DestructiveCommandsRefuseWithoutWriteConsent)
    {
        const auto refused = m_Registry.Invoke(m_Host, "olo_asset_delete",
                                               Json{ { "path", "Assets/Textures/Checkerboard.png" } },
                                               AutomationWriteConsent::Withheld);
        EXPECT_EQ(refused.Outcome, AutomationInvocation::Status::WriteConsentWithheld);
        EXPECT_TRUE(std::filesystem::exists(TexturePath()));
    }

    // --- import settings ----------------------------------------------------

    TEST_F(AutomationAssetCommandsTest, ImportSettingsRoundTripThroughASidecar)
    {
        const Json empty = Success("olo_asset_import_settings", Json{ { "path", "Assets/Textures/Checkerboard.png" } });
        EXPECT_FALSE(empty.at("exists").get<bool>());
        EXPECT_TRUE(empty.at("settings").empty());

        const Json written = Success("olo_asset_import_settings",
                                     Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                           { "settings", Json{ { "flipUV", true }, { "srgb", false } } } });
        EXPECT_TRUE(written.at("changed").get<bool>());
        // Nothing consumes these yet, and the result says so rather than letting
        // a caller believe a setting took effect.
        EXPECT_FALSE(written.at("appliedByImporter").get<bool>());

        // A second write MERGES: setting one key must not drop the others.
        const Json merged = Success("olo_asset_import_settings",
                                    Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                          { "settings", Json{ { "srgb", true } } } });
        EXPECT_TRUE(merged.at("settings").at("flipUV").get<bool>());
        EXPECT_TRUE(merged.at("settings").at("srgb").get<bool>());

        // ...and a null value is how a key is removed. Explicit, not inferred.
        const Json removed = Success("olo_asset_import_settings",
                                     Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                           { "settings", Json{ { "flipUV", nullptr } } } });
        EXPECT_FALSE(removed.at("settings").contains("flipUV"));
        EXPECT_TRUE(removed.at("settings").at("srgb").get<bool>());
    }

    // The sidecar lands next to an asset inside Assets/, which is the directory
    // the hot-reload watcher is watching. It is safe only because ".oloimport" is
    // absent from the extension map, so DecideFileWatchAction resolves it to
    // Ignore. If somebody ever registers that extension, this fails here rather
    // than as a mystery auto-import in a live session.
    TEST_F(AutomationAssetCommandsTest, TheSidecarExtensionIsInvisibleToTheHotReloadWatcher)
    {
        Success("olo_asset_import_settings",
                Json{ { "path", "Assets/Textures/Checkerboard.png" }, { "settings", Json{ { "flipUV", true } } } });
        const auto sidecar = m_Project / "Assets" / "Textures" / "Checkerboard.png.oloimport";
        ASSERT_TRUE(std::filesystem::exists(sidecar));

        const FileWatchDecisionInput input{ .IsPresenceEvent = true,
                                            .Type = AssetExtensions::GetAssetTypeFromPath(sidecar.string()),
                                            .ExistsAsRegularFile = true,
                                            .AlreadyTracked = false,
                                            .CurrentlyLoaded = false };
        EXPECT_EQ(input.Type, AssetType::None);
        EXPECT_EQ(DecideFileWatchAction(input), FileWatchAction::Ignore);
    }

    // The sidecar is part of the asset. One left behind by a move stops applying;
    // one left behind by a delete silently reattaches to whatever is created at
    // that path next, which is a stranger failure than losing it outright.
    TEST_F(AutomationAssetCommandsTest, MoveCarriesTheImportSettingsSidecar)
    {
        Success("olo_asset_import_settings",
                Json{ { "path", "Assets/Textures/Checkerboard.png" }, { "settings", Json{ { "flipUV", true } } } });
        Success("olo_asset_move", Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                        { "destination", "Assets/Textures/Moved/Checkerboard.png" },
                                        { "createDirectories", true } });

        EXPECT_FALSE(std::filesystem::exists(m_Project / "Assets" / "Textures" / "Checkerboard.png.oloimport"));
        const Json settings = Success("olo_asset_import_settings",
                                      Json{ { "path", "Assets/Textures/Moved/Checkerboard.png" } });
        EXPECT_TRUE(settings.at("exists").get<bool>());
        EXPECT_TRUE(settings.at("settings").at("flipUV").get<bool>());
    }

    TEST_F(AutomationAssetCommandsTest, DeleteTakesTheImportSettingsSidecarWithIt)
    {
        Write(m_Project / "Assets" / "Textures" / "Lonely.png", "nobody-points-here");
        Success("olo_asset_import_settings",
                Json{ { "path", "Assets/Textures/Lonely.png" }, { "settings", Json{ { "flipUV", true } } } });

        const Json deleted = Success("olo_asset_delete", Json{ { "path", "Assets/Textures/Lonely.png" } });
        ASSERT_TRUE(deleted.at("deleted").get<bool>()) << deleted.dump(2);
        EXPECT_TRUE(deleted.at("importSettingsRemoved").get<bool>());
        EXPECT_FALSE(std::filesystem::exists(m_Project / "Assets" / "Textures" / "Lonely.png.oloimport"));
    }

    TEST_F(AutomationAssetCommandsTest, ImportSettingsRefuseToOverwriteAMalformedSidecar)
    {
        Write(m_Project / "Assets" / "Textures" / "Checkerboard.png.oloimport", "this is not json");
        const auto result = Call("olo_asset_import_settings", Json{ { "path", "Assets/Textures/Checkerboard.png" },
                                                                    { "settings", Json{ { "flipUV", true } } } });
        EXPECT_TRUE(result.Result.IsError) << result.Result.Content.dump(2);
        EXPECT_EQ(Read(m_Project / "Assets" / "Textures" / "Checkerboard.png.oloimport"), "this is not json")
            << "an unreadable sidecar is a human's problem; clobbering it would destroy whatever they meant to put "
               "there";
    }

    // --- create -------------------------------------------------------------

    TEST_F(AutomationAssetCommandsTest, CreateNamesTheTypesItSupportsWhenItRefuses)
    {
        const auto result =
            Call("olo_asset_create", Json{ { "type", "Texture2D" }, { "destination", "Assets/Nope.png" } });
        ASSERT_TRUE(result.Result.IsError);
        const std::string message = result.Result.Content.dump();
        EXPECT_NE(message.find("Material"), std::string::npos)
            << "a refusal that does not say what IS possible sends the caller guessing; message was: " << message;
    }

    // CreateOrReplaceAsset stamps the type from its template argument and never
    // looks at the extension, so a Material at "foo.png" would serialize fine and
    // be re-registered as a Texture2D by the next directory scan -- an asset that
    // silently changes type. Caught at the door instead.
    TEST_F(AutomationAssetCommandsTest, CreateRefusesAnExtensionThatMeansADifferentType)
    {
        const auto result = Call("olo_asset_create",
                                 Json{ { "type", "Material" }, { "destination", "Assets/Wrong.png" } });
        ASSERT_TRUE(result.Result.IsError) << result.Result.Content.dump(2);
        EXPECT_FALSE(std::filesystem::exists(m_Project / "Assets" / "Wrong.png"));
    }

    // MaterialAsset's constructor resolves a shader out of
    // Renderer3D::GetShaderLibrary() and ASSERTS when it finds none, so on a host
    // with no shaders loaded, calling the factory would abort the process rather
    // than return an error. The command checks first.
    //
    // Deliberately asserts the CONTRACT rather than one of the two outcomes: the
    // shader library is process-wide state that another test in the same binary
    // may have populated (see cross-test-renderer-state.md), so "no shaders
    // loaded" is not something a case running in a shared process can assume.
    // Pinning either outcome makes this pass alone and fail in a full run -- which
    // is exactly what it did. What must hold either way is that it never aborts,
    // and that a refusal says what is missing.
    TEST_F(AutomationAssetCommandsTest, CreateEitherWritesAMaterialOrSaysWhyItCannot)
    {
        const auto result =
            Call("olo_asset_create", Json{ { "type", "Material" }, { "destination", "Assets/New.olomaterial" } });
        ASSERT_EQ(result.Outcome, AutomationInvocation::Status::Ok) << result.Message;

        if (result.Result.IsError)
        {
            EXPECT_NE(result.Result.Content.dump().find("shader library"), std::string::npos)
                << "a refusal must name what is missing; message was: " << result.Result.Content.dump();
            EXPECT_FALSE(std::filesystem::exists(m_Project / "Assets" / "New.olomaterial"))
                << "a refusal must not have written anything";
        }
        else
        {
            const Json created = result.Result.StructuredContent;
            EXPECT_TRUE(created.at("created").get<bool>()) << created.dump(2);
            EXPECT_TRUE(created.at("registered").get<bool>());
            EXPECT_NE(created.at("handle").get<std::string>(), "0");
            EXPECT_TRUE(std::filesystem::exists(m_Project / "Assets" / "New.olomaterial"));
        }
    }

    // A type with no such dependency still creates headlessly, so the command is
    // not merely refusing everything.
    TEST_F(AutomationAssetCommandsTest, CreateWritesAnInstancePlacementAssetAndRegistersIt)
    {
        const Json created = Success("olo_asset_create", Json{ { "type", "InstancePlacement" },
                                                               { "destination", "Assets/New.oloinstances" } });
        ASSERT_TRUE(created.at("created").get<bool>()) << created.dump(2);
        EXPECT_TRUE(created.at("registered").get<bool>());
        EXPECT_NE(created.at("handle").get<std::string>(), "0");
        EXPECT_TRUE(std::filesystem::exists(m_Project / "Assets" / "New.oloinstances"));
    }

    TEST_F(AutomationAssetCommandsTest, CreateRefusesOutsideTheAssetDirectory)
    {
        const auto result =
            Call("olo_asset_create", Json{ { "type", "Material" }, { "destination", "../Escaped.olomaterial" } });
        EXPECT_TRUE(result.Result.IsError) << result.Result.Content.dump(2);
    }

    // --- get ----------------------------------------------------------------

    TEST_F(AutomationAssetCommandsTest, GetAnswersForAnUnregisteredFileToo)
    {
        // "What is this file, and what points at it" has to work BEFORE anything
        // imported it -- that is exactly when somebody is about to delete it.
        const Json result = Success("olo_asset_get", Json{ { "path", "Assets/Textures/Checkerboard.png" } });
        EXPECT_TRUE(result.at("existsOnDisk").get<bool>());
        EXPECT_EQ(result.at("name").get<std::string>(), "Checkerboard.png");
        EXPECT_EQ(result.at("type").get<std::string>(), "Texture2D");
    }

    TEST_F(AutomationAssetCommandsTest, GetRejectsBothOrNeitherSelector)
    {
        EXPECT_TRUE(Call("olo_asset_get", Json::object()).Result.IsError);
        EXPECT_TRUE(Call("olo_asset_get", Json{ { "handle", "1" }, { "path", "x" } }).Result.IsError);
    }
} // namespace OloEngine::Automation::Tests
