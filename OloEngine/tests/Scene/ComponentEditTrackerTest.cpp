#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// ComponentEditTrackerTest — unit test (headless, no GL, no live editor).
//
// Pins the undo state machine behind SceneHierarchyPanel::DrawComponent<T>
// (UndoRedo/ComponentEditTracker.h) against a real Scene, CommandHistory and
// ComponentChangeCommand. Each InspectorFrame() below is one frame of the
// inspector: BeginFrame, the widgets, EndFrame, push.
//
// The bug this pins: an undo changed the component while the inspector showed
// it, and the Value path recorded that as a fresh edit. The redo stack was
// cleared and every further Ctrl+Z flipped between the two values. The Bytes
// path kept a stale snapshot instead, so the next edit recorded the wrong
// before-state.
// =============================================================================

#include "UndoRedo/ComponentCommands.h"
#include "UndoRedo/ComponentEditTracker.h"
#include "UndoRedo/EditorCommand.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <memory>
#include <type_traits>

// OLO_TEST_LAYER: unit

namespace
{
    using OloEngine::CircleRendererComponent;
    using OloEngine::CommandHistory;
    using OloEngine::ComponentChangeCommand;
    using OloEngine::ComponentEditDetection;
    using OloEngine::ComponentEditTracker;
    using OloEngine::DiscoveredSetComponent;
    using OloEngine::Entity;
    using OloEngine::ParticleSystemComponent;
    using OloEngine::Ref;
    using OloEngine::Scene;
    using OloEngine::TextComponent;
    using OloEngine::UUID;

    static_assert(std::is_trivially_copyable_v<CircleRendererComponent>,
                  "the Bytes cases need a trivially copyable component");
    static_assert(!std::is_trivially_copyable_v<TextComponent> && std::equality_comparable<TextComponent>,
                  "the Value cases need a non-trivial component with operator==");

    template<typename T, ComponentEditDetection Detection>
    class EditTrackerFixture
    {
      public:
        EditTrackerFixture()
        {
            m_Scene = Ref<Scene>::Create();
            m_Entity = m_Scene->CreateEntity("Edited");
            m_Entity.AddComponent<T>();
        }

        T& Component()
        {
            return m_Entity.GetComponent<T>();
        }

        // One inspector frame. `widgets` is what the ImGui widgets did to the
        // component this frame; `widgetActive` is whether one is still held.
        template<typename Widgets>
        void InspectorFrame(Widgets&& widgets, bool widgetActive = false)
        {
            T& component = Component();
            m_Tracker.BeginFrame(component);
            widgets(component);
            if (m_Tracker.EndFrame(component, widgetActive))
            {
                History.PushAlreadyExecuted(std::make_unique<ComponentChangeCommand<T>>(
                    m_Scene, m_Entity.GetUUID(), m_Tracker.Snapshot(), component, "Property Change"));
            }
        }

        void IdleFrames(int count)
        {
            for (int i = 0; i < count; ++i)
            {
                InspectorFrame([](T&) {});
            }
        }

        CommandHistory History;

      private:
        Ref<Scene> m_Scene;
        Entity m_Entity;
        ComponentEditTracker<T, Detection> m_Tracker;
    };

    using TextEdits = EditTrackerFixture<TextComponent, ComponentEditDetection::Value>;
    using ParticleEdits = EditTrackerFixture<ParticleSystemComponent, ComponentEditDetection::Value>;
    using DiscoveredEdits = EditTrackerFixture<DiscoveredSetComponent, ComponentEditDetection::Value>;
    using CircleEdits = EditTrackerFixture<CircleRendererComponent, ComponentEditDetection::Bytes>;
} // namespace

TEST(ComponentEditTracker, ValueUndoIsNotRecordedAsANewEdit)
{
    TextEdits f;
    f.IdleFrames(2);
    f.InspectorFrame([](TextComponent& c)
                     { c.Kerning = 0.5f; });
    ASSERT_TRUE(f.History.CanUndo());
    // Frames pass before the user presses Ctrl+Z; they re-take the snapshot.
    f.IdleFrames(3);

    f.History.Undo();
    EXPECT_FLOAT_EQ(f.Component().Kerning, 0.0f);
    f.IdleFrames(3);
    EXPECT_FALSE(f.History.CanUndo()) << "the undo was recorded as an inspector edit";
    EXPECT_TRUE(f.History.CanRedo()) << "an inspector push cleared the redo stack";

    f.History.Redo();
    EXPECT_FLOAT_EQ(f.Component().Kerning, 0.5f);
    f.IdleFrames(3);
    EXPECT_TRUE(f.History.CanUndo());
    EXPECT_FALSE(f.History.CanRedo());

    // Exactly one entry: one more undo empties the stack instead of ping-ponging.
    f.History.Undo();
    f.IdleFrames(3);
    EXPECT_FLOAT_EQ(f.Component().Kerning, 0.0f);
    EXPECT_FALSE(f.History.CanUndo());
}

// The case the bug was found on (#1412): ParticleSystemComponent joined the
// Value path, and its Edit-mode preview also runs while the inspector is open.
TEST(ComponentEditTracker, ParticleUndoIsNotRecordedAsANewEdit)
{
    ParticleEdits f;
    const f32 original = f.Component().System.Duration;
    f.IdleFrames(2);
    f.InspectorFrame([](ParticleSystemComponent& c)
                     { c.System.Duration = 7.5f; });
    f.IdleFrames(3);

    f.History.Undo();
    EXPECT_FLOAT_EQ(f.Component().System.Duration, original);
    f.IdleFrames(3);
    EXPECT_FALSE(f.History.CanUndo());
    EXPECT_TRUE(f.History.CanRedo());
}

