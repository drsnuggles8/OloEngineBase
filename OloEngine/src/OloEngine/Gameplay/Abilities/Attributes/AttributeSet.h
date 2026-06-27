#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/TransparentStringHash.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeModifier.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{

    class AttributeSet
    {
      public:
        AttributeSet() = default;

        void DefineAttribute(const std::string& name, f32 baseValue);

        [[nodiscard("attribute base value must be used")]] f32 GetBaseValue(const std::string& name) const;
        [[nodiscard("attribute current value must be used")]] f32 GetCurrentValue(const std::string& name) const;
        [[nodiscard("existence check must be used")]] bool HasAttribute(const std::string& name) const;
        [[nodiscard("attribute name list must be used")]] std::vector<std::string> GetAttributeNames() const;

        void SetBaseValue(const std::string& name, f32 value);
        void AddModifier(const std::string& attribute, const AttributeModifier& modifier);
        void RemoveModifiersBySource(const std::string& attribute, const GameplayTag& source);
        void RemoveAllModifiers(const std::string& attribute);
        void ClearAllModifiers();

        [[nodiscard("modifier list must be used")]] const std::vector<AttributeModifier>& GetModifiers(const std::string& attribute) const;

        // For deserialization — restores base values + raw modifier list bypassing
        // ApplyEffect / removal bookkeeping. Caller is responsible for ensuring the
        // modifier source tags are still valid in the surrounding ability system.
        void RestoreFromSnapshot(const std::string& attribute, f32 baseValue,
                                 const std::vector<AttributeModifier>& modifiers);

        auto operator==(const AttributeSet& other) const -> bool;

      private:
        struct Attribute
        {
            f32 BaseValue = 0.0f;
            std::vector<AttributeModifier> Modifiers;
            mutable f32 CachedCurrentValue = 0.0f;
            mutable bool Dirty = true;
        };

        void RecalculateAttribute(const Attribute& attr) const;

        std::unordered_map<std::string, Attribute, StringHash, StringEqual> m_Attributes;
    };

} // namespace OloEngine
