#pragma once

#include <string>

namespace OloEngine
{
    // Tag-style component that marks a UI text-bearing entity as auto-
    // localized. A system (not yet wired up — see Issue #175 follow-up) will
    // look up `LocalizationKey` via LocalizationManager and write the result
    // into the entity's TextComponent every time the active locale changes.
    //
    // Kept deliberately POD — equality + memcmp-based undo (see
    // SceneHierarchyPanel::DrawComponent's three-tier constexpr-if) work out
    // of the box because std::string makes the type non-trivially-copyable
    // but equality-comparable.
    struct LocalizedTextComponent
    {
        std::string LocalizationKey;

        LocalizedTextComponent() = default;
        explicit LocalizedTextComponent(std::string key) : LocalizationKey(std::move(key)) {}

        auto operator==(const LocalizedTextComponent&) const -> bool = default;
    };
} // namespace OloEngine
