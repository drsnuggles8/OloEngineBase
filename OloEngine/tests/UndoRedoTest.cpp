#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// The undo/redo headers are in OloEditor/src which is not on the test include path.
// We test the core infrastructure and command patterns using equivalent standalone definitions.
// For real editor commands (which depend on EditorLayer/panels), integration testing is required.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Dialogue/DialogueTypes.h"

#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Minimal Command Infrastructure (mirrors EditorCommand.h for unit testing)
// =============================================================================

namespace UndoTest
{
    class EditorCommand
    {
      public:
        virtual ~EditorCommand() = default;
        virtual void Execute() = 0;
        virtual void Undo() = 0;
        [[nodiscard]] virtual std::string GetDescription() const = 0;
    };

    class CompoundCommand : public EditorCommand
    {
      public:
        explicit CompoundCommand(std::string description) : m_Description(std::move(description)) {}

        void Add(std::unique_ptr<EditorCommand> command)
        {
            m_Commands.push_back(std::move(command));
        }

        void Execute() override
        {
            for (auto& cmd : m_Commands)
                cmd->Execute();
        }

        void Undo() override
        {
            for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
                (*it)->Undo();
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }
        [[nodiscard]] bool IsEmpty() const
        {
            return m_Commands.empty();
        }
        [[nodiscard]] std::size_t Size() const
        {
            return m_Commands.size();
        }

      private:
        std::vector<std::unique_ptr<EditorCommand>> m_Commands;
        std::string m_Description;
    };

    class CommandHistory
    {
      public:
        static constexpr std::size_t MaxHistorySize = 128;

        void Execute(std::unique_ptr<EditorCommand> command)
        {
            command->Execute();
            if (!m_RedoStack.empty() && m_SavePointValid && m_SavePointVersion > m_Version)
            {
                m_SavePointValid = false;
            }
            m_UndoStack.push_back(std::move(command));
            m_RedoStack.clear();
            ++m_Version;
            while (m_UndoStack.size() > MaxHistorySize)
            {
                m_UndoStack.pop_front();
                TrimSavePoint();
            }
        }

        void PushAlreadyExecuted(std::unique_ptr<EditorCommand> command)
        {
            if (!m_RedoStack.empty() && m_SavePointValid && m_SavePointVersion > m_Version)
            {
                m_SavePointValid = false;
            }
            m_UndoStack.push_back(std::move(command));
            m_RedoStack.clear();
            ++m_Version;
            while (m_UndoStack.size() > MaxHistorySize)
            {
                m_UndoStack.pop_front();
                TrimSavePoint();
            }
        }

        void Undo()
        {
            if (m_UndoStack.empty())
                return;
            auto command = std::move(m_UndoStack.back());
            m_UndoStack.pop_back();
            command->Undo();
            m_RedoStack.push_back(std::move(command));
            --m_Version;
        }

        void Redo()
        {
            if (m_RedoStack.empty())
                return;
            auto command = std::move(m_RedoStack.back());
            m_RedoStack.pop_back();
            command->Execute();
            m_UndoStack.push_back(std::move(command));
            ++m_Version;
        }

        [[nodiscard]] bool CanUndo() const
        {
            return !m_UndoStack.empty();
        }
        [[nodiscard]] bool CanRedo() const
        {
            return !m_RedoStack.empty();
        }
        [[nodiscard]] std::string GetUndoDescription() const
        {
            return m_UndoStack.empty() ? "" : m_UndoStack.back()->GetDescription();
        }
        [[nodiscard]] std::string GetRedoDescription() const
        {
            return m_RedoStack.empty() ? "" : m_RedoStack.back()->GetDescription();
        }
        [[nodiscard]] std::size_t UndoSize() const
        {
            return m_UndoStack.size();
        }
        [[nodiscard]] std::size_t RedoSize() const
        {
            return m_RedoStack.size();
        }

        void MarkSaved()
        {
            m_SavePointVersion = m_Version;
            m_SavePointValid = true;
        }

        [[nodiscard]] bool IsDirty() const
        {
            if (!m_SavePointValid)
                return true;
            return m_Version != m_SavePointVersion;
        }

        void Clear()
        {
            m_UndoStack.clear();
            m_RedoStack.clear();
            m_Version = 0;
            m_SavePointVersion = 0;
            m_SavePointValid = true;
        }

      private:
        void TrimSavePoint()
        {
            if (m_SavePointValid && m_SavePointVersion > 0)
            {
                --m_SavePointVersion;
            }
            else if (m_SavePointVersion == 0)
            {
                m_SavePointValid = false;
            }
        }

        std::deque<std::unique_ptr<EditorCommand>> m_UndoStack;
        std::deque<std::unique_ptr<EditorCommand>> m_RedoStack;
        std::size_t m_Version = 0;
        std::size_t m_SavePointVersion = 0;
        bool m_SavePointValid = true;
    };

    // =========================================================================
    // Test command that increments/decrements an integer
    // =========================================================================
    class IncrementCommand : public EditorCommand
    {
      public:
        explicit IncrementCommand(int& target, int amount = 1)
            : m_Target(target), m_Amount(amount) {}

        void Execute() override
        {
            m_Target += m_Amount;
        }
        void Undo() override
        {
            m_Target -= m_Amount;
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return "Increment";
        }

      private:
        int& m_Target;
        int m_Amount;
    };

    // =========================================================================
    // Transform command (mirrors TransformChangeCommand)
    // =========================================================================
    class TransformChangeCommand : public EditorCommand
    {
      public:
        TransformChangeCommand(Ref<Scene> scene, UUID entityUUID,
                               glm::vec3 oldT, glm::vec3 oldR, glm::vec3 oldS,
                               glm::vec3 newT, glm::vec3 newR, glm::vec3 newS)
            : m_Scene(std::move(scene)), m_UUID(entityUUID),
              m_OldT(oldT), m_OldR(oldR), m_OldS(oldS),
              m_NewT(newT), m_NewR(newR), m_NewS(newS) {}

        void Execute() override
        {
            Apply(m_NewT, m_NewR, m_NewS);
        }
        void Undo() override
        {
            Apply(m_OldT, m_OldR, m_OldS);
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return "Transform Change";
        }

      private:
        void Apply(const glm::vec3& t, const glm::vec3& r, const glm::vec3& s)
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (!e)
                return;
            auto& tc = e->GetComponent<TransformComponent>();
            tc.Translation = t;
            tc.Rotation = r;
            tc.Scale = s;
        }

