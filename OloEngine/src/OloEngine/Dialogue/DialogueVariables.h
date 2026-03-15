#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <unordered_map>
#include <variant>

namespace OloEngine
{
    using DialogueVariableValue = std::variant<bool, i32, std::string>;

    class DialogueVariables
    {
    public:
        bool GetBool(const std::string& key, bool defaultValue = false) const;
        i32 GetInt(const std::string& key, i32 defaultValue = 0) const;
        std::string GetString(const std::string& key, const std::string& defaultValue = "") const;

        void SetBool(const std::string& key, bool value);
        void SetInt(const std::string& key, i32 value);
        void SetString(const std::string& key, const std::string& value);

        bool Has(const std::string& key) const;
        void Clear();

    private:
        std::unordered_map<std::string, DialogueVariableValue> m_Variables;
    };

} // namespace OloEngine
