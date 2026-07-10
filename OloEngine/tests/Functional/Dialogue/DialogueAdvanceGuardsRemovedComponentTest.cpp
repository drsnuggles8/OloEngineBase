// OLO_TEST_LAYER: Functional
#include "OloEnginePCH.h"

// =============================================================================
// DialogueAdvanceGuardsRemovedComponentTest — Functional Test.
//
// Cross-subsystem seam under test:
//   DialogueSystem::AdvanceDialogue / ProcessNode × Entity::HasComponent ×
//   DialogueComponent removal mid-conversation.
//
// AdvanceDialogue and ProcessNode used to call
// entity.GetComponent<DialogueComponent>() unconditionally, unlike EndDialogue
// which correctly guards with HasComponent<DialogueComponent>() first. If a
// DialogueComponent is removed while an entity is mid-dialogue (editor
// remove-component button, a script, or gameplay logic), the next
// AdvanceDialogue/SelectChoice hit an EnTT assert / UB instead of ending the
// dialogue cleanly.
//
// Scenario: start a dialogue, let the reveal finish, remove DialogueComponent
// out from under the live DialogueStateComponent, then call AdvanceDialogue —
// it must end the dialogue (remove DialogueStateComponent) instead of
// asserting/crashing.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class DialogueAdvanceGuardsRemovedComponentTest : public FunctionalTest
{
  protected:
    static constexpr u64 kNodeA_ID = 0xA0ULL;
    static constexpr const char* kTextA = "Hi.";

    void BuildScene() override
    {
        EnableAssetManager({});

        m_TreeAsset = Ref<DialogueTreeAsset>::Create();

        DialogueNodeData a;
        a.ID = OloEngine::UUID{ kNodeA_ID };
        a.Type = "dialogue";
        a.Name = "A";
        a.Properties.emplace("text", DialoguePropertyValue{ std::string(kTextA) });

        auto& nodes = m_TreeAsset->GetNodesWritable();
        nodes.push_back(std::move(a));

        m_TreeAsset->SetRootNodeID(OloEngine::UUID{ kNodeA_ID });
        m_TreeAsset->RebuildNodeIndex();

        m_TreeHandle = AssetManager::AddMemoryOnlyAsset<DialogueTreeAsset>(m_TreeAsset);
        ASSERT_NE(static_cast<u64>(m_TreeHandle), 0ULL);

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

TEST_F(DialogueAdvanceGuardsRemovedComponentTest, AdvanceEndsDialogueInsteadOfAssertingWhenComponentRemovedMidConversation)
{
    auto* dialogueSystem = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogueSystem, nullptr);

    dialogueSystem->StartDialogue(m_Speaker);
    ASSERT_TRUE(m_Speaker.HasComponent<DialogueStateComponent>());
    EXPECT_EQ(m_Speaker.GetComponent<DialogueStateComponent>().m_State, DialogueState::Displaying);

    TickFor(1.5f); // past the reveal duration, so the next Advance walks the graph

    // Simulate the component vanishing mid-conversation (remove-component
    // button / script) while DialogueStateComponent is still live.
    m_Speaker.RemoveComponent<DialogueComponent>();
    ASSERT_TRUE(m_Speaker.HasComponent<DialogueStateComponent>())
        << "test setup invariant: state must still be live for this to exercise the guard.";

    EXPECT_NO_FATAL_FAILURE(dialogueSystem->AdvanceDialogue(m_Speaker));

    EXPECT_FALSE(m_Speaker.HasComponent<DialogueStateComponent>())
        << "AdvanceDialogue must end the dialogue (EndDialogue removes DialogueStateComponent) "
           "when DialogueComponent is missing, instead of dereferencing it unconditionally.";
}