        Ref<Scene> m_Scene;
        UUID m_UUID;
        glm::vec3 m_OldT, m_OldR, m_OldS;
        glm::vec3 m_NewT, m_NewR, m_NewS;
    };

    // =========================================================================
    // Create/Delete entity commands (mirrors EntityCommands.h)
    // =========================================================================
    class CreateEntityCommand : public EditorCommand
    {
      public:
        CreateEntityCommand(Ref<Scene> scene, std::string name,
                            std::function<void(Entity)> onCreated = nullptr,
                            std::function<void()> onDestroyed = nullptr)
            : m_Scene(std::move(scene)), m_Name(std::move(name)),
              m_OnCreated(std::move(onCreated)), m_OnDestroyed(std::move(onDestroyed)) {}

        void Execute() override
        {
            Entity entity;
            if (m_UUID != UUID(0))
                entity = m_Scene->CreateEntityWithUUID(m_UUID, m_Name);
            else
            {
                entity = m_Scene->CreateEntity(m_Name);
                m_UUID = entity.GetUUID();
            }
            if (m_OnCreated)
                m_OnCreated(entity);
        }

        void Undo() override
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e)
                m_Scene->DestroyEntity(*e);
            if (m_OnDestroyed)
                m_OnDestroyed();
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Create Entity '" + m_Name + "'";
        }
        [[nodiscard]] UUID GetEntityUUID() const
        {
            return m_UUID;
        }

      private:
        Ref<Scene> m_Scene;
        std::string m_Name;
        UUID m_UUID{ 0 };
        std::function<void(Entity)> m_OnCreated;
        std::function<void()> m_OnDestroyed;
    };

    class DeleteEntityCommand : public EditorCommand
    {
      public:
        DeleteEntityCommand(Ref<Scene> scene, Entity entity)
            : m_Scene(std::move(scene)), m_UUID(entity.GetUUID()),
              m_Name(entity.GetComponent<TagComponent>().Tag)
        {
            // Snapshot transform
            if (entity.HasComponent<TransformComponent>())
            {
                m_HasTransform = true;
                m_Transform = entity.GetComponent<TransformComponent>();
            }
        }

        void Execute() override
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e)
                m_Scene->DestroyEntity(*e);
        }

        void Undo() override
        {
            Entity restored = m_Scene->CreateEntityWithUUID(m_UUID, m_Name);
            if (m_HasTransform)
                restored.GetComponent<TransformComponent>() = m_Transform;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Delete Entity '" + m_Name + "'";
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_UUID;
        std::string m_Name;
        bool m_HasTransform = false;
        TransformComponent m_Transform;
    };

    // =========================================================================
    // Rename entity command (mirrors RenameEntityCommand)
    // =========================================================================
    class RenameEntityCommand : public EditorCommand
    {
      public:
        RenameEntityCommand(Ref<Scene> scene, UUID uuid, std::string oldName, std::string newName)
            : m_Scene(std::move(scene)), m_UUID(uuid),
              m_OldName(std::move(oldName)), m_NewName(std::move(newName)) {}

        void Execute() override
        {
            SetName(m_NewName);
        }
        void Undo() override
        {
            SetName(m_OldName);
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return "Rename Entity";
        }

      private:
        void SetName(const std::string& name)
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e)
                e->GetComponent<TagComponent>().Tag = name;
        }

        Ref<Scene> m_Scene;
        UUID m_UUID;
        std::string m_OldName, m_NewName;
    };

    // =========================================================================
    // Component add/remove commands (mirrors ComponentCommands.h)
    // =========================================================================
    template<typename T>
    class AddComponentCommand : public EditorCommand
    {
      public:
        AddComponentCommand(Ref<Scene> scene, UUID uuid) : m_Scene(std::move(scene)), m_UUID(uuid) {}

        void Execute() override
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e && !e->HasComponent<T>())
                e->AddComponent<T>();
        }

        void Undo() override
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e && e->HasComponent<T>())
                e->RemoveComponent<T>();
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Add Component";
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_UUID;
    };

    template<typename T>
    class RemoveComponentCommand : public EditorCommand
    {
      public:
        RemoveComponentCommand(Ref<Scene> scene, UUID uuid, const T& snapshot)
            : m_Scene(std::move(scene)), m_UUID(uuid), m_Snapshot(snapshot) {}

        void Execute() override
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e && e->HasComponent<T>())
                e->RemoveComponent<T>();
        }

        void Undo() override
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e && !e->HasComponent<T>())
                e->AddOrReplaceComponent<T>(m_Snapshot);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Remove Component";
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_UUID;
        T m_Snapshot;
    };

    // =========================================================================
    // Component change command (mirrors ComponentChangeCommand<T>)
    // =========================================================================
    template<typename T>
    class ComponentChangeCommand : public EditorCommand
    {
      public:
        ComponentChangeCommand(Ref<Scene> scene, UUID uuid, T oldData, T newData, std::string desc = "Property Change")
            : m_Scene(std::move(scene)), m_UUID(uuid),
              m_OldData(std::move(oldData)), m_NewData(std::move(newData)), m_Description(std::move(desc)) {}

        void Execute() override
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e && e->HasComponent<T>())
                e->GetComponent<T>() = m_NewData;
        }

        void Undo() override
        {
            auto e = m_Scene->TryGetEntityWithUUID(m_UUID);
            if (e && e->HasComponent<T>())
                e->GetComponent<T>() = m_OldData;
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_UUID;
        T m_OldData, m_NewData;
        std::string m_Description;
    };

    // =========================================================================
    // Post-process change command (mirrors PostProcessChangeCommand)
    // =========================================================================
    class PostProcessChangeCommand : public EditorCommand
    {
      public:
        PostProcessChangeCommand(PostProcessSettings* target,
                                 PostProcessSettings oldSettings, PostProcessSettings newSettings)
            : m_Target(target), m_OldSettings(oldSettings), m_NewSettings(newSettings) {}

        void Execute() override
        {
            if (m_Target)
                *m_Target = m_NewSettings;
        }
        void Undo() override
        {
            if (m_Target)
                *m_Target = m_OldSettings;
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return "Post-Process Change";
        }

      private:
        PostProcessSettings* m_Target;
        PostProcessSettings m_OldSettings;
        PostProcessSettings m_NewSettings;
    };

    // =========================================================================
    // Terrain sculpt command (mirrors TerrainSculptCommand, simplified for testing)
    // =========================================================================
    class TerrainSculptCommand : public EditorCommand
    {
      public:
        TerrainSculptCommand(std::vector<f32>& heightmap, u32 resolution,
                             u32 regionX, u32 regionY, u32 regionW, u32 regionH,
                             std::vector<f32> oldHeights, std::vector<f32> newHeights)
            : m_Heightmap(heightmap), m_Resolution(resolution),
              m_RegionX(regionX), m_RegionY(regionY), m_RegionW(regionW), m_RegionH(regionH),
              m_OldHeights(std::move(oldHeights)), m_NewHeights(std::move(newHeights)) {}

        void Execute() override
        {
            ApplyHeights(m_NewHeights);
        }
        void Undo() override
        {
            ApplyHeights(m_OldHeights);
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return "Terrain Sculpt";
        }

      private:
        void ApplyHeights(const std::vector<f32>& heights)
        {
            for (u32 row = 0; row < m_RegionH; ++row)
            {
                u32 srcIdx = row * m_RegionW;
                u32 dstIdx = (m_RegionY + row) * m_Resolution + m_RegionX;
                std::memcpy(&m_Heightmap[dstIdx], &heights[srcIdx], m_RegionW * sizeof(f32));
            }
        }

        std::vector<f32>& m_Heightmap;
        u32 m_Resolution;
        u32 m_RegionX, m_RegionY, m_RegionW, m_RegionH;
        std::vector<f32> m_OldHeights, m_NewHeights;
    };

    // =========================================================================
    // Streaming settings change command (mirrors StreamingSettingsChangeCommand)
    // =========================================================================
    class StreamingSettingsChangeCommand : public EditorCommand
    {
      public:
        StreamingSettingsChangeCommand(StreamingSettings& target,
                                       StreamingSettings oldSettings, StreamingSettings newSettings)
            : m_Target(target), m_OldSettings(std::move(oldSettings)), m_NewSettings(std::move(newSettings)) {}

        void Execute() override
        {
            m_Target = m_NewSettings;
        }
        void Undo() override
        {
            m_Target = m_OldSettings;
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return "Streaming Settings Change";
        }

      private:
        StreamingSettings& m_Target;
        StreamingSettings m_OldSettings;
        StreamingSettings m_NewSettings;
    };

    // =========================================================================
    // Dialogue editor change command (mirrors DialogueEditorChangeCommand)
    // =========================================================================
    struct DialogueEditorSnapshot
    {
        std::vector<DialogueNodeData> Nodes;
        std::vector<DialogueConnection> Connections;
        UUID RootNodeID;
    };

    class DialogueEditorChangeCommand : public EditorCommand
    {
      public:
        DialogueEditorChangeCommand(DialogueEditorSnapshot oldState, DialogueEditorSnapshot newState,
                                    std::function<void(const DialogueEditorSnapshot&)> applyFn,
                                    std::string desc = "Dialogue Edit")
            : m_OldState(std::move(oldState)), m_NewState(std::move(newState)),
              m_ApplyFn(std::move(applyFn)), m_Description(std::move(desc)) {}

        void Execute() override
        {
            m_ApplyFn(m_NewState);
        }
        void Undo() override
        {
            m_ApplyFn(m_OldState);
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        DialogueEditorSnapshot m_OldState;
        DialogueEditorSnapshot m_NewState;
        std::function<void(const DialogueEditorSnapshot&)> m_ApplyFn;
        std::string m_Description;
    };

    // =========================================================================
    // Input action map change command (mirrors InputActionMapChangeCommand)
    // =========================================================================
    class InputActionMapChangeCommand : public EditorCommand
    {
      public:
        InputActionMapChangeCommand(InputActionMap& target,
                                    InputActionMap oldMap, InputActionMap newMap,
                                    std::string desc = "Input Settings Change")
            : m_Target(target), m_OldMap(std::move(oldMap)), m_NewMap(std::move(newMap)),
              m_Description(std::move(desc)) {}

        void Execute() override
        {
            m_Target = m_NewMap;
        }
        void Undo() override
        {
            m_Target = m_OldMap;
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        InputActionMap& m_Target;
        InputActionMap m_OldMap;
        InputActionMap m_NewMap;
        std::string m_Description;
    };

} // namespace UndoTest

// =============================================================================
// Helper: create a scene with a named entity
// =============================================================================
static Ref<Scene> MakeScene()
{
    return Scene::Create();
}

// =============================================================================
// CommandHistory Tests
// =============================================================================

TEST(CommandHistory, InitiallyEmpty)
{
    UndoTest::CommandHistory history;
    EXPECT_FALSE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());
    EXPECT_EQ(history.UndoSize(), 0u);
    EXPECT_EQ(history.RedoSize(), 0u);
}

