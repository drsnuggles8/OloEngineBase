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
            if (s == "french" || s == "FrenchLike" || s == "one-other-zero")
                return PluralRule::FrenchLike;
            if (s == "polish" || s == "PolishLike")
                return PluralRule::PolishLike;
            if (s == "russian" || s == "RussianLike" || s == "slavic")
                return PluralRule::RussianLike;
            if (s == "arabic" || s == "ArabicLike")
                return PluralRule::ArabicLike;
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

            if (const YAML::Node ts = root["thousand_separator"]; ts && ts.IsScalar())
                outLocale.ThousandSeparator = ts.as<std::string>();
            if (const YAML::Node ds = root["decimal_separator"]; ds && ds.IsScalar())
                outLocale.DecimalSeparator = ds.as<std::string>();

            if (const YAML::Node fp = root["font"]; fp && fp.IsScalar())
                outLocale.FontPath = fp.as<std::string>();
            if (const YAML::Node fb = root["font_fallbacks"]; fb && fb.IsSequence())
            {
                outLocale.FontFallbacks.clear();
                outLocale.FontFallbacks.reserve(fb.size());
                for (const auto& entry : fb)
                {
                    if (entry.IsScalar())
                        outLocale.FontFallbacks.push_back(entry.as<std::string>());
                }
            }

            const YAML::Node stringsNode = root["strings"];
            if (!stringsNode || !stringsNode.IsMap())
            {
                OLO_CORE_ERROR("StringTable: missing required 'strings' map (locale '{}')", outLocale.Code);
                return false;
            }

            outStrings.reserve(stringsNode.size());
            // Strings can be authored two ways:
            //   ui.button.play: "Play"
            // or, with translator metadata:
            //   ui.button.play:
            //     value: "Play"
            //     context: "Main menu primary CTA"
            //     max_length: 16
            // The richer form is opt-in per key — un-extended keys still
            // round-trip cleanly through the simple shape.
            for (const auto& kv : stringsNode)
            {
                if (!kv.first.IsScalar())
                    continue;
                const auto key = kv.first.as<std::string>();
                if (kv.second.IsScalar())
                {
                    outStrings.emplace(key, kv.second.as<std::string>());
                    continue;
                }
                if (kv.second.IsMap())
                {
                    const YAML::Node valueNode = kv.second["value"];
                    if (!valueNode || !valueNode.IsScalar())
                        continue; // missing required `value:` inside the map — skip
                    outStrings.emplace(key, valueNode.as<std::string>());
                    // Metadata is parsed and stored back on the StringTable by
                    // the caller — see StringTable::LoadFromYAMLString.
                }
            }
            return true;
        }

        // Second pass: pluck per-key metadata out of the same map shape so
        // we can hand it back to StringTable after the values have been
        // merged. Returns empty when no per-key maps were authored.
        std::unordered_map<std::string, StringEntryMetadata> ExtractMetadata(const YAML::Node& root)
        {
            std::unordered_map<std::string, StringEntryMetadata> out;
            if (!root || !root.IsMap())
                return out;
            const YAML::Node stringsNode = root["strings"];
            if (!stringsNode || !stringsNode.IsMap())
                return out;
            for (const auto& kv : stringsNode)
            {
                if (!kv.first.IsScalar() || !kv.second.IsMap())
                    continue;
                StringEntryMetadata md;
                if (auto ctxNode = kv.second["context"]; ctxNode && ctxNode.IsScalar())
                    md.Context = ctxNode.as<std::string>();
                if (auto lenNode = kv.second["max_length"]; lenNode && lenNode.IsScalar())
                    md.MaxLength = lenNode.as<u32>();
                if (!md.Context.empty() || md.MaxLength != 0u)
                    out.emplace(kv.first.as<std::string>(), std::move(md));
            }
            return out;
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
            auto newMetadata = ExtractMetadata(root);
            m_Locale = std::move(newLocale);
            m_Strings = std::move(newStrings);
            m_Metadata = std::move(newMetadata);
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
        m_Metadata.clear();
    }

    StringEntryMetadata StringTable::GetMetadata(const std::string& key) const
    {
        const auto it = m_Metadata.find(key);
        if (it == m_Metadata.end())
            return {};
        return it->second;
    }

    void StringTable::SetMetadata(const std::string& key, StringEntryMetadata md)
    {
        if (md.Context.empty() && md.MaxLength == 0u)
        {
            // Drop empty metadata so GetMetadata's "no entry" path keeps
            // returning a default-constructed struct.
            m_Metadata.erase(key);
            return;
        }
        m_Metadata.insert_or_assign(key, std::move(md));
    }
} // namespace OloEngine
