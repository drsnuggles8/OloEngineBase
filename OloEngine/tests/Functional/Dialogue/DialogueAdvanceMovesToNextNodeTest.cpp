#include "OloEnginePCH.h"

// =============================================================================
// DialogueAdvanceMovesToNextNodeTest — Functional Test.
//
// Cross-subsystem seam under test:
//   DialogueSystem::StartDialogue × DialogueSystem::AdvanceDialogue ×
//   DialogueTreeAsset.GetConnectionsFrom × DialogueStateComponent.
//   Advance walks the default outgoing connection from the current node;
//   if there's no outgoing edge, EndDialogue runs and state.m_State
//   returns to Inactive. The previous DialogueTextRevealsOverTicksTest
//   only covered the SINGLE-node case (reveal animation); this one pins
//   the graph traversal — two connected "dialogue" nodes plus a
//   terminating advance.
//
// Scenario: tree A → B (both "dialogue" nodes). Start at A, tick the
// scene long enough for the typewriter to finish, AdvanceDialogue once
// → current node = B. AdvanceDialogue again → no outgoing edge → state
// transitions to Inactive (EndDialogue path).
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

class DialogueAdvanceMovesToNextNodeTest : public FunctionalTest
{
  protected:
    static constexpr u64 kNodeA_ID = 0xA0ULL;
    static constexpr u64 kNodeB_ID = 0xB0ULL;
    static constexpr const char* kTextA = "Hi.";
    static constexpr const char* kTextB = "Bye.";

    void BuildScene() override
    {
        EnableAssetManager({});

        m_TreeAsset = Ref<DialogueTreeAsset>::Create();

        DialogueNodeData a;
        a.ID = OloEngine::UUID{ kNodeA_ID };
        a.Type = "dialogue";
        a.Name = "A";
        a.Properties.emplace("text", DialoguePropertyValue{ std::string(kTextA) });

        DialogueNodeData b;
        b.ID = OloEngine::UUID{ kNodeB_ID };
        b.Type = "dialogue";
        b.Name = "B";
        b.Properties.emplace("text", DialoguePropertyValue{ std::string(kTextB) });

        auto& nodes = m_TreeAsset->GetNodesWritable();
        nodes.push_back(std::move(a));
        nodes.push_back(std::move(b));

        DialogueConnection conn;
        conn.SourceNodeID = OloEngine::UUID{ kNodeA_ID };
        conn.TargetNodeID = OloEngine::UUID{ kNodeB_ID };
        m_TreeAsset->GetConnectionsWritable().push_back(conn);

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

TEST_F(DialogueAdvanceMovesToNextNodeTest, AdvanceTraversesGraphEdgeAndEndsAfterTerminalNode)
{
    auto* dialogueSystem = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogueSystem, nullptr);

    dialogueSystem->StartDialogue(m_Speaker);

    auto& state = m_Speaker.GetComponent<DialogueStateComponent>();
    EXPECT_EQ(state.m_State, DialogueState::Displaying);
    EXPECT_EQ(state.m_CurrentText, std::string(kTextA));
    EXPECT_EQ(static_cast<u64>(state.m_CurrentNodeID), kNodeA_ID);

    // First AdvanceDialogue on un-finished reveal just snaps text to full —
    // it does NOT traverse the edge yet. So we need either to tick past
    // the reveal duration, or call AdvanceDialogue twice. We'll tick.
    TickFor(1.5f); // way past the reveal of "Hi." at 30 chars/sec
    ASSERT_NEAR(state.m_TextRevealProgress, 1.0f, 1e-4f)
        << "reveal didn't complete; subsequent AdvanceDialogue would snap "
           "to full text instead of moving to node B.";

    // Now advance — should walk the edge to node B.
    dialogueSystem->AdvanceDialogue(m_Speaker);
    EXPECT_EQ(static_cast<u64>(state.m_CurrentNodeID), kNodeB_ID)
        << "AdvanceDialogue didn't update m_CurrentNodeID after reveal completed — "
           "the GetConnectionsFrom default-edge branch didn't fire, or "
           "ProcessNode failed to write the new node ID back to state.";
    EXPECT_EQ(state.m_State, DialogueState::Displaying);
    EXPECT_EQ(state.m_CurrentText, std::string(kTextB))
        << "text didn't refresh to node B's content — ProcessNode for the "
           "second node didn't re-extract the 'text' property.";

    // Tick so the second reveal completes, then advance again — there's no
    // outgoing edge from B, so dialogue must end. EndDialogue does NOT
    // flip m_State to Inactive — it removes the DialogueStateComponent
    // outright. Holding a reference to the old state past EndDialogue is
    // UB, so we re-query via HasComponent.
    TickFor(1.5f);
    dialogueSystem->AdvanceDialogue(m_Speaker);
    EXPECT_FALSE(m_Speaker.HasComponent<DialogueStateComponent>())
        << "no outgoing edge from terminal node, but DialogueStateComponent "
           "wasn't removed — EndDialogue branch didn't fire on empty-connections "
           "result, or it stopped removing the component on end.";
}