TEST(CommandHistory, ExecuteAndUndo)
{
    UndoTest::CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 5));
    EXPECT_EQ(value, 5);
    EXPECT_TRUE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());

    history.Undo();
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(history.CanUndo());
    EXPECT_TRUE(history.CanRedo());
}

TEST(CommandHistory, UndoAndRedo)
{
    UndoTest::CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 3));
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 7));
    EXPECT_EQ(value, 10);

    history.Undo();
    EXPECT_EQ(value, 3);

    history.Redo();
    EXPECT_EQ(value, 10);
}

TEST(CommandHistory, NewCommandClearsRedoStack)
{
    UndoTest::CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 1));
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 2));
    history.Undo(); // redo stack has +2
    EXPECT_TRUE(history.CanRedo());

    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 10));
    EXPECT_FALSE(history.CanRedo()); // redo stack cleared
    EXPECT_EQ(value, 11);            // 1 + 10
}

TEST(CommandHistory, PushAlreadyExecutedDoesNotCallExecute)
{
    UndoTest::CommandHistory history;
    int value = 42;

    history.PushAlreadyExecuted(std::make_unique<UndoTest::IncrementCommand>(value, 5));
    EXPECT_EQ(value, 42); // Not changed

    history.Undo();
    EXPECT_EQ(value, 37); // Undone: 42 - 5
}

TEST(CommandHistory, MultipleUndoRedo)
{
    UndoTest::CommandHistory history;
    int value = 0;

    for (int i = 1; i <= 5; ++i)
        history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, i));
    EXPECT_EQ(value, 15); // 1+2+3+4+5

    // Undo all
    for (int i = 0; i < 5; ++i)
        history.Undo();
    EXPECT_EQ(value, 0);

    // Redo all
    for (int i = 0; i < 5; ++i)
        history.Redo();
    EXPECT_EQ(value, 15);
}

TEST(CommandHistory, MaxHistorySizeEnforced)
{
    UndoTest::CommandHistory history;
    int value = 0;

    for (int i = 0; i < 200; ++i)
        history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 1));

    EXPECT_EQ(value, 200);
    EXPECT_EQ(history.UndoSize(), UndoTest::CommandHistory::MaxHistorySize);
}

TEST(CommandHistory, Clear)
{
    UndoTest::CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 1));
    history.Undo();
    EXPECT_TRUE(history.CanRedo());

    history.Clear();
    EXPECT_FALSE(history.CanUndo());
    EXPECT_FALSE(history.CanRedo());
}

TEST(CommandHistory, Descriptions)
{
    UndoTest::CommandHistory history;
    int value = 0;

    EXPECT_EQ(history.GetUndoDescription(), "");
    EXPECT_EQ(history.GetRedoDescription(), "");

    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value));
    EXPECT_EQ(history.GetUndoDescription(), "Increment");

    history.Undo();
    EXPECT_EQ(history.GetRedoDescription(), "Increment");
}

TEST(CommandHistory, UndoOnEmptyDoesNothing)
{
    UndoTest::CommandHistory history;
    history.Undo(); // Should not crash
    EXPECT_FALSE(history.CanUndo());
}

TEST(CommandHistory, RedoOnEmptyDoesNothing)
{
    UndoTest::CommandHistory history;
    history.Redo(); // Should not crash
    EXPECT_FALSE(history.CanRedo());
}

// =============================================================================
// CompoundCommand Tests
// =============================================================================

TEST(CompoundCommand, EmptyCompoundIsEmpty)
{
    UndoTest::CompoundCommand compound("Test");
    EXPECT_TRUE(compound.IsEmpty());
    EXPECT_EQ(compound.Size(), 0u);
}

TEST(CompoundCommand, ExecutesAllInOrder)
{
    int value = 0;
    auto compound = std::make_unique<UndoTest::CompoundCommand>("Multi-increment");
    compound->Add(std::make_unique<UndoTest::IncrementCommand>(value, 1));
    compound->Add(std::make_unique<UndoTest::IncrementCommand>(value, 10));
    compound->Add(std::make_unique<UndoTest::IncrementCommand>(value, 100));
    EXPECT_EQ(compound->Size(), 3u);

    compound->Execute();
    EXPECT_EQ(value, 111);
}

