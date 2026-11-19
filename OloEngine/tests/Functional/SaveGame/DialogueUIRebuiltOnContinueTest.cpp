#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// DialogueUIRebuiltOnContinueTest — Functional Test (#1712).
//
// Cross-subsystem seam under test:
//   DialogueSystem / DialogueUIController × SaveGameSerializer, through
//   Scene's runtime-owned entity mark.
//
// The dialogue UI (DialogueCanvas, DialoguePanel, DialogueSpeakerName,
// DialogueBody and the DialogueChoice_N buttons) is built by the controller
// at runtime start. v50 saved it like any other entity, and Continue then
// built a second one beside the restored copy: every save -> Continue -> save
// cycle grew the scene by four entities. The contract now:
//
//   * the save never carries a runtime-owned entity;
//   * the save carries the conversation (DialogueStateComponent on the NPC),
//     and Continue rebuilds the UI from it, in each of the three states a
//     player can save in: no conversation, a line on screen, choices offered;
//   * an authored entity that merely shares a tag with the UI is untouched;
//   * a save loaded into a running scene rebuilds the UI the registry swap
//     took away;
//   * a v50 save, which can carry the duplicated hierarchy, is refused;
//   * the edit scene, which scene YAML writes, never holds the UI.
//
// Continue is modelled as production runs it: the save restores into a scene
// that is not running yet, then runtime start builds the dialogue system
// (InitDialogueSystem, which OnRuntimeStart calls).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr u64 kGreeting = 0xD1;
    constexpr u64 kChoice = 0xC1;
    constexpr u64 kAsk = 0xD2;
    constexpr u64 kLeave = 0xD3;
    constexpr u64 kUserPanelId = 0x1712'0001;
    constexpr std::array<std::string_view, 4> kControllerTags = { "DialogueCanvas", "DialoguePanel",
                                                                  "DialogueSpeakerName", "DialogueBody" };

    enum class SavedIn
    {
        NoConversation,
        LineOnScreen,
        ChoicesOffered
    };

    sizet CountEntities(const Scene& scene)
    {
        return scene.GetAllEntitiesWith<IDComponent>().size();
    }

    // Entities with this tag, split by whether a runtime system owns them.
    sizet CountTagged(const Scene& scene, std::string_view tag, bool runtimeOwned)
    {
        sizet count = 0;
        auto view = scene.GetAllEntitiesWith<IDComponent, TagComponent>();
        for (auto [handle, id, tc] : view.each())
        {
            if (tc.Tag == tag && scene.IsRuntimeOwned(Entity{ handle, scene }) == runtimeOwned)
                ++count;
        }
        return count;
    }

    sizet CountChoiceButtons(const Scene& scene)
    {
        sizet count = 0;
        auto view = scene.GetAllEntitiesWith<TagComponent>();
        for (auto [handle, tc] : view.each())
        {
            if (std::string_view(tc.Tag).starts_with("DialogueChoice_"))
                ++count;
        }
        return count;
    }

    std::optional<std::string> ControllerText(const Scene& scene, std::string_view tag)
    {
        auto view = scene.GetAllEntitiesWith<TagComponent, UITextComponent>();
        for (auto [handle, tc, text] : view.each())
        {
            if (tc.Tag == tag && scene.IsRuntimeOwned(Entity{ handle, scene }))
                return text.m_Text;
        }
        return std::nullopt;
    }

    std::vector<std::string> ChoiceLabels(const Scene& scene)
    {
        std::vector<std::string> labels;
        for (sizet i = 0;; ++i)
        {
            auto label = ControllerText(scene, "DialogueChoice_" + std::to_string(i));
            if (!label)
                return labels;
            labels.push_back(std::move(*label));
        }
    }

    // What the save itself holds, decoded into a scene no system touches.
    Ref<Scene> Decode(const std::vector<u8>& payload)
    {
        Ref<Scene> probe = Ref<Scene>::Create();
        EXPECT_TRUE(SaveGameSerializer::RestoreSceneState(*probe, payload));
        return probe;
    }

    // Continue: restore into a scene that is not running, then runtime start
    // builds the dialogue system, and one update lays the UI out.
    Ref<Scene> Continue(const std::vector<u8>& payload)
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        EXPECT_TRUE(SaveGameSerializer::RestoreSceneState(*scene, payload));
        scene->InitDialogueSystem();
        scene->GetDialogueSystem()->Update(Timestep{ 1.0f / 60.0f });
        return scene;
    }
} // namespace

class DialogueUIRebuiltOnContinueTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        EnableAssetManager({});

        // greeting -> choice -> { "Ask about the light", "Leave" }
        auto tree = Ref<DialogueTreeAsset>::Create();
        const auto addLine = [&tree](u64 id, const char* name, const char* text)
        {
            DialogueNodeData node;
            node.ID = UUID{ id };
            node.Type = "dialogue";
            node.Name = name;
            node.Properties.try_emplace("speaker", DialoguePropertyValue{ std::string("Keeper") });
            node.Properties.try_emplace("text", DialoguePropertyValue{ std::string(text) });
            tree->GetNodesWritable().AddTail(std::move(node));
        };
        addLine(kGreeting, "Greeting", "The lamp still turns.");
        addLine(kAsk, "Ask about the light", "It never goes out.");
        addLine(kLeave, "Leave", "Safe sailing.");
        DialogueNodeData choice;
        choice.ID = UUID{ kChoice };
        choice.Type = "choice";
        choice.Name = "Choice";
        tree->GetNodesWritable().AddTail(std::move(choice));
        for (const auto& [from, to] : { std::pair{ kGreeting, kChoice }, std::pair{ kChoice, kAsk }, std::pair{ kChoice, kLeave } })
        {
            DialogueConnection connection;
            connection.SourceNodeID = UUID{ from };
            connection.TargetNodeID = UUID{ to };
            tree->GetConnectionsWritable().Add(connection);
        }
        tree->SetRootNodeID(UUID{ kGreeting });
        tree->RebuildNodeIndex();
        const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<DialogueTreeAsset>(tree);
        ASSERT_NE(static_cast<u64>(handle), 0ULL);

        m_Npc = GetScene().CreateEntity("Keeper");
        auto& dialogue = m_Npc.AddComponent<DialogueComponent>();
        dialogue.m_DialogueTree = handle;
        dialogue.m_TriggerOnce = false;
        m_NpcId = m_Npc.GetUUID();

        // An authored entity that shares the controller's panel tag, with a
        // child, so its identity, components and hierarchy are all at stake.
        Entity userPanel = GetScene().CreateEntityWithUUID(UUID{ kUserPanelId }, "DialoguePanel");
        userPanel.AddComponent<UIPanelComponent>().m_BackgroundColor = { 0.25f, 0.5f, 0.75f, 1.0f };
        userPanel.AddComponent<UIRectTransformComponent>().m_AnchoredPosition = { 12.0f, 34.0f };
        Entity userLabel = GetScene().CreateEntity("UserLabel");
        userLabel.AddComponent<UITextComponent>().m_Text = "authored";
        userLabel.SetParent(userPanel);
        m_UserLabelId = userLabel.GetUUID();

        m_AuthoredCount = CountEntities(GetScene());
        EnableDialogue(); // runtime start: the controller builds its four entities
    }

    void Reach(SavedIn state)
    {
        auto* dialogue = GetScene().GetDialogueSystem();
        ASSERT_NE(dialogue, nullptr);
        if (state != SavedIn::NoConversation && !m_Npc.HasComponent<DialogueStateComponent>())
        {
            dialogue->StartDialogue(m_Npc);
            dialogue->AdvanceDialogue(m_Npc); // finish the typewriter
        }
        if (state == SavedIn::ChoicesOffered)
        {
            dialogue->AdvanceDialogue(m_Npc);
        }
        RunFrames(1);
    }

    // The scene after Continue: the authored entities, the four controller
    // entities, and a button per offered choice.
    [[nodiscard]] sizet RunningCount(SavedIn state) const
    {
        return m_AuthoredCount + kControllerTags.size() + (state == SavedIn::ChoicesOffered ? 2u : 0u);
    }

    void ExpectSaveHoldsNoControllerUI(const std::vector<u8>& payload) const
    {
        ExpectSaveHoldsNoControllerUI(payload, m_AuthoredCount);
    }

    static void ExpectSaveHoldsNoControllerUI(const std::vector<u8>& payload, sizet authoredCount)
    {
        ASSERT_FALSE(payload.empty());
        Ref<Scene> saved = Decode(payload);
        EXPECT_EQ(CountEntities(*saved), authoredCount) << "the save carries entities the authored scene does not have";
        for (const std::string_view tag : { "DialogueCanvas", "DialogueSpeakerName", "DialogueBody" })
            EXPECT_EQ(CountTagged(*saved, tag, false), 0u) << tag;
        EXPECT_EQ(CountTagged(*saved, "DialoguePanel", false), 1u) << "only the authored panel is saved";
        EXPECT_EQ(CountChoiceButtons(*saved), 0u);
    }

    static void ExpectOneControllerHierarchy(const Scene& scene, sizet expectedCount, sizet choiceButtons)
    {
        EXPECT_EQ(CountEntities(scene), expectedCount);
        for (const std::string_view tag : kControllerTags)
            EXPECT_EQ(CountTagged(scene, tag, true), 1u) << tag;
        EXPECT_EQ(CountChoiceButtons(scene), choiceButtons);
    }

    void ExpectOneControllerHierarchy(const Scene& scene, SavedIn state) const
    {
        ExpectOneControllerHierarchy(scene, RunningCount(state), state == SavedIn::ChoicesOffered ? 2u : 0u);
    }

    void ExpectUserPanelIntact(const Scene& scene) const
    {
        const auto panel = scene.TryGetEntityWithUUID(UUID{ kUserPanelId });
        ASSERT_TRUE(panel.has_value());
        EXPECT_FALSE(scene.IsRuntimeOwned(*panel));
        EXPECT_EQ(panel->GetComponent<TagComponent>().Tag, "DialoguePanel");
        ASSERT_TRUE(panel->HasComponent<UIPanelComponent>());
        EXPECT_FLOAT_EQ(panel->GetComponent<UIPanelComponent>().m_BackgroundColor.b, 0.75f);
        ASSERT_TRUE(panel->HasComponent<UIRectTransformComponent>());
        EXPECT_FLOAT_EQ(panel->GetComponent<UIRectTransformComponent>().m_AnchoredPosition.y, 34.0f);
        const TArray<UUID> children = panel->Children();
        ASSERT_EQ(children.Num(), 1) << "the authored panel's child list holds exactly its own child";
        EXPECT_EQ(static_cast<u64>(children[0]), static_cast<u64>(m_UserLabelId));
    }

    void RunCycle(SavedIn state)
    {
        Reach(state);
        ExpectOneControllerHierarchy(GetScene(), state);
        std::optional<DialogueStateComponent> savedState;
        if (m_Npc.HasComponent<DialogueStateComponent>())
            savedState = m_Npc.GetComponent<DialogueStateComponent>();
        ASSERT_EQ(savedState.has_value(), state != SavedIn::NoConversation);

        std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(GetScene());
        ExpectSaveHoldsNoControllerUI(payload);

        // save -> Continue -> save, twice: the hierarchy never grows.
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            SCOPED_TRACE(cycle);
            Ref<Scene> continued = Continue(payload);
            ExpectOneControllerHierarchy(*continued, state);
            ExpectUserPanelIntact(*continued);

            const auto npc = continued->TryGetEntityWithUUID(m_NpcId);
            ASSERT_TRUE(npc.has_value());
            ASSERT_EQ(npc->HasComponent<DialogueStateComponent>(), savedState.has_value());
            if (savedState)
            {
                EXPECT_TRUE(npc->GetComponent<DialogueStateComponent>() == *savedState)
                    << "the conversation restored on Continue differs from the one saved";
            }

            payload = SaveGameSerializer::CaptureSceneState(*continued);
            ExpectSaveHoldsNoControllerUI(payload);
        }
    }

    Entity m_Npc;
    UUID m_NpcId;
    UUID m_UserLabelId;
    sizet m_AuthoredCount = 0;
};

