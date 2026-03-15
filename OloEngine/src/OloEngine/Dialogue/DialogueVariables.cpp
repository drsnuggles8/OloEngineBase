#include "OloEnginePCH.h"
#include "OloEngine/Dialogue/DialogueVariables.h"

namespace OloEngine
{
    bool DialogueVariables::GetBool(const std::string& key, bool defaultValue) const
    {
        auto it = m_Variables.find(key);
        if (it == m_Variables.end())
            return defaultValue;
        if (const auto* val = std::get_if<bool>(&it->second))
            return *val;
        return defaultValue;
    }

    i32 DialogueVariables::GetInt(const std::string& key, i32 defaultValue) const
    {
        auto it = m_Variables.find(key);
        if (it == m_Variables.end())
            return defaultValue;
        if (const auto* val = std::get_if<i32>(&it->second))
            return *val;
        return defaultValue;
    }

    std::string DialogueVariables::GetString(const std::string& key, const std::string& defaultValue) const
    {
        auto it = m_Variables.find(key);
        if (it == m_Variables.end())
            return defaultValue;
        if (const auto* val = std::get_if<std::string>(&it->second))
            return *val;
        return defaultValue;
    }

    void DialogueVariables::SetBool(const std::string& key, bool value)
    {
        m_Variables[key] = value;
    }

    void DialogueVariables::SetInt(const std::string& key, i32 value)
    {
        m_Variables[key] = value;
    }

    void DialogueVariables::SetString(const std::string& key, const std::string& value)
    {
        m_Variables[key] = value;
    }

    bool DialogueVariables::Has(const std::string& key) const
    {
        return m_Variables.contains(key);
    }

    void DialogueVariables::Clear()
    {
        m_Variables.clear();
    }

} // namespace OloEngine