TEST(CompoundCommand, UndoesInReverseOrder)
{
    std::vector<int> order;
    // Use a custom command that records execution/undo order
    struct OrderTracker : UndoTest::EditorCommand
    {
        OrderTracker(std::vector<int>& log, int id) : m_Log(log), m_Id(id) {}
        void Execute() override
        {
            m_Log.push_back(m_Id);
        }
        void Undo() override
        {
            m_Log.push_back(-m_Id);
        }
        [[nodiscard]] std::string GetDescription() const override
        {
            return "Track";
        }
        std::vector<int>& m_Log;
        int m_Id;
    };

    auto compound = std::make_unique<UndoTest::CompoundCommand>("Ordered");
    compound->Add(std::make_unique<OrderTracker>(order, 1));
    compound->Add(std::make_unique<OrderTracker>(order, 2));
    compound->Add(std::make_unique<OrderTracker>(order, 3));

    compound->Execute();
    EXPECT_EQ(order, (std::vector<int>{ 1, 2, 3 }));

    order.clear();
    compound->Undo();
    EXPECT_EQ(order, (std::vector<int>{ -3, -2, -1 }));
}

TEST(CompoundCommand, WorksWithCommandHistory)
{
    UndoTest::CommandHistory history;
    int a = 0, b = 0;

    auto compound = std::make_unique<UndoTest::CompoundCommand>("Both");
    compound->Add(std::make_unique<UndoTest::IncrementCommand>(a, 5));
    compound->Add(std::make_unique<UndoTest::IncrementCommand>(b, 10));

    history.Execute(std::move(compound));
    EXPECT_EQ(a, 5);
    EXPECT_EQ(b, 10);

    history.Undo();
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 0);

    history.Redo();
    EXPECT_EQ(a, 5);
    EXPECT_EQ(b, 10);
}

// =============================================================================
// Entity CRUD Command Tests
// =============================================================================

TEST(CreateEntityCommand, CreatesAndDestroysEntity)
{
    auto scene = MakeScene();
    UndoTest::CommandHistory history;
    UUID createdUUID{ 0 };

    auto cmd = std::make_unique<UndoTest::CreateEntityCommand>(scene, "TestEntity",
                                                               [&](Entity e)
                                                               { createdUUID = e.GetUUID(); });

    history.Execute(std::move(cmd));
    EXPECT_NE(createdUUID, UUID(0));

    auto entity = scene->TryGetEntityWithUUID(createdUUID);
    ASSERT_TRUE(entity.has_value());
    EXPECT_EQ(entity->GetComponent<TagComponent>().Tag, "TestEntity");

    // Undo should destroy the entity
    history.Undo();
    EXPECT_FALSE(scene->TryGetEntityWithUUID(createdUUID).has_value());

    // Redo should recreate with same UUID
    history.Redo();
    entity = scene->TryGetEntityWithUUID(createdUUID);
    ASSERT_TRUE(entity.has_value());
    EXPECT_EQ(entity->GetComponent<TagComponent>().Tag, "TestEntity");
}

TEST(DeleteEntityCommand, DeletesAndRestoresEntity)
{
    auto scene = MakeScene();
    Entity entity = scene->CreateEntity("ToDelete");
    UUID uuid = entity.GetUUID();

    // Set a custom transform
    auto& tc = entity.GetComponent<TransformComponent>();
    tc.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
    tc.Scale = glm::vec3(4.0f, 5.0f, 6.0f);

    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::DeleteEntityCommand>(scene, entity));

    EXPECT_FALSE(scene->TryGetEntityWithUUID(uuid).has_value());

    // Undo should restore
    history.Undo();
    auto restored = scene->TryGetEntityWithUUID(uuid);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->GetComponent<TagComponent>().Tag, "ToDelete");
    EXPECT_EQ(restored->GetComponent<TransformComponent>().Translation, glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(restored->GetComponent<TransformComponent>().Scale, glm::vec3(4.0f, 5.0f, 6.0f));
}

TEST(RenameEntityCommand, RenamesAndRestores)
{
    auto scene = MakeScene();
    Entity entity = scene->CreateEntity("OldName");
    UUID uuid = entity.GetUUID();

    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::RenameEntityCommand>(scene, uuid, "OldName", "NewName"));
    EXPECT_EQ(entity.GetComponent<TagComponent>().Tag, "NewName");

    history.Undo();
    EXPECT_EQ(entity.GetComponent<TagComponent>().Tag, "OldName");

    history.Redo();
    EXPECT_EQ(entity.GetComponent<TagComponent>().Tag, "NewName");
}

// =============================================================================
// Transform Change Command Tests
// =============================================================================

TEST(TransformChangeCommand, ChangesAndRestoresTransform)
{
    auto scene = MakeScene();
    Entity entity = scene->CreateEntity("Movable");
    UUID uuid = entity.GetUUID();

    auto& tc = entity.GetComponent<TransformComponent>();
    glm::vec3 oldT = tc.Translation;
    glm::vec3 oldR = tc.Rotation;
    glm::vec3 oldS = tc.Scale;

    glm::vec3 newT(10.0f, 20.0f, 30.0f);
    glm::vec3 newR(0.1f, 0.2f, 0.3f);
    glm::vec3 newS(2.0f, 3.0f, 4.0f);

    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::TransformChangeCommand>(
        scene, uuid, oldT, oldR, oldS, newT, newR, newS));

    EXPECT_EQ(tc.Translation, newT);
    EXPECT_EQ(tc.Rotation, newR);
    EXPECT_EQ(tc.Scale, newS);

    history.Undo();
    EXPECT_EQ(tc.Translation, oldT);
    EXPECT_EQ(tc.Rotation, oldR);
    EXPECT_EQ(tc.Scale, oldS);
}

// =============================================================================
// Component Add/Remove Command Tests
// =============================================================================

TEST(AddComponentCommand, AddsAndRemovesComponent)
{
    auto scene = MakeScene();
    Entity entity = scene->CreateEntity("Test");
    UUID uuid = entity.GetUUID();

    EXPECT_FALSE(entity.HasComponent<SpriteRendererComponent>());

    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::AddComponentCommand<SpriteRendererComponent>>(scene, uuid));
    EXPECT_TRUE(entity.HasComponent<SpriteRendererComponent>());

    history.Undo();
    EXPECT_FALSE(entity.HasComponent<SpriteRendererComponent>());

    history.Redo();
    EXPECT_TRUE(entity.HasComponent<SpriteRendererComponent>());
}

TEST(RemoveComponentCommand, RemovesAndRestoresWithSnapshot)
{
    auto scene = MakeScene();
    Entity entity = scene->CreateEntity("Test");
    UUID uuid = entity.GetUUID();

    auto& sprite = entity.AddComponent<SpriteRendererComponent>();
    sprite.Color = glm::vec4(0.5f, 0.6f, 0.7f, 0.8f);
    sprite.TilingFactor = 3.14f;

    UndoTest::CommandHistory history;
    auto snapshot = entity.GetComponent<SpriteRendererComponent>();
    history.Execute(std::make_unique<UndoTest::RemoveComponentCommand<SpriteRendererComponent>>(scene, uuid, snapshot));
    EXPECT_FALSE(entity.HasComponent<SpriteRendererComponent>());

    history.Undo();
    ASSERT_TRUE(entity.HasComponent<SpriteRendererComponent>());
    auto& restored = entity.GetComponent<SpriteRendererComponent>();
    EXPECT_EQ(restored.Color, glm::vec4(0.5f, 0.6f, 0.7f, 0.8f));
    EXPECT_FLOAT_EQ(restored.TilingFactor, 3.14f);
}