TEST_F(DialogueUIRebuiltOnContinueTest, NoConversationCycleDoesNotGrowTheUI)
{
    RunCycle(SavedIn::NoConversation);
}

TEST_F(DialogueUIRebuiltOnContinueTest, LineOnScreenCycleDoesNotGrowTheUI)
{
    RunCycle(SavedIn::LineOnScreen);
}

TEST_F(DialogueUIRebuiltOnContinueTest, ChoicesOfferedCycleDoesNotGrowTheUI)
{
    RunCycle(SavedIn::ChoicesOffered);
}

TEST_F(DialogueUIRebuiltOnContinueTest, ContinueShowsTheSameSpeakerLineAndChoices)
{
    Reach(SavedIn::LineOnScreen);
    Ref<Scene> line = Continue(SaveGameSerializer::CaptureSceneState(GetScene()));
    EXPECT_EQ(ControllerText(*line, "DialogueSpeakerName"), "Keeper");
    EXPECT_EQ(ControllerText(*line, "DialogueBody"), "The lamp still turns.");

    Reach(SavedIn::ChoicesOffered); // the line on screen advances to the choice
    Ref<Scene> choices = Continue(SaveGameSerializer::CaptureSceneState(GetScene()));
    const auto npc = choices->TryGetEntityWithUUID(m_NpcId);
    ASSERT_TRUE(npc.has_value());
    const auto& state = npc->GetComponent<DialogueStateComponent>();
    EXPECT_EQ(state.m_State, DialogueState::WaitingForChoice);
    EXPECT_EQ(static_cast<u64>(state.m_CurrentNodeID), kChoice);
    EXPECT_EQ(ChoiceLabels(*choices), (std::vector<std::string>{ "Ask about the light", "Leave" }));

    // The restored conversation is live, not a picture of one: picking a
    // choice follows the tree from the restored node.
    choices->GetDialogueSystem()->SelectChoice(*npc, 1);
    choices->GetDialogueSystem()->Update(Timestep{ 1.0f / 60.0f });
    EXPECT_EQ(static_cast<u64>(npc->GetComponent<DialogueStateComponent>().m_CurrentNodeID), kLeave);
    EXPECT_EQ(npc->GetComponent<DialogueStateComponent>().m_CurrentText, "Safe sailing.");
    EXPECT_EQ(CountChoiceButtons(*choices), 0u);
}

