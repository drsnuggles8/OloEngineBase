#pragma once

#include "EditorCommand.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <functional>
#include <utility>

namespace OloEngine
{
    // Undo/Redo for adding a component
    template<typename T>
    class AddComponentCommand : public EditorCommand
    {
      public:
        AddComponentCommand(Ref<Scene> scene, UUID entityUUID)
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID)
        {
        }

        void Execute() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && !entityOpt->HasComponent<T>())
            {
                entityOpt->AddComponent<T>();
            }
        }

        void Undo() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && entityOpt->HasComponent<T>())
            {
                entityOpt->RemoveComponent<T>();
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Add Component";
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
    };

    // Undo/Redo for removing a component — snapshots component data for restore
    template<typename T>
    class RemoveComponentCommand : public EditorCommand
    {
      public:
        RemoveComponentCommand(Ref<Scene> scene, UUID entityUUID, const T& snapshotData)
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID), m_SnapshotData(snapshotData)
        {
        }

        void Execute() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && entityOpt->HasComponent<T>())
            {
                entityOpt->RemoveComponent<T>();
            }
        }

        void Undo() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && !entityOpt->HasComponent<T>())
            {
                entityOpt->AddOrReplaceComponent<T>(m_SnapshotData);
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Remove Component";
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        T m_SnapshotData;
    };

    // Generic command for any property change — stores old and new values
    template<typename T>
    class ChangePropertyCommand : public EditorCommand
    {
      public:
        ChangePropertyCommand(T* target, T oldValue, T newValue, std::string description = "Property Change")
            : m_Target(target), m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            if (m_Target)
            {
                *m_Target = m_NewValue;
            }
        }

        void Undo() override
        {
            if (m_Target)
            {
                *m_Target = m_OldValue;
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        T* m_Target;
        T m_OldValue;
        T m_NewValue;
        std::string m_Description;
    };

    // UUID-based undo/redo for whole-component property changes (safe against EnTT relocation)
    template<typename T>
    class ComponentChangeCommand : public EditorCommand
    {
      public:
        ComponentChangeCommand(Ref<Scene> scene, UUID entityUUID, T oldData, T newData, std::string description = "Property Change")
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID), m_OldData(std::move(oldData)), m_NewData(std::move(newData)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && entityOpt->HasComponent<T>())
            {
                entityOpt->GetComponent<T>() = m_NewData;
            }
        }

        void Undo() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && entityOpt->HasComponent<T>())
            {
                entityOpt->GetComponent<T>() = m_OldData;
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        T m_OldData;
        T m_NewData;
        std::string m_Description;
    };

    // Undo/Redo for adding a component with a post-init callback (e.g. particle presets)
    template<typename T>
    class AddComponentWithInitCommand : public EditorCommand
    {
      public:
        AddComponentWithInitCommand(Ref<Scene> scene, UUID entityUUID,
                                    std::function<void(T&)> initFn,
                                    std::string description = "Add Component")
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID), m_InitFn(std::move(initFn)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && !entityOpt->HasComponent<T>())
            {
                auto& comp = entityOpt->AddComponent<T>();
                if (m_InitFn)
                {
                    m_InitFn(comp);
                }
                // Snapshot after init for redo consistency
                m_Snapshot = entityOpt->GetComponent<T>();
                m_HasSnapshot = true;
            }
        }

        void Undo() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && entityOpt->HasComponent<T>())
            {
                entityOpt->RemoveComponent<T>();
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        std::function<void(T&)> m_InitFn;
        std::string m_Description;
        T m_Snapshot{};
        bool m_HasSnapshot = false;
    };

} // namespace OloEngine