// =============================================================================
// Component Change Command Tests
// =============================================================================

TEST(ComponentChangeCommand, ChangesAndRestoresComponent)
{
    auto scene = MakeScene();
    Entity entity = scene->CreateEntity("Test");
    UUID uuid = entity.GetUUID();

    auto& tc = entity.GetComponent<TransformComponent>();
    TransformComponent oldState = tc; // default
    tc.Translation = glm::vec3(5.0f, 6.0f, 7.0f);
    TransformComponent newState = tc;

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(
        std::make_unique<UndoTest::ComponentChangeCommand<TransformComponent>>(scene, uuid, oldState, newState));

    EXPECT_EQ(tc.Translation, glm::vec3(5.0f, 6.0f, 7.0f));

    history.Undo();
    EXPECT_EQ(tc.Translation, oldState.Translation);

    history.Redo();
    EXPECT_EQ(tc.Translation, glm::vec3(5.0f, 6.0f, 7.0f));
}

TEST(ComponentChangeCommand, RobustToEntityNotFound)
{
    auto scene = MakeScene();
    UUID bogusUUID{ 9999999 };

    TransformComponent old{}, new_{};
    new_.Translation = glm::vec3(1.0f);

    auto cmd = std::make_unique<UndoTest::ComponentChangeCommand<TransformComponent>>(scene, bogusUUID, old, new_);
    cmd->Execute(); // Should not crash
    cmd->Undo();    // Should not crash
}

// =============================================================================
// Post-Process Change Command Tests
// =============================================================================

TEST(PostProcessChangeCommand, ChangesAndRestoresSettings)
{
    PostProcessSettings settings;
    PostProcessSettings oldSettings = settings;

    settings.BloomEnabled = true;
    settings.BloomThreshold = 2.0f;
    settings.Exposure = 3.0f;
    PostProcessSettings newSettings = settings;

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(
        std::make_unique<UndoTest::PostProcessChangeCommand>(&settings, oldSettings, newSettings));

    EXPECT_TRUE(settings.BloomEnabled);
    EXPECT_FLOAT_EQ(settings.BloomThreshold, 2.0f);
    EXPECT_FLOAT_EQ(settings.Exposure, 3.0f);

    history.Undo();
    EXPECT_FALSE(settings.BloomEnabled);
    EXPECT_FLOAT_EQ(settings.BloomThreshold, 1.0f); // default
    EXPECT_FLOAT_EQ(settings.Exposure, 1.0f);       // default

    history.Redo();
    EXPECT_TRUE(settings.BloomEnabled);
    EXPECT_FLOAT_EQ(settings.BloomThreshold, 2.0f);
}

TEST(PostProcessChangeCommand, AllFieldsPreserved)
{
    PostProcessSettings settings;
    PostProcessSettings oldSettings = settings;

    // Modify all fields
    settings.Tonemap = TonemapOperator::ACES;
    settings.Exposure = 5.0f;
    settings.Gamma = 1.8f;
    settings.BloomEnabled = true;
    settings.BloomThreshold = 3.0f;
    settings.BloomIntensity = 2.0f;
    settings.BloomIterations = 8;
    settings.VignetteEnabled = true;
    settings.VignetteIntensity = 0.7f;
    settings.VignetteSmoothness = 0.9f;
    settings.ChromaticAberrationEnabled = true;
    settings.ChromaticAberrationIntensity = 0.01f;
    settings.FXAAEnabled = true;
    settings.DOFEnabled = true;
    settings.DOFFocusDistance = 20.0f;
    settings.DOFFocusRange = 10.0f;
    settings.DOFBokehRadius = 5.0f;
    settings.MotionBlurEnabled = true;
    settings.MotionBlurStrength = 0.8f;
    settings.MotionBlurSamples = 16;
    settings.SSAOEnabled = true;
    settings.SSAORadius = 1.0f;

    PostProcessSettings newSettings = settings;

    // Apply and undo — verify every field is restored
    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(
        std::make_unique<UndoTest::PostProcessChangeCommand>(&settings, oldSettings, newSettings));

    history.Undo();
    EXPECT_EQ(settings.Tonemap, TonemapOperator::Reinhard);
    EXPECT_FLOAT_EQ(settings.Exposure, 1.0f);
    EXPECT_FALSE(settings.BloomEnabled);
    EXPECT_FALSE(settings.VignetteEnabled);
    EXPECT_FALSE(settings.FXAAEnabled);
    EXPECT_FALSE(settings.DOFEnabled);
    EXPECT_FALSE(settings.MotionBlurEnabled);
    EXPECT_FALSE(settings.SSAOEnabled);

    history.Redo();
    EXPECT_EQ(settings.Tonemap, TonemapOperator::ACES);
    EXPECT_FLOAT_EQ(settings.Exposure, 5.0f);
    EXPECT_TRUE(settings.BloomEnabled);
    EXPECT_TRUE(settings.VignetteEnabled);
    EXPECT_TRUE(settings.FXAAEnabled);
    EXPECT_TRUE(settings.DOFEnabled);
    EXPECT_TRUE(settings.MotionBlurEnabled);
    EXPECT_TRUE(settings.SSAOEnabled);
}

// =============================================================================
// Terrain Sculpt Command Tests
// =============================================================================

TEST(TerrainSculptCommand, ModifiesAndRestoresHeightRegion)
{
    // Create a 4x4 heightmap
    const u32 resolution = 4;
    std::vector<f32> heightmap(resolution * resolution, 0.0f);

    // Old heights for a 2x2 region at (1,1)
    std::vector<f32> oldHeights = { 0.0f, 0.0f, 0.0f, 0.0f };
    std::vector<f32> newHeights = { 0.5f, 0.6f, 0.7f, 0.8f };

    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::TerrainSculptCommand>(
        heightmap, resolution, 1, 1, 2, 2, oldHeights, newHeights));

    // Check that region was modified
    EXPECT_FLOAT_EQ(heightmap[1 * resolution + 1], 0.5f);
    EXPECT_FLOAT_EQ(heightmap[1 * resolution + 2], 0.6f);
    EXPECT_FLOAT_EQ(heightmap[2 * resolution + 1], 0.7f);
    EXPECT_FLOAT_EQ(heightmap[2 * resolution + 2], 0.8f);
    // Unchanged pixels
    EXPECT_FLOAT_EQ(heightmap[0], 0.0f);
    EXPECT_FLOAT_EQ(heightmap[3], 0.0f);

    history.Undo();
    EXPECT_FLOAT_EQ(heightmap[1 * resolution + 1], 0.0f);
    EXPECT_FLOAT_EQ(heightmap[1 * resolution + 2], 0.0f);
    EXPECT_FLOAT_EQ(heightmap[2 * resolution + 1], 0.0f);
    EXPECT_FLOAT_EQ(heightmap[2 * resolution + 2], 0.0f);

    history.Redo();
    EXPECT_FLOAT_EQ(heightmap[1 * resolution + 1], 0.5f);
    EXPECT_FLOAT_EQ(heightmap[2 * resolution + 2], 0.8f);
}

