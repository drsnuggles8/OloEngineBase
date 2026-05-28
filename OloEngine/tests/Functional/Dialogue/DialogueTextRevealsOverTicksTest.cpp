#include "OloEnginePCH.h"

// =============================================================================
// DialogueTextRevealsOverTicksTest — Functional Test.
//
// Cross-subsystem seam under test:
//   AssetManager (memory asset) × DialogueSystem::StartDialogue ×
//   DialogueSystem::Update × DialogueStateComponent. The system pulls the
//   DialogueTreeAsset out of AssetManager by handle, transitions the
//   per-entity state to Displaying, then ticks `m_TextRevealProgress`
//   from 0 → 1 over time at the entity's `m_TextRevealSpeed`. If
//   Scene::OnUpdateRuntime stops driving DialogueSystem::Update, or the
//   memory-asset lookup path regresses, the reveal animation freezes.
//
// Scenario: register a tiny programmatic DialogueTreeAsset (single
// "dialogue" node, ~34 characters of text) with the memory-asset path on
// EditorAssetManager. Attach a DialogueComponent + DialogueStateComponent
// to an entity, call StartDialogue, then tick the scene. After enough
// time has elapsed at the default reveal speed (30 chars/sec), the
// progress must reach 1.0.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class DialogueTextRevealsOverTicksTest : public FunctionalTest
{
  protected:
    static constexpr const char* kLine = "The dragon stirs in its slumber..."; // 34 chars

    void BuildScene() override
    {
        EnableAssetManager({});

        m_TreeAsset = Ref<DialogueTreeAsset>::Create();

        DialogueNodeData root;
        root.ID = OloEngine::UUID{ static_cast<u64>(0x100ULL) };
        root.Type = "dialogue";
        root.Name = "Opening";
        root.Properties.emplace("text", DialoguePropertyValue{ std::string(kLine) });
        m_TreeAsset->GetNodesWritable().push_back(std::move(root));
        m_TreeAsset->SetRootNodeID(OloEngine::UUID{ static_cast<u64>(0x100ULL) });
        m_TreeAsset->RebuildNodeIndex();

        m_TreeHandle = AssetManager::AddMemoryOnlyAsset<DialogueTreeAsset>(m_TreeAsset);
        ASSERT_NE(static_cast<u64>(m_TreeHandle), 0ULL)
            << "AddMemoryOnlyAsset returned a zero handle.";

        m_Speaker = GetScene().CreateEntity("Speaker");
        auto& dc = m_Speaker.AddComponent<DialogueComponent>();
        dc.m_DialogueTree = m_TreeHandle;
        m_Speaker.AddComponent<DialogueStateComponent>();

        EnableDialogue();
    }

    Ref<DialogueTreeAsset> m_TreeAsset;
    AssetHandle m_TreeHandle{};
    Entity m_Speaker;
};

TEST_F(DialogueTextRevealsOverTicksTest, StartDialogueDisplaysAndRevealProgressesAcrossTicks)
{
    auto* dialogueSystem = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogueSystem, nullptr)
        << "Scene::InitDialogueSystem did not allocate the dialogue system — "
           "the EnableDialogue helper is broken.";

    dialogueSystem->StartDialogue(m_Speaker);

    const auto& state = m_Speaker.GetComponent<DialogueStateComponent>();
    EXPECT_EQ(state.m_State, DialogueState::Displaying)
        << "StartDialogue did not transition into Displaying — the "
           "AssetManager handle lookup may have failed.";
    EXPECT_EQ(state.m_CurrentText, std::string(kLine))
        << "dialogue text wasn't copied out of the node properties.";
    EXPECT_NEAR(state.m_TextRevealProgress, 0.0f, 1e-4f);

    // Default reveal speed is 30 chars/sec, text is 34 chars → ~1.13s for full reveal.
    // Tick 0.6s: progress should be in (0.1, 1.0).
    TickFor(0.6f);
    const f32 mid = state.m_TextRevealProgress;
    EXPECT_GT(mid, 0.1f)
        << "reveal progress didn't advance during ticks — DialogueSystem::Update "
           "isn't being invoked from Scene::OnUpdateRuntime.";
    EXPECT_LT(mid, 1.0f)
        << "reveal completed too quickly; m_TextRevealSpeed default changed?";

    TickFor(1.0f);
    EXPECT_NEAR(state.m_TextRevealProgress, 1.0f, 1e-4f)
        << "reveal progress never reached 1.0 despite enough simulated time.";
}