TEST_F(DialogueUIRebuiltOnContinueTest, LoadIntoTheRunningSceneRebuildsTheUI)
{
    // A load during play keeps the dialogue system alive and swaps the
    // registry under it; the controller's entities go with the old registry.
    Reach(SavedIn::ChoicesOffered);
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(GetScene());

    GetScene().GetDialogueSystem()->EndDialogue(m_Npc);
    RunFrames(1);
    ASSERT_FALSE(m_Npc.HasComponent<DialogueStateComponent>());

    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(GetScene(), payload));
    RunFrames(1);

    ExpectOneControllerHierarchy(GetScene(), SavedIn::ChoicesOffered);
    ExpectUserPanelIntact(GetScene());
    EXPECT_EQ(ControllerText(GetScene(), "DialogueSpeakerName"), "Keeper");
    EXPECT_EQ(ChoiceLabels(GetScene()), (std::vector<std::string>{ "Ask about the light", "Leave" }));
}

TEST_F(DialogueUIRebuiltOnContinueTest, AV50SaveWithTheDuplicatedHierarchyIsRefused)
{
    // What v50 wrote after one Continue: the authored scene plus two unmarked
    // copies of the controller hierarchy.
    Ref<Scene> v50 = Ref<Scene>::Create();
    v50->CreateEntityWithUUID(UUID{ kUserPanelId }, "DialoguePanel").AddComponent<UIPanelComponent>();
    for (int copy = 0; copy < 2; ++copy)
    {
        for (const std::string_view tag : kControllerTags)
            (void)v50->CreateEntity(std::string(tag));
    }
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*v50);
    ASSERT_FALSE(payload.empty());

    std::filesystem::create_directories(SaveGameManager::GetSaveDirectory());
    const std::filesystem::path path = SaveGameManager::GetSaveFilePath("v50_duplicate_ui");
    SaveGameHeader header;
    header.EntityCount = static_cast<u32>(CountEntities(*v50));
    header.PayloadUncompressedSize = payload.size();
    ASSERT_TRUE(SaveGameFile::Write(path, header, SaveGameMetadata{}, {}, payload));

    // Positive control: the same bytes under the current version load, so
    // the refusal below is the version's doing, not the payload's.
    {
        Ref<Scene> target = Ref<Scene>::Create();
        ASSERT_EQ(SaveGameManager::Load(*target, "v50_duplicate_ui"), SaveLoadResult::Success);
        EXPECT_EQ(CountTagged(*target, "DialogueCanvas", false), 2u);
    }

    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        const u32 version = 50;
        file.seekp(static_cast<std::streamoff>(offsetof(SaveGameHeader, FormatVersion)));
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    }
    const sizet before = CountEntities(GetScene());
    EXPECT_EQ(SaveGameManager::Load(GetScene(), "v50_duplicate_ui"), SaveLoadResult::UnsupportedVersion);
    EXPECT_EQ(CountEntities(GetScene()), before) << "a refused save must leave the scene untouched";
    ExpectOneControllerHierarchy(GetScene(), SavedIn::NoConversation);
}

TEST_F(DialogueUIRebuiltOnContinueTest, TheEditSceneNeverHoldsTheControllerUI)
{
    // Scene YAML writes the edit scene. Play runs on a copy, and the
    // controller builds its UI only in that copy.
    Ref<Scene> edit = Ref<Scene>::Create();
    Entity npc = edit->CreateEntity("Keeper");
    npc.AddComponent<DialogueComponent>().m_DialogueTree = m_Npc.GetComponent<DialogueComponent>().m_DialogueTree;
    edit->CreateEntityWithUUID(UUID{ kUserPanelId }, "DialoguePanel").AddComponent<UIPanelComponent>();

    Ref<Scene> play = Scene::Copy(edit);
    play->InitDialogueSystem();
    ASSERT_EQ(CountTagged(*play, "DialogueCanvas", true), 1u) << "positive control: the Play copy has the UI";

    const std::string yaml = SceneSerializer(edit).SerializeToYAML();
    EXPECT_EQ(yaml.find("DialogueCanvas"), std::string::npos);
    EXPECT_EQ(yaml.find("DialogueSpeakerName"), std::string::npos);
    EXPECT_EQ(yaml.find("DialogueBody"), std::string::npos);
    EXPECT_EQ(CountTagged(*edit, "DialoguePanel", false), 1u);
    EXPECT_EQ(CountEntities(*edit), 2u);
}