TEST(TerrainSculptCommand, LargeRegionSnapshot)
{
    const u32 resolution = 64;
    std::vector<f32> heightmap(resolution * resolution, 1.0f);

    // Snapshot a 10x10 region at (5, 5)
    const u32 regionW = 10, regionH = 10;
    std::vector<f32> oldHeights(regionW * regionH, 1.0f);
    std::vector<f32> newHeights(regionW * regionH);
    for (u32 i = 0; i < regionW * regionH; ++i)
        newHeights[i] = static_cast<f32>(i) / static_cast<f32>(regionW * regionH);

    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::TerrainSculptCommand>(
        heightmap, resolution, 5, 5, regionW, regionH, oldHeights, newHeights));

    // Verify center of modified region
    EXPECT_NE(heightmap[5 * resolution + 5], 1.0f);
    // Verify outside region is unmodified
    EXPECT_FLOAT_EQ(heightmap[0], 1.0f);

    history.Undo();
    // All should be back to 1.0f
    for (u32 row = 0; row < regionH; ++row)
        for (u32 col = 0; col < regionW; ++col)
            EXPECT_FLOAT_EQ(heightmap[(5 + row) * resolution + (5 + col)], 1.0f);
}

// =============================================================================
// Integration: Complex undo/redo scenarios
// =============================================================================

TEST(UndoRedoIntegration, CreateThenModifyThenDelete)
{
    auto scene = MakeScene();
    UndoTest::CommandHistory history;
    UUID uuid{ 0 };

    // 1. Create entity
    history.Execute(std::make_unique<UndoTest::CreateEntityCommand>(scene, "Player",
                                                                    [&](Entity e)
                                                                    { uuid = e.GetUUID(); }));
    ASSERT_NE(uuid, UUID(0));

    // 2. Modify transform
    auto entity = *scene->TryGetEntityWithUUID(uuid);
    auto& tc = entity.GetComponent<TransformComponent>();
    glm::vec3 oldT = tc.Translation;
    tc.Translation = glm::vec3(100.0f, 0.0f, 0.0f);
    history.PushAlreadyExecuted(std::make_unique<UndoTest::TransformChangeCommand>(
        scene, uuid, oldT, glm::vec3(0.0f), glm::vec3(1.0f),
        glm::vec3(100.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)));

    // 3. Delete entity
    history.Execute(std::make_unique<UndoTest::DeleteEntityCommand>(scene, entity));
    EXPECT_FALSE(scene->TryGetEntityWithUUID(uuid).has_value());

    // Undo delete
    history.Undo();
    EXPECT_TRUE(scene->TryGetEntityWithUUID(uuid).has_value());

    // Undo transform
    history.Undo();
    entity = *scene->TryGetEntityWithUUID(uuid);
    EXPECT_EQ(entity.GetComponent<TransformComponent>().Translation, oldT);

    // Undo create
    history.Undo();
    EXPECT_FALSE(scene->TryGetEntityWithUUID(uuid).has_value());
}

TEST(UndoRedoIntegration, InterleavedEntityAndComponentOps)
{
    auto scene = MakeScene();
    UndoTest::CommandHistory history;

    Entity entity = scene->CreateEntity("TestEntity");
    UUID uuid = entity.GetUUID();

    // Add SpriteRendererComponent
    history.Execute(std::make_unique<UndoTest::AddComponentCommand<SpriteRendererComponent>>(scene, uuid));
    ASSERT_TRUE(entity.HasComponent<SpriteRendererComponent>());

    // Modify sprite color
    auto& sprite = entity.GetComponent<SpriteRendererComponent>();
    auto oldSprite = sprite;
    sprite.Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    history.PushAlreadyExecuted(
        std::make_unique<UndoTest::ComponentChangeCommand<SpriteRendererComponent>>(scene, uuid, oldSprite, sprite));

    // Add CameraComponent
    history.Execute(std::make_unique<UndoTest::AddComponentCommand<CameraComponent>>(scene, uuid));
    EXPECT_TRUE(entity.HasComponent<CameraComponent>());

    // Undo: remove camera
    history.Undo();
    EXPECT_FALSE(entity.HasComponent<CameraComponent>());

    // Undo: restore sprite color
    history.Undo();
    EXPECT_EQ(entity.GetComponent<SpriteRendererComponent>().Color, oldSprite.Color);

    // Undo: remove sprite
    history.Undo();
    EXPECT_FALSE(entity.HasComponent<SpriteRendererComponent>());
}

TEST(UndoRedoIntegration, CompoundCreateWithComponents)
{
    auto scene = MakeScene();
    UndoTest::CommandHistory history;
    UUID uuid{ 0 };

    auto compound = std::make_unique<UndoTest::CompoundCommand>("Create Entity with Components");

    // This simulates what the editor does when importing an animated model
    auto createCmd = std::make_unique<UndoTest::CreateEntityCommand>(scene, "AnimatedModel",
                                                                     [&](Entity e)
                                                                     {
                                                                         uuid = e.GetUUID();
                                                                         e.AddComponent<SpriteRendererComponent>();
                                                                     });

    compound->Add(std::move(createCmd));

    history.Execute(std::move(compound));
    ASSERT_NE(uuid, UUID(0));

    auto entity = scene->TryGetEntityWithUUID(uuid);
    ASSERT_TRUE(entity.has_value());
    EXPECT_TRUE(entity->HasComponent<SpriteRendererComponent>());

    // Single undo removes everything
    history.Undo();
    EXPECT_FALSE(scene->TryGetEntityWithUUID(uuid).has_value());

    // Redo brings it all back
    history.Redo();
    entity = scene->TryGetEntityWithUUID(uuid);
    ASSERT_TRUE(entity.has_value());
}

TEST(UndoRedoIntegration, PostProcessUndoDoesNotAffectScene)
{
    // Post-process undo is independent of scene state
    auto scene = MakeScene();
    Entity entity = scene->CreateEntity("Test");
    UUID uuid = entity.GetUUID();

    PostProcessSettings settings;
    PostProcessSettings old = settings;
    settings.Exposure = 5.0f;

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(
        std::make_unique<UndoTest::PostProcessChangeCommand>(&settings, old, settings));

    // Transform should be unaffected
    auto& tc = entity.GetComponent<TransformComponent>();
    tc.Translation = glm::vec3(1.0f, 2.0f, 3.0f);

    history.Undo();
    EXPECT_FLOAT_EQ(settings.Exposure, 1.0f);
    EXPECT_EQ(tc.Translation, glm::vec3(1.0f, 2.0f, 3.0f)); // unchanged
}

