#include "OloEnginePCH.h"
#include "OloEngine/Localization/StringTable.h"
#include "OloEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>

namespace OloEngine
{
    namespace
    {
        // Empty sentinel returned by Get() when a key is missing. Returning a
        // const-ref to this static is safe across threads — it is read-only.
        const std::string& EmptyString()
        {
            static const std::string s_Empty;
            return s_Empty;
        }

        TextDirection ParseDirection(const std::string& s)
        {
            if (s == "rtl" || s == "RTL")
                return TextDirection::RTL;
            return TextDirection::LTR;
        }

        PluralRule ParsePluralRule(const std::string& s)
        {
            if (s == "other-only" || s == "OtherOnly" || s == "none")
                return PluralRule::OtherOnly;
            return PluralRule::OneOther;
        }

        bool ParseInto(const YAML::Node& root, LocaleDefinition& outLocale, std::unordered_map<std::string, std::string>& outStrings)
        {
            if (!root.IsMap())
            {
                OLO_CORE_ERROR("StringTable: YAML root is not a map");
                return false;
            }

            const YAML::Node localeNode = root["locale"];
            if (!localeNode || !localeNode.IsScalar())
            {
                OLO_CORE_ERROR("StringTable: missing required 'locale' scalar");
                return false;
            }
            outLocale.Code = localeNode.as<std::string>();

            if (const YAML::Node nameNode = root["name"]; nameNode && nameNode.IsScalar())
                outLocale.Name = nameNode.as<std::string>();
            else
                outLocale.Name = outLocale.Code;

            if (const YAML::Node dirNode = root["direction"]; dirNode && dirNode.IsScalar())
                outLocale.Direction = ParseDirection(dirNode.as<std::string>());

            if (const YAML::Node pluralNode = root["plural_rule"]; pluralNode && pluralNode.IsScalar())
                outLocale.Plural = ParsePluralRule(pluralNode.as<std::string>());

            const YAML::Node stringsNode = root["strings"];
            if (!stringsNode || !stringsNode.IsMap())
            {
                OLO_CORE_ERROR("StringTable: missing required 'strings' map (locale '{}')", outLocale.Code);
                return false;
            }

            outStrings.reserve(stringsNode.size());
            for (const auto& kv : stringsNode)
            {
                if (!kv.first.IsScalar() || !kv.second.IsScalar())
                    continue;
                outStrings.emplace(kv.first.as<std::string>(), kv.second.as<std::string>());
            }
            return true;
        }
    } // namespace

    bool StringTable::LoadFromYAML(const std::string& filePath)
    {
        std::ifstream in(filePath);
        if (!in.is_open())
        {
            OLO_CORE_ERROR("StringTable::LoadFromYAML: cannot open '{}'", filePath);
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        return LoadFromYAMLString(ss.str());
    }

    bool StringTable::LoadFromYAMLString(const std::string& yamlContent)
    {
        try
        {
            const YAML::Node root = YAML::Load(yamlContent);
            LocaleDefinition newLocale{};
            std::unordered_map<std::string, std::string> newStrings;
            if (!ParseInto(root, newLocale, newStrings))
                return false;
            m_Locale = std::move(newLocale);
            m_Strings = std::move(newStrings);
            return true;
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("StringTable: YAML parse failure: {}", e.what());
            return false;
        }
    }

    const std::string& StringTable::Get(const std::string& key) const
    {
        const auto it = m_Strings.find(key);
        if (it == m_Strings.end())
            return EmptyString();
        return it->second;
    }

    bool StringTable::Has(const std::string& key) const
    {
        return m_Strings.contains(key);
    }

    std::vector<std::string> StringTable::GetAllKeys() const
    {
        std::vector<std::string> keys;
        keys.reserve(m_Strings.size());
        for (const auto& kv : m_Strings)
            keys.push_back(kv.first);
        return keys;
    }

    void StringTable::Set(const std::string& key, const std::string& value)
    {
        m_Strings[key] = value;
    }

    void StringTable::Clear()
    {
        m_Strings.clear();
    }
} // namespace OloEngine
