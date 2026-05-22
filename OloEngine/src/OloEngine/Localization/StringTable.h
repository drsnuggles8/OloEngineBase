#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Localization/LocaleDefinition.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Optional translator-facing per-key metadata. Source locales author
    // `Context` to disambiguate (e.g. "Run" — execute vs. jog vs. flowing),
    // and `MaxLength` as a UI-fits constraint that the localization lint
    // pass enforces. Both are optional; the simple `key: "value"` YAML
    // shorthand still works and produces empty metadata.
    struct StringEntryMetadata
    {
        std::string Context;
        u32 MaxLength = 0; // 0 = unconstrained
    };

    // Per-locale key->string mapping. Owns the locale's metadata block.
    // Loaded by LocalizationManager from a .ololocale YAML file.
    class StringTable
    {
      public:
        StringTable() = default;
        explicit StringTable(LocaleDefinition locale) : m_Locale(std::move(locale)) {}

        // Populate from a YAML document on disk. Returns false on parse error
        // or if the file is missing the required `locale` and `strings`
        // sections. On failure the existing table contents are left untouched
        // so a hot-reload that hits a bad save doesn't blank the UI.
        bool LoadFromYAML(const std::string& filePath);

        // Populate from an in-memory YAML string. Same contract as
        // LoadFromYAML — useful for tests that don't want to touch the FS.
        bool LoadFromYAMLString(const std::string& yamlContent);

        // Returns the looked-up string, or an empty string if the key is
        // missing. The LocalizationManager layers the "???" fallback policy
        // on top — this is the lower-level primitive.
        [[nodiscard("Store this!")]] const std::string& Get(const std::string& key) const;

        [[nodiscard("Store this!")]] bool Has(const std::string& key) const;

        [[nodiscard("Store this!")]] const LocaleDefinition& GetLocaleInfo() const
        {
            return m_Locale;
        }

        [[nodiscard("Store this!")]] sizet Size() const
        {
            return m_Strings.size();
        }

        // Enumerate every translation key. Order is unspecified. Used by the
        // editor's missing-key validation and by translator-export tooling.
        [[nodiscard("Store this!")]] std::vector<std::string> GetAllKeys() const;

        // Insert or overwrite a key->value pair. Primarily for programmatic
        // use (tests, editor-side authoring) — locale files are normally the
        // source of truth.
        void Set(const std::string& key, const std::string& value);

        void Clear();

        // Lookup / set metadata for a key. Missing keys return an empty
        // metadata struct; `SetMetadata` always upserts (no key-must-exist
        // precondition, so authors can stub metadata before the value).
        [[nodiscard("Store this!")]] StringEntryMetadata GetMetadata(const std::string& key) const;
        void SetMetadata(const std::string& key, StringEntryMetadata md);

      private:
        LocaleDefinition m_Locale;
        std::unordered_map<std::string, std::string> m_Strings;
        std::unordered_map<std::string, StringEntryMetadata> m_Metadata;
    };
} // namespace OloEngine