TEST(UndoRedoIntegration, TerrainSculptMultipleStrokes)
{
    const u32 resolution = 8;
    std::vector<f32> heightmap(resolution * resolution, 0.0f);

    UndoTest::CommandHistory history;

    // Stroke 1: raise region (0,0)-(2,2)
    std::vector<f32> old1(4, 0.0f);
    std::vector<f32> new1 = { 1.0f, 1.0f, 1.0f, 1.0f };
    history.Execute(std::make_unique<UndoTest::TerrainSculptCommand>(
        heightmap, resolution, 0, 0, 2, 2, old1, new1));

    // Stroke 2: raise region (3,3)-(2,2)
    std::vector<f32> old2(4, 0.0f);
    std::vector<f32> new2 = { 2.0f, 2.0f, 2.0f, 2.0f };
    history.Execute(std::make_unique<UndoTest::TerrainSculptCommand>(
        heightmap, resolution, 3, 3, 2, 2, old2, new2));

    EXPECT_FLOAT_EQ(heightmap[0], 1.0f);
    EXPECT_FLOAT_EQ(heightmap[3 * resolution + 3], 2.0f);

    // Undo stroke 2
    history.Undo();
    EXPECT_FLOAT_EQ(heightmap[3 * resolution + 3], 0.0f);
    EXPECT_FLOAT_EQ(heightmap[0], 1.0f); // stroke 1 still applied

    // Undo stroke 1
    history.Undo();
    EXPECT_FLOAT_EQ(heightmap[0], 0.0f);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(UndoRedoEdgeCases, OperationOnDestroyedEntity)
{
    auto scene = MakeScene();
    Entity entity = scene->CreateEntity("Ephemeral");
    UUID uuid = entity.GetUUID();

    auto& tc = entity.GetComponent<TransformComponent>();
    TransformComponent old = tc;
    tc.Translation = glm::vec3(1.0f);
    TransformComponent newState = tc;

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(
        std::make_unique<UndoTest::ComponentChangeCommand<TransformComponent>>(scene, uuid, old, newState));

    // Destroy the entity externally
    scene->DestroyEntity(entity);

    // Undo/Redo should not crash even though entity is gone
    history.Undo();
    history.Redo();
}

TEST(UndoRedoEdgeCases, MultipleUndoRedoCycles)
{
    UndoTest::CommandHistory history;
    int value = 0;

    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 1));

    // Rapidly cycle undo/redo
    for (int i = 0; i < 100; ++i)
    {
        history.Undo();
        EXPECT_EQ(value, 0);
        history.Redo();
        EXPECT_EQ(value, 1);
    }
}

// =============================================================================
// Streaming settings undo/redo
// =============================================================================

TEST(StreamingSettingsCommand, ChangesAndRestoresSettings)
{
    StreamingSettings settings;
    settings.Enabled = false;
    settings.DefaultLoadRadius = 200.0f;
    settings.DefaultUnloadRadius = 250.0f;
    settings.MaxLoadedRegions = 16;
    settings.RegionDirectory = "regions/default";

    StreamingSettings oldSettings = settings;

    StreamingSettings newSettings;
    newSettings.Enabled = true;
    newSettings.DefaultLoadRadius = 400.0f;
    newSettings.DefaultUnloadRadius = 500.0f;
    newSettings.MaxLoadedRegions = 32;
    newSettings.RegionDirectory = "regions/custom";

    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::StreamingSettingsChangeCommand>(
        settings, oldSettings, newSettings));

    EXPECT_TRUE(settings.Enabled);
    EXPECT_FLOAT_EQ(settings.DefaultLoadRadius, 400.0f);
    EXPECT_FLOAT_EQ(settings.DefaultUnloadRadius, 500.0f);
    EXPECT_EQ(settings.MaxLoadedRegions, 32u);
    EXPECT_EQ(settings.RegionDirectory, "regions/custom");

    history.Undo();

    EXPECT_FALSE(settings.Enabled);
    EXPECT_FLOAT_EQ(settings.DefaultLoadRadius, 200.0f);
    EXPECT_FLOAT_EQ(settings.DefaultUnloadRadius, 250.0f);
    EXPECT_EQ(settings.MaxLoadedRegions, 16u);
    EXPECT_EQ(settings.RegionDirectory, "regions/default");

    history.Redo();

    EXPECT_TRUE(settings.Enabled);
    EXPECT_EQ(settings.RegionDirectory, "regions/custom");
}

// =============================================================================
// Dialogue editor undo/redo
// =============================================================================

TEST(DialogueEditorCommand, SnapshotRestoreRoundTrip)
{
    // Simulated panel state
    std::vector<DialogueNodeData> nodes;
    std::vector<DialogueConnection> connections;
    UUID rootNodeID = 0;

    auto applyFn = [&](const UndoTest::DialogueEditorSnapshot& snapshot)
    {
        nodes = snapshot.Nodes;
        connections = snapshot.Connections;
        rootNodeID = snapshot.RootNodeID;
    };

    // Capture "old" (empty state)
    UndoTest::DialogueEditorSnapshot oldState{ nodes, connections, rootNodeID };

    // Add a node
    DialogueNodeData node1;
    node1.ID = UUID();
    node1.Type = "dialogue";
    node1.Name = "Greeting";
    nodes.push_back(node1);
    rootNodeID = node1.ID;

    UndoTest::DialogueEditorSnapshot newState{ nodes, connections, rootNodeID };

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(std::make_unique<UndoTest::DialogueEditorChangeCommand>(
        oldState, newState, applyFn, "Create Node"));

    EXPECT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].Name, "Greeting");
    EXPECT_EQ(rootNodeID, node1.ID);

    history.Undo();

    EXPECT_TRUE(nodes.empty());
    EXPECT_EQ(rootNodeID, static_cast<u64>(0));

    history.Redo();

    EXPECT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].Name, "Greeting");
}

TEST(DialogueEditorCommand, ConnectionUndoRedo)
{
    std::vector<DialogueNodeData> nodes;
    std::vector<DialogueConnection> connections;
    UUID rootNodeID = 0;

    auto applyFn = [&](const UndoTest::DialogueEditorSnapshot& snapshot)
    {
        nodes = snapshot.Nodes;
        connections = snapshot.Connections;
        rootNodeID = snapshot.RootNodeID;
    };

    // Setup two nodes
    DialogueNodeData n1;
    n1.ID = UUID();
    n1.Name = "Start";
    DialogueNodeData n2;
    n2.ID = UUID();
    n2.Name = "End";
    nodes = { n1, n2 };

    UndoTest::DialogueEditorSnapshot oldState{ nodes, connections, rootNodeID };

    // Add a connection
    DialogueConnection conn;
    conn.SourceNodeID = n1.ID;
    conn.TargetNodeID = n2.ID;
    conn.SourcePort = "output";
    conn.TargetPort = "input";
    connections.push_back(conn);

    UndoTest::DialogueEditorSnapshot newState{ nodes, connections, rootNodeID };

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(std::make_unique<UndoTest::DialogueEditorChangeCommand>(
        oldState, newState, applyFn, "Create Connection"));

    EXPECT_EQ(connections.size(), 1u);
    EXPECT_EQ(connections[0].SourceNodeID, n1.ID);

    history.Undo();

    EXPECT_TRUE(connections.empty());
    EXPECT_EQ(nodes.size(), 2u); // Nodes preserved

    history.Redo();

    EXPECT_EQ(connections.size(), 1u);
}

// =============================================================================
// Input action map undo/redo
// =============================================================================

