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

    // UUID-based undo/redo for a SINGLE property write applied through a setter
    // function DIRECTLY on the live component — never a whole-object copy/assign
    // (contrast ComponentChangeCommand<T> above). Needed for a component whose
    // operator= cannot be trusted to preserve runtime-only state or to push a value
    // into a live subsystem handle: AudioSourceComponent's operator= resets
    // ActiveEventID to 0 and never re-invokes Source->SetVolume()/SetPitch()/etc, so
    // swapping in a whole new AudioSourceComponent would silently detach a playing
    // sound and leave its actual playback parameters unchanged even though the
    // stored config looked right. `setFn` is generated from the SAME OLO_PROPERTY
    // Get/Set expression pair OLO_PROPERTY already compiles for Lua/C# scripting
    // (see OloHeaderTool's EmitMcpSetterFields and McpGenericFieldWrite.h's
    // MakeSetterField, issue #607's AudioSourceComponent slice) — Execute applies
    // the new value, Undo re-applies the captured old value, both through the exact
    // same setter so any side effect (e.g. Source->SetVolume) fires identically
    // either direction.
    template<typename T, typename F, typename SetFn>
    class PropertySetCommand : public EditorCommand
    {
      public:
        PropertySetCommand(Ref<Scene> scene, UUID entityUUID, SetFn setFn, F oldValue, F newValue, std::string description = "Property Change")
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID), m_SetFn(std::move(setFn)), m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue)), m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            Apply(m_NewValue);
        }

        void Undo() override
        {
            Apply(m_OldValue);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        void Apply(const F& value)
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && entityOpt->HasComponent<T>())
            {
                m_SetFn(entityOpt->GetComponent<T>(), value);
            }
        }

        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        SetFn m_SetFn;
        F m_OldValue;
        F m_NewValue;
        std::string m_Description;
    };

    // UUID-based undo/redo for a SINGLE MAP-KEY write applied through a setter
    // function DIRECTLY on the live component (issue #607's MorphTargetComponent::
    // Weights map-key addressing slice) — the map-typed sibling of
    // PropertySetCommand above. A plain PropertySetCommand's Undo just re-applies
    // `setFn` with the captured old value, which is wrong for a map key that did
    // NOT exist before the write: `setFn` (e.g. MorphTargetComponent::SetWeight)
    // uses `operator[]`, so re-applying it on Undo would leave a phantom entry at
    // the map's semantic default (e.g. weight 0.0) instead of restoring "the key
    // was never there" — a caller enumerating current keys (olo_entity_list_fields)
    // would then see a key that never existed before the original write. Undo
    // therefore branches on `keyExistedBefore`: restore the captured old value when
    // the key was already present, or erase the key entirely when it was not.
    template<typename T, typename F, typename SetFn, typename EraseFn>
    class MapKeyPropertySetCommand : public EditorCommand
    {
      public:
        MapKeyPropertySetCommand(Ref<Scene> scene, UUID entityUUID, SetFn setFn, EraseFn eraseFn,
                                 F oldValue, F newValue, bool keyExistedBefore, std::string description = "Property Change")
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID), m_SetFn(std::move(setFn)), m_EraseFn(std::move(eraseFn)),
              m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue)), m_KeyExistedBefore(keyExistedBefore),
              m_Description(std::move(description))
        {
        }

        void Execute() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && entityOpt->HasComponent<T>())
            {
                m_SetFn(entityOpt->GetComponent<T>(), m_NewValue);
            }
        }

        void Undo() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt && entityOpt->HasComponent<T>())
            {
                if (m_KeyExistedBefore)
                {
                    m_SetFn(entityOpt->GetComponent<T>(), m_OldValue);
                }
                else
                {
                    m_EraseFn(entityOpt->GetComponent<T>());
                }
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Description;
        }

      private:
        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        SetFn m_SetFn;
        EraseFn m_EraseFn;
        F m_OldValue;
        F m_NewValue;
        bool m_KeyExistedBefore;
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
                if (m_HasSnapshot)
                {
                    // Redo: restore from snapshot instead of re-running init
                    comp = m_Snapshot;
                }
                else
                {
                    if (m_InitFn)
                    {
                        m_InitFn(comp);
                    }
                    // Snapshot after init for redo consistency
                    m_Snapshot = entityOpt->GetComponent<T>();
                    m_HasSnapshot = true;
                }
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