// DiscoveredSetComponent also joined the Value path in #1412. Its inspector is
// read-only and DiscoverySystem fills it at runtime, so the case that matters
// is that such a write is never recorded, and that an entry for it round-trips.
TEST(ComponentEditTracker, DiscoveredSetRuntimeWritesAreNotEditsAndEditsRoundTrip)
{
    DiscoveredEdits f;
    f.IdleFrames(1);
    f.Component().m_Discovered.Add(UUID(42));
    f.IdleFrames(3);
    EXPECT_FALSE(f.History.CanUndo()) << "a runtime write was recorded as an edit";

    f.InspectorFrame([](DiscoveredSetComponent& c)
                     { c.m_Discovered.Add(UUID(7)); });
    ASSERT_TRUE(f.History.CanUndo());
    f.IdleFrames(3);
    f.History.Undo();
    f.IdleFrames(3);
    ASSERT_EQ(f.Component().m_Discovered.Num(), 1);
    EXPECT_EQ(static_cast<u64>(f.Component().m_Discovered[0]), 42u);
    EXPECT_FALSE(f.History.CanUndo());
    EXPECT_TRUE(f.History.CanRedo());

    f.History.Redo();
    f.IdleFrames(3);
    EXPECT_EQ(f.Component().m_Discovered.Num(), 2);
    EXPECT_FALSE(f.History.CanRedo());
}

TEST(ComponentEditTracker, BytesEditAfterUndoRecordsTheRestoredValueAsItsBefore)
{
    CircleEdits f;
    f.IdleFrames(2);
    f.InspectorFrame([](CircleRendererComponent& c)
                     { c.Thickness = 0.25f; });
    f.IdleFrames(2);

    f.History.Undo();
    f.IdleFrames(2);
    EXPECT_FLOAT_EQ(f.Component().Thickness, 1.0f);
    EXPECT_TRUE(f.History.CanRedo());

    f.InspectorFrame([](CircleRendererComponent& c)
                     { c.Thickness = 0.75f; });
    f.IdleFrames(2);
    f.History.Undo();
    EXPECT_FLOAT_EQ(f.Component().Thickness, 1.0f) << "undo restored a stale before-state";
}

TEST(ComponentEditTracker, ChangesMadeOutsideTheInspectorAreNeverRecorded)
{
    // A gizmo drag, an MCP write or a script: the component changes between
    // frames and no inspector widget touched it.
    TextEdits text;
    text.IdleFrames(2);
    text.Component().Kerning = 2.0f;
    text.IdleFrames(3);
    EXPECT_FALSE(text.History.CanUndo());

    CircleEdits circle;
    circle.IdleFrames(2);
    circle.Component().Fade = 0.5f;
    circle.IdleFrames(3);
    EXPECT_FALSE(circle.History.CanUndo());

    // ...and the next real edit still undoes to the externally set value.
    text.InspectorFrame([](TextComponent& c)
                        { c.Kerning = 3.0f; });
    circle.InspectorFrame([](CircleRendererComponent& c)
                          { c.Fade = 0.75f; });
    circle.IdleFrames(1);
    text.History.Undo();
    circle.History.Undo();
    EXPECT_FLOAT_EQ(text.Component().Kerning, 2.0f);
    EXPECT_FLOAT_EQ(circle.Component().Fade, 0.5f);
}

TEST(ComponentEditTracker, ADragRecordsOneEntryFromThePreDragValue)
{
    TextEdits text;
    CircleEdits circle;
    text.IdleFrames(1);
    circle.IdleFrames(1);
    for (int step = 1; step <= 3; ++step)
    {
        const auto value = static_cast<f32>(step);
        text.InspectorFrame([value](TextComponent& c)
                            { c.Kerning = value; }, /*widgetActive=*/true);
        circle.InspectorFrame([value](CircleRendererComponent& c)
                              { c.Thickness = value; }, /*widgetActive=*/true);
        EXPECT_FALSE(text.History.CanUndo()) << "pushed mid-drag at step " << step;
        EXPECT_FALSE(circle.History.CanUndo()) << "pushed mid-drag at step " << step;
    }
    text.IdleFrames(1);
    circle.IdleFrames(1);

    text.History.Undo();
    circle.History.Undo();
    EXPECT_FLOAT_EQ(text.Component().Kerning, 0.0f);
    EXPECT_FLOAT_EQ(circle.Component().Thickness, 1.0f);
    EXPECT_FALSE(text.History.CanUndo());
    EXPECT_FALSE(circle.History.CanUndo());
}

TEST(ComponentEditTracker, AnEditThatEndsWhereItStartedRecordsNothing)
{
    TextEdits f;
    f.IdleFrames(1);
    f.InspectorFrame([](TextComponent& c)
                     { c.Kerning = 1.0f; }, /*widgetActive=*/true);
    f.InspectorFrame([](TextComponent& c)
                     { c.Kerning = 0.0f; }, /*widgetActive=*/true);
    f.IdleFrames(2);
    EXPECT_FALSE(f.History.CanUndo());
}