TEST(InputActionMapCommand, AddAndRemoveAction)
{
    InputActionMap map;
    map.Name = "TestMap";

    InputActionMap oldMap = map;

    InputAction jumpAction;
    jumpAction.Name = "Jump";
    jumpAction.Bindings.push_back(InputBinding::Key(Key::Space));
    map.AddAction(std::move(jumpAction));

    InputActionMap newMap = map;

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(std::make_unique<UndoTest::InputActionMapChangeCommand>(
        map, oldMap, newMap, "Add Action"));

    EXPECT_TRUE(map.HasAction("Jump"));
    EXPECT_EQ(map.Actions.size(), 1u);

    history.Undo();

    EXPECT_FALSE(map.HasAction("Jump"));
    EXPECT_TRUE(map.Actions.empty());

    history.Redo();

    EXPECT_TRUE(map.HasAction("Jump"));
    auto* action = map.GetAction("Jump");
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->Bindings.size(), 1u);
    EXPECT_EQ(action->Bindings[0].Code, Key::Space);
}

TEST(InputActionMapCommand, RebindWithConflictResolution)
{
    InputActionMap map;
    map.Name = "GameMap";

    InputAction moveAction;
    moveAction.Name = "Move";
    moveAction.Bindings.push_back(InputBinding::Key(Key::W));
    map.AddAction(std::move(moveAction));

    InputAction fireAction;
    fireAction.Name = "Fire";
    fireAction.Bindings.push_back(InputBinding::Key(Key::F));
    map.AddAction(std::move(fireAction));

    InputActionMap oldMap = map;

    // Simulate rebinding "Fire" to Key::W (conflict with "Move")
    // Conflict resolution removes W from Move, adds to Fire
    auto* move = map.GetAction("Move");
    auto it = std::ranges::find(move->Bindings, InputBinding::Key(Key::W));
    if (it != move->Bindings.end())
    {
        move->Bindings.erase(it);
    }
    auto* fire = map.GetAction("Fire");
    fire->Bindings[0] = InputBinding::Key(Key::W);

    InputActionMap newMap = map;

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(std::make_unique<UndoTest::InputActionMapChangeCommand>(
        map, oldMap, newMap, "Rebind"));

    // After rebind: Fire has W, Move has no bindings
    EXPECT_EQ(map.GetAction("Fire")->Bindings[0].Code, Key::W);
    EXPECT_TRUE(map.GetAction("Move")->Bindings.empty());

    // Undo: restores original bindings
    history.Undo();

    EXPECT_EQ(map.GetAction("Move")->Bindings.size(), 1u);
    EXPECT_EQ(map.GetAction("Move")->Bindings[0].Code, Key::W);
    EXPECT_EQ(map.GetAction("Fire")->Bindings[0].Code, Key::F);

    // Redo
    history.Redo();

    EXPECT_EQ(map.GetAction("Fire")->Bindings[0].Code, Key::W);
    EXPECT_TRUE(map.GetAction("Move")->Bindings.empty());
}

TEST(InputActionMapCommand, ResetToEmpty)
{
    InputActionMap map;
    map.Name = "FullMap";

    InputAction action1;
    action1.Name = "Jump";
    action1.Bindings.push_back(InputBinding::Key(Key::Space));
    map.AddAction(std::move(action1));

    InputAction action2;
    action2.Name = "Crouch";
    action2.Bindings.push_back(InputBinding::Key(Key::LeftControl));
    map.AddAction(std::move(action2));

    InputActionMap oldMap = map;
    map = {};
    InputActionMap emptyMap = map;

    UndoTest::CommandHistory history;
    history.PushAlreadyExecuted(std::make_unique<UndoTest::InputActionMapChangeCommand>(
        map, oldMap, emptyMap, "Reset Input Map"));

    EXPECT_TRUE(map.Actions.empty());
    EXPECT_TRUE(map.Name.empty());

    history.Undo();

    EXPECT_EQ(map.Name, "FullMap");
    EXPECT_EQ(map.Actions.size(), 2u);
    EXPECT_TRUE(map.HasAction("Jump"));
    EXPECT_TRUE(map.HasAction("Crouch"));

    history.Redo();

    EXPECT_TRUE(map.Actions.empty());
}

// =============================================================================
// Save-Point / Dirty Tracking Tests
// =============================================================================

TEST(UndoRedoTest, SavePoint_CleanAfterClear)
{
    UndoTest::CommandHistory history;
    EXPECT_FALSE(history.IsDirty());
}

TEST(UndoRedoTest, SavePoint_DirtyAfterExecute)
{
    int value = 0;
    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value));
    EXPECT_TRUE(history.IsDirty());
}

TEST(UndoRedoTest, SavePoint_CleanAfterSave)
{
    int value = 0;
    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value));
    EXPECT_TRUE(history.IsDirty());
    history.MarkSaved();
    EXPECT_FALSE(history.IsDirty());
}

TEST(UndoRedoTest, SavePoint_DirtyAfterUndoPastSave)
{
    int value = 0;
    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value));
    history.MarkSaved();
    history.Undo();
    EXPECT_TRUE(history.IsDirty());
}

TEST(UndoRedoTest, SavePoint_CleanAfterRedoBackToSave)
{
    int value = 0;
    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value));
    history.MarkSaved();
    history.Undo();
    EXPECT_TRUE(history.IsDirty());
    history.Redo();
    EXPECT_FALSE(history.IsDirty());
}

TEST(UndoRedoTest, SavePoint_DirtyAfterBranch)
{
    int value = 0;
    UndoTest::CommandHistory history;
    // Push A, B, C and save
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 1));
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 2));
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 3));
    history.MarkSaved();
    EXPECT_FALSE(history.IsDirty());

    // Undo once then push a new command (branch)
    history.Undo();
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 10));
    // Even though version count matches, save point is invalidated by branching
    EXPECT_TRUE(history.IsDirty());
}

TEST(UndoRedoTest, SavePoint_CleanAfterClearFollowedByMark)
{
    int value = 0;
    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value));
    history.MarkSaved();
    history.Clear();
    // After Clear, state is clean (fresh scene)
    EXPECT_FALSE(history.IsDirty());
}

TEST(UndoRedoTest, SavePoint_DirtyAfterPushAlreadyExecuted)
{
    int value = 42;
    UndoTest::CommandHistory history;
    history.MarkSaved();
    EXPECT_FALSE(history.IsDirty());
    history.PushAlreadyExecuted(std::make_unique<UndoTest::IncrementCommand>(value));
    EXPECT_TRUE(history.IsDirty());
}

TEST(UndoRedoTest, SavePoint_MultipleUndoRedoCycles)
{
    int value = 0;
    UndoTest::CommandHistory history;
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 1));
    history.Execute(std::make_unique<UndoTest::IncrementCommand>(value, 2));
    history.MarkSaved(); // saved at version 2

    // Undo to version 1 → dirty
    history.Undo();
    EXPECT_TRUE(history.IsDirty());

    // Undo to version 0 → dirty
    history.Undo();
    EXPECT_TRUE(history.IsDirty());

    // Redo to version 1 → still dirty
    history.Redo();
    EXPECT_TRUE(history.IsDirty());

    // Redo to version 2 → clean
    history.Redo();
    EXPECT_FALSE(history.IsDirty());
}
