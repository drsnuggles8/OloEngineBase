#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"

namespace OloEngine
{

    void AttributeSet::DefineAttribute(const std::string& name, f32 baseValue)
    {
        auto& attr = m_Attributes[name];
        attr.BaseValue = baseValue;
        attr.Dirty = true;
    }

    f32 AttributeSet::GetBaseValue(const std::string& name) const
    {
        auto it = m_Attributes.find(name);
        if (it == m_Attributes.end())
        {
            return 0.0f;
        }
        return it->second.BaseValue;
    }

    f32 AttributeSet::GetCurrentValue(const std::string& name) const
    {
        auto it = m_Attributes.find(name);
        if (it == m_Attributes.end())
        {
            return 0.0f;
        }
        auto const& attr = it->second;
        if (attr.Dirty)
        {
            RecalculateAttribute(attr);
        }
        return attr.CachedCurrentValue;
    }

    bool AttributeSet::HasAttribute(const std::string& name) const
    {
        return m_Attributes.contains(name);
    }

    std::vector<std::string> AttributeSet::GetAttributeNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_Attributes.size());
        for (auto const& [name, _] : m_Attributes)
        {
            names.push_back(name);
        }
        return names;
    }

    void AttributeSet::SetBaseValue(const std::string& name, f32 value)
    {
        auto it = m_Attributes.find(name);
        if (it != m_Attributes.end())
        {
            it->second.BaseValue = value;
            it->second.Dirty = true;
        }
    }

    void AttributeSet::AddModifier(const std::string& attribute, const AttributeModifier& modifier)
    {
        auto it = m_Attributes.find(attribute);
        if (it != m_Attributes.end())
        {
            it->second.Modifiers.push_back(modifier);
            it->second.Dirty = true;
        }
    }

    void AttributeSet::RemoveModifiersBySource(const std::string& attribute, const GameplayTag& source)
    {
        auto it = m_Attributes.find(attribute);
        if (it != m_Attributes.end())
        {
            std::erase_if(it->second.Modifiers, [&source](const AttributeModifier& m)
                          { return m.Source.MatchesExact(source); });
            it->second.Dirty = true;
        }
    }

    void AttributeSet::RemoveAllModifiers(const std::string& attribute)
    {
        auto it = m_Attributes.find(attribute);
        if (it != m_Attributes.end())
        {
            it->second.Modifiers.clear();
            it->second.Dirty = true;
        }
    }

    void AttributeSet::ClearAllModifiers()
    {
        for (auto& [_, attr] : m_Attributes)
        {
            attr.Modifiers.clear();
            attr.Dirty = true;
        }
    }

    const std::vector<AttributeModifier>& AttributeSet::GetModifiers(const std::string& attribute) const
    {
        auto it = m_Attributes.find(attribute);
        if (it != m_Attributes.end())
        {
            return it->second.Modifiers;
        }
        static const std::vector<AttributeModifier> empty;
        return empty;
    }

    auto AttributeSet::operator==(const AttributeSet& other) const -> bool
    {
        if (m_Attributes.size() != other.m_Attributes.size())
        {
            return false;
        }
        for (auto const& [name, attr] : m_Attributes)
        {
            auto it = other.m_Attributes.find(name);
            if (it == other.m_Attributes.end())
            {
                return false;
            }
            if (attr.BaseValue != it->second.BaseValue)
            {
                return false;
            }
            if (attr.Modifiers != it->second.Modifiers)
            {
                return false;
            }
        }
        return true;
    }

    void AttributeSet::RecalculateAttribute(const Attribute& attr) const
    {
        f32 value = attr.BaseValue;
        f32 additive = 0.0f;
        f32 multiplicative = 1.0f;
        bool hasOverride = false;
        f32 overrideValue = 0.0f;

        for (auto const& mod : attr.Modifiers)
        {
            switch (mod.Op)
            {
            case AttributeModifier::Operation::Add:
                additive += mod.Magnitude;
                break;
            case AttributeModifier::Operation::Multiply:
                multiplicative *= mod.Magnitude;
                break;
            case AttributeModifier::Operation::Override:
                hasOverride = true;
                overrideValue = mod.Magnitude;
                break;
            }
        }

        if (hasOverride)
        {
            attr.CachedCurrentValue = overrideValue;
        }
        else
        {
            attr.CachedCurrentValue = (value + additive) * multiplicative;
        }

        attr.Dirty = false;
    }

} // namespace OloEngine
