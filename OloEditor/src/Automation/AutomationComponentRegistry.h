#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class EditorCommand;
    class Entity;
    class Scene;
} // namespace OloEngine

namespace OloEngine::Automation
{
    class AutomationRegistry;

    // Authored component state, independent of inspector equality support. Entity
    // commands keep these beside their UUID/tag/hierarchy bookkeeping.
    class ComponentSnapshot
    {
      public:
        virtual ~ComponentSnapshot() = default;
        virtual void Restore(Entity entity) const = 0;
    };

    using ComponentSnapshots = std::vector<std::shared_ptr<const ComponentSnapshot>>;

    struct ComponentTypeEntry
    {
        std::string Name;
        bool Authored = false;
        bool Addable = false;
        bool Removable = false;
        std::string RejectionReason;
        std::function<bool(Entity)> Has;
        std::function<std::string(Entity)> ValidateSnapshot;
        std::function<std::shared_ptr<const ComponentSnapshot>(Entity)> Capture;
        std::function<std::unique_ptr<EditorCommand>(Ref<Scene>, UUID)> MakeAddCommand;
        std::function<std::unique_ptr<EditorCommand>(Ref<Scene>, UUID)> MakeRemoveCommand;
    };

    // Generated type coverage, materialized once without constructing components.
    [[nodiscard]] const std::vector<ComponentTypeEntry>& ComponentTypes();
    [[nodiscard]] const ComponentTypeEntry* FindComponentType(std::string_view name);

    // Empty means every authored component can be restored. Derived transform/UI
    // caches are recomputed; other uncaptured runtime state is refused explicitly.
    [[nodiscard]] std::string ValidateEntitySnapshot(Entity entity);
    // Duplication must not give two entities ownership of the same mutable terrain
    // or generated assets. Undo restoration retains them for their original UUID.
    [[nodiscard]] std::string ValidateEntityDuplication(Entity entity);
    // Throws before capture if ValidateEntitySnapshot refuses the entity. Includes
    // Transform/Relationship; callers handle ID and Tag themselves.
    [[nodiscard]] ComponentSnapshots CaptureEntityComponents(Entity entity);

    void RegisterComponentAuthoringCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
