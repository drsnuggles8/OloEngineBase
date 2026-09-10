// OLO_TEST_LAYER: unit

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationSceneCommands.h"
#include "Automation/AutomationSceneDocument.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "UndoRedo/EntityCommands.h"
#include "TestTempDir.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace OloEngine::Automation::Tests
{
    namespace
    {
        using Json = nlohmann::json;

        // The production document seam with real Scene, CommandHistory and disk.
        // Only editor panel/renderer installation is replaced; the separate live
        // editor verification covers those EditorLayer-owned callbacks.
        class DocumentHost final : public IAutomationHost
        {
          public:
            explicit DocumentHost(std::filesystem::path assetRoot)
                : Document(CaptureSceneDocument(Ref<Scene>::Create())), AssetRoot(std::move(assetRoot))
            {
                ContextData.GetActiveScene = [this]()
                { return Document.SceneRef; };
                ContextData.GetCommandHistory = [this]() -> CommandHistory*
                { return EditMode ? &History : nullptr; };
                ContextData.SceneDocument.Capture = [this]()
                {
                    auto current = CaptureSceneDocument(Document.SceneRef, Document.Path);
                    current.Selection = Document.Selection;
                    current.RenderedSettings = Document.RenderedSettings;
                    return current;
                };
                ContextData.SceneDocument.Install = [this](const SceneDocumentSnapshot& document)
                {
                    ApplySceneDocument(document);
                    Document = document;
                };
                ContextData.SceneDocument.AssetDirectory = [this]()
                { return AssetRoot; };
                ContextData.SceneDocument.IsDirty = [this]()
                { return History.IsDirty(); };
            }

            [[nodiscard]] const MCP::EditorMcpContext& Context() const override
            {
                return ContextData;
            }
            [[nodiscard]] bool IsCurrentCallCancelled() const override
            {
                return false;
            }
            [[nodiscard]] bool PublishArtifact(AutomationArtifact) override
            {
                return false;
            }

            CommandHistory History;
            SceneDocumentSnapshot Document;
            std::filesystem::path AssetRoot;
            bool EditMode = true;
            MCP::EditorMcpContext ContextData;

          protected:
            Json MarshalReadOnMainThread(const std::function<Json()>& job, std::chrono::milliseconds) override
            {
                return job();
            }
            void EmitProgressUpdate(f64, f64, const std::string&) const override {}
        };

        std::string ReadBytes(const std::filesystem::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        }

        void WriteBytes(const std::filesystem::path& path, const std::string& bytes)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << bytes;
            ASSERT_TRUE(output.good());
        }
    } // namespace

    class McpAutomationSceneLifecycle : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Root = OloEngine::Tests::TempDir();
            m_Host = std::make_unique<DocumentHost>(m_Root);
            RegisterSceneLifecycleCommands(m_Registry);
        }

        void TearDown() override
        {
            m_Host.reset();
            std::error_code ignored;
            std::filesystem::remove_all(m_Root, ignored);
        }

        AutomationInvocation Call(const std::string& name, const Json& arguments = Json::object())
        {
            return m_Registry.Invoke(*m_Host, name, arguments, AutomationWriteConsent::Granted);
        }

        Json Success(const std::string& name, const Json& arguments = Json::object())
        {
            auto result = Call(name, arguments);
            EXPECT_EQ(result.Outcome, AutomationInvocation::Status::Ok) << result.Message;
            EXPECT_FALSE(result.Result.IsError) << result.Result.Content.dump();
            return result.Result.StructuredContent;
        }

        Entity AddDirtyEntity(const std::string& name)
        {
            auto command = std::make_unique<CreateEntityCommand>(m_Host->Document.SceneRef, name);
            auto* pending = command.get();
            m_Host->History.Execute(std::move(command));
            return *m_Host->Document.SceneRef->TryGetEntityWithUUID(pending->GetEntityUUID());
        }

        std::filesystem::path m_Root;
        std::unique_ptr<DocumentHost> m_Host;
        AutomationRegistry m_Registry;
    };

    TEST_F(McpAutomationSceneLifecycle, NewUndoRestoresActualSceneSelectionPathAndCheckpoint)
    {
        auto originalScene = m_Host->Document.SceneRef;
        originalScene->SetName("Original");
        m_Host->Document.Path = m_Root / "original.olo";
        Entity originalEntity = AddDirtyEntity("Retained");
        const UUID originalId = originalEntity.GetUUID();
        m_Host->Document.Selection = { originalId };
        const auto beforeYaml = SceneSerializer(originalScene).SerializeToYAML();

        EXPECT_TRUE(Success("olo_scene_new", { { "name", "Fresh" } })["changed"]);
        EXPECT_NE(m_Host->Document.SceneRef, originalScene);
        EXPECT_TRUE(m_Host->Document.Path.empty());
        EXPECT_TRUE(m_Host->Document.Selection.empty());
        EXPECT_FALSE(m_Host->History.IsDirty());
        const auto newScene = m_Host->Document.SceneRef;

        EXPECT_TRUE(Success("olo_editor_undo")["dirty"]);
        EXPECT_EQ(m_Host->Document.SceneRef, originalScene);
        EXPECT_EQ(m_Host->Document.Path, m_Root / "original.olo");
        EXPECT_EQ(m_Host->Document.Selection, std::vector<UUID>{ originalId });
        EXPECT_EQ(SceneSerializer(m_Host->Document.SceneRef).SerializeToYAML(), beforeYaml);
        Success("olo_editor_undo");
        EXPECT_FALSE(originalScene->TryGetEntityWithUUID(originalId));
        EXPECT_FALSE(m_Host->History.IsDirty());

        Success("olo_editor_redo");
        Success("olo_editor_redo");
        EXPECT_EQ(m_Host->Document.SceneRef, newScene);
        EXPECT_FALSE(m_Host->History.IsDirty());
    }

    TEST_F(McpAutomationSceneLifecycle, SaveAsUndoRemovesNewFileAndRedoRestoresExactBytes)
    {
        const UUID entityId = AddDirtyEntity("Authored").GetUUID();
        auto result = Success("olo_scene_save_as", { { "path", "authored.olo" } });
        const auto path = m_Root / "authored.olo";
        EXPECT_EQ(result["path"], path.string());
        EXPECT_FALSE(result["dirty"]);
        EXPECT_TRUE(result["changed"]);
        const auto bytes = ReadBytes(path);
        EXPECT_FALSE(bytes.empty());

        auto loaded = Ref<Scene>::Create();
        ASSERT_TRUE(SceneSerializer(loaded).Deserialize(path));
        EXPECT_TRUE(loaded->TryGetEntityWithUUID(entityId));
        EXPECT_TRUE(Success("olo_editor_undo")["dirty"]);
        EXPECT_FALSE(std::filesystem::exists(path));
        EXPECT_TRUE(m_Host->Document.Path.empty());
        EXPECT_EQ(m_Host->Document.SceneRef->GetName(), "Untitled");
        Success("olo_editor_redo");
        EXPECT_EQ(ReadBytes(path), bytes);
        EXPECT_FALSE(m_Host->History.IsDirty());
    }

    TEST_F(McpAutomationSceneLifecycle, OverwriteUndoRestoresExistingBytesAndOriginalDestination)
    {
        const auto path = m_Root / "existing.olo";
        const std::string originalBytes = "Original external scene bytes\r\n";
        WriteBytes(path, originalBytes);
        m_Host->Document.Path = m_Root / "current.olo";
        m_Host->Document.SceneRef->SetName("Current");
        AddDirtyEntity("Change");
        Success("olo_scene_save_as", { { "path", path.string() } });
        EXPECT_NE(ReadBytes(path), originalBytes);
        Success("olo_editor_undo");
        EXPECT_EQ(ReadBytes(path), originalBytes);
        EXPECT_EQ(m_Host->Document.Path, m_Root / "current.olo");
        EXPECT_EQ(m_Host->Document.SceneRef->GetName(), "Current");
        EXPECT_TRUE(m_Host->History.IsDirty());
    }

    TEST_F(McpAutomationSceneLifecycle, SavedDocumentNewUndoReturnsToCleanCheckpoint)
    {
        AddDirtyEntity("Saved entity");
        Success("olo_scene_save_as", { { "path", "original.olo" } });
        const auto originalScene = m_Host->Document.SceneRef;
        Success("olo_scene_new");
        Success("olo_editor_undo");
        EXPECT_EQ(m_Host->Document.SceneRef, originalScene);
        EXPECT_EQ(m_Host->Document.Path, m_Root / "original.olo");
        EXPECT_FALSE(m_Host->History.IsDirty());
        Success("olo_editor_undo");
        EXPECT_TRUE(m_Host->History.IsDirty());
        EXPECT_TRUE(m_Host->Document.Path.empty());
    }

    TEST_F(McpAutomationSceneLifecycle, SaveCommitsPreparedSettingsAndUndoRestoresAuthoredSettings)
    {
        m_Host->Document.SceneRef->GetWindSettings().Enabled = false;
        m_Host->Document.RenderedSettings.Wind.Enabled = true;
        m_Host->ContextData.SceneDocument.PrepareSave = [](const SceneDocumentSnapshot& before)
        {
            auto after = before;
            after.AuthoredSettings = before.RenderedSettings;
            return after;
        };
        Success("olo_scene_save_as", { { "path", "settings.olo" } });
        EXPECT_TRUE(m_Host->Document.SceneRef->GetWindSettings().Enabled);
        auto reopened = Ref<Scene>::Create();
        ASSERT_TRUE(SceneSerializer(reopened).Deserialize(m_Root / "settings.olo"));
        EXPECT_TRUE(reopened->GetWindSettings().Enabled);
        Success("olo_editor_undo");
        EXPECT_FALSE(m_Host->Document.SceneRef->GetWindSettings().Enabled);
        EXPECT_TRUE(m_Host->Document.RenderedSettings.Wind.Enabled);
        EXPECT_FALSE(m_Host->History.IsDirty());
    }

    TEST_F(McpAutomationSceneLifecycle, ExternalFileChangesRejectUndoAndRedoWithoutLosingHistory)
    {
        AddDirtyEntity("Change");
        const auto path = m_Root / "guarded.olo";
        Success("olo_scene_save_as", { { "path", path.string() } });
        const auto savedBytes = ReadBytes(path);
        WriteBytes(path, "External modification");
        EXPECT_TRUE(Call("olo_editor_undo").Result.IsError);
        EXPECT_EQ(ReadBytes(path), "External modification");
        EXPECT_TRUE(m_Host->History.CanUndo());
        EXPECT_FALSE(m_Host->History.CanRedo());
        EXPECT_FALSE(m_Host->History.IsDirty());
        WriteBytes(path, savedBytes);
        Success("olo_editor_undo");
        WriteBytes(path, "New unrelated file");
        EXPECT_TRUE(Call("olo_editor_redo").Result.IsError);
        EXPECT_EQ(ReadBytes(path), "New unrelated file");
        EXPECT_TRUE(m_Host->History.CanRedo());
        EXPECT_TRUE(m_Host->History.IsDirty());
        EXPECT_TRUE(m_Host->Document.Path.empty());
        std::filesystem::remove(path);
        Success("olo_editor_redo");
        EXPECT_EQ(ReadBytes(path), savedBytes);
    }

    TEST_F(McpAutomationSceneLifecycle, IdenticalSaveAddsNoUndoEntryAndSavedEditChainRestoresCheckpoints)
    {
        AddDirtyEntity("First");
        Success("olo_scene_save_as", { { "path", "checkpoint.olo" } });
        const auto firstBytes = ReadBytes(m_Root / "checkpoint.olo");
        const auto undoDescription = m_Host->History.GetUndoDescription();
        EXPECT_FALSE(Success("olo_scene_save")["changed"]);
        EXPECT_EQ(m_Host->History.GetUndoDescription(), undoDescription);
        AddDirtyEntity("Second");
        EXPECT_TRUE(m_Host->History.IsDirty());
        // The UI Save action calls the same document seam directly. Interleave
        // that entry with API saves and edits, then walk through both saves.
        ASSERT_TRUE(SaveSceneDocument(m_Host->ContextData.SceneDocument, m_Host->History));
        EXPECT_FALSE(m_Host->History.IsDirty());
        Success("olo_editor_undo");
        EXPECT_EQ(ReadBytes(m_Root / "checkpoint.olo"), firstBytes);
        EXPECT_TRUE(m_Host->History.IsDirty());
        Success("olo_editor_undo");
        EXPECT_FALSE(m_Host->History.IsDirty());
        Success("olo_editor_redo");
        EXPECT_TRUE(m_Host->History.IsDirty());
        Success("olo_editor_redo");
        EXPECT_FALSE(m_Host->History.IsDirty());
        Success("olo_editor_undo");
        Success("olo_editor_undo");
        Success("olo_editor_undo");
        EXPECT_FALSE(std::filesystem::exists(m_Root / "checkpoint.olo"));
        EXPECT_TRUE(m_Host->Document.Path.empty());
    }

    TEST_F(McpAutomationSceneLifecycle, UndoSaveDoesNotReviveCheckpointFromDiscardedRedoBranch)
    {
        AddDirtyEntity("Discarded saved state");
        m_Host->History.MarkSaved();
        m_Host->History.Undo();
        ASSERT_TRUE(m_Host->History.IsDirty());
        Success("olo_scene_save_as", { { "path", "branch.olo" } });
        Success("olo_editor_undo");
        AddDirtyEntity("Different state at the same history version");
        EXPECT_TRUE(m_Host->History.IsDirty());
    }

    TEST_F(McpAutomationSceneLifecycle, FailureAndConsentPreserveDocumentAndHistory)
    {
        const auto original = m_Host->Document.SceneRef;
        const auto denied = m_Registry.Invoke(*m_Host, "olo_scene_new", Json::object());
        EXPECT_EQ(denied.Outcome, AutomationInvocation::Status::WriteConsentWithheld);
        EXPECT_TRUE(Call("olo_scene_save").Result.IsError);
        EXPECT_TRUE(Call("olo_scene_save_as", { { "path", "missing/nested.olo" } }).Result.IsError);
        EXPECT_TRUE(Call("olo_scene_save_as", { { "path", "not-a-scene.txt" } }).Result.IsError);
        EXPECT_TRUE(Call("olo_scene_save_as", { { "path", "" } }).Result.IsError);
        EXPECT_EQ(m_Host->Document.SceneRef, original);
        EXPECT_TRUE(m_Host->Document.Path.empty());
        EXPECT_FALSE(m_Host->History.CanUndo());
        EXPECT_FALSE(m_Host->History.IsDirty());
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(m_Root), std::filesystem::directory_iterator()), 0);
    }

    TEST_F(McpAutomationSceneLifecycle, PlayCanReadAuthoredDirtyStateButCannotMutate)
    {
        AddDirtyEntity("Unsaved authored entity");
        m_Host->EditMode = false;
        const auto status = Success("olo_scene_status");
        EXPECT_FALSE(status["editMode"]);
        EXPECT_TRUE(status["dirty"]);
        EXPECT_TRUE(Call("olo_scene_new").Result.IsError);
        EXPECT_TRUE(Call("olo_editor_undo").Result.IsError);
        EXPECT_TRUE(Call("olo_scene_save_as", { { "path", "blocked.olo" } }).Result.IsError);
        EXPECT_FALSE(std::filesystem::exists(m_Root / "blocked.olo"));
    }

    TEST_F(McpAutomationSceneLifecycle, MissingDocumentContextRejectsUndoBeforeMutation)
    {
        const UUID uuid = AddDirtyEntity("Retained").GetUUID();
        m_Host->ContextData.SceneDocument.Capture = {};
        EXPECT_TRUE(Call("olo_editor_undo").Result.IsError);
        EXPECT_TRUE(m_Host->Document.SceneRef->TryGetEntityWithUUID(uuid));
        EXPECT_TRUE(m_Host->History.CanUndo());
    }

    TEST_F(McpAutomationSceneLifecycle, RegisteredAuthoringCommandsBuildSaveAndReopenScene)
    {
        AutomationRegistry registry;
        MCP::RegisterBuiltinCommands(registry);
        const auto invoke = [&](const std::string& name, const Json& arguments = Json::object())
        {
            const auto result = registry.Invoke(*m_Host, name, arguments, AutomationWriteConsent::Granted);
            EXPECT_EQ(result.Outcome, AutomationInvocation::Status::Ok) << result.Message;
            EXPECT_FALSE(result.Result.IsError) << result.Result.Content.dump();
            return result.Result.StructuredContent;
        };
        invoke("olo_scene_new", { { "name", "Generated" } });
        const auto parent = invoke("olo_entity_create", { { "name", "Parent" } }).at("entity").get<std::string>();
        const auto child = invoke("olo_entity_create", { { "name", "Light" } }).at("entity").get<std::string>();
        invoke("olo_entity_reparent", { { "entity", child }, { "parent", parent } });
        invoke("olo_component_add", { { "entity", child }, { "component", "PointLightComponent" } });
        invoke("olo_entity_set_field", { { "entity", child }, { "component", "PointLightComponent" }, { "field", "Intensity" }, { "value", 7.25 } });
        invoke("olo_scene_save_as", { { "path", "generated.olo" } });

        auto reopened = Ref<Scene>::Create();
        ASSERT_TRUE(SceneSerializer(reopened).Deserialize(m_Root / "generated.olo"));
        const auto parentEntity = reopened->TryGetEntityWithUUID(UUID(std::stoull(parent)));
        const auto lightEntity = reopened->TryGetEntityWithUUID(UUID(std::stoull(child)));
        ASSERT_TRUE(parentEntity);
        ASSERT_TRUE(lightEntity);
        EXPECT_EQ(static_cast<u64>(parentEntity->GetParentUUID()), 0u);
        EXPECT_EQ(lightEntity->GetParentUUID(), parentEntity->GetUUID());
        ASSERT_TRUE(lightEntity->HasComponent<PointLightComponent>());
        EXPECT_FLOAT_EQ(lightEntity->GetComponent<PointLightComponent>().m_Intensity, 7.25f);
        EXPECT_EQ(parentEntity->Children(), std::vector<UUID>{ lightEntity->GetUUID() });
    }
} // namespace OloEngine::Automation::Tests
