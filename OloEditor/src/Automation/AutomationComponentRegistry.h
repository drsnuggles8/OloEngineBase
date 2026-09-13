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
        // Empty when this component may be COPIED to a second entity. Stricter
        // than ValidateSnapshot: a snapshot hands the state back to the entity it
        // came from, whereas a copy leaves two entities sharing whatever the
        // component holds, and three components own resources that cannot have
        // two owners. Asked per component so a caller copying ONE component
        // (a prefab apply) is not refused by an unrelated one, and so
        // ValidateEntityDuplication has a single place to read it from.
        std::function<std::string(Entity)> ValidateCopy;
        std::function<std::shared_ptr<const ComponentSnapshot>(Entity)> Capture;
        // Remove the component if present, with no undo bookkeeping of its own.
        // Set for every AUTHORED type -- including the ones MakeRemoveCommand
        // refuses -- because restoring a captured entity STATE has to be able to
        // take away a component the snapshot does not carry, and "which
        // components may a caller remove on purpose" is a different question
        // from "which components does a memento have to be able to clear".
        // Null for a non-authored type, which no memento captures either.
        std::function<void(Entity)> Remove;
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
    // The whole-entity form of ComponentTypeEntry::ValidateCopy.
    [[nodiscard]] std::string ValidateEntityDuplication(Entity entity);
    // Throws before capture if ValidateEntitySnapshot refuses the entity. Includes
    // Transform/Relationship; callers handle ID and Tag themselves.
    [[nodiscard]] ComponentSnapshots CaptureEntityComponents(Entity entity);

    void RegisterComponentAuthoringCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
