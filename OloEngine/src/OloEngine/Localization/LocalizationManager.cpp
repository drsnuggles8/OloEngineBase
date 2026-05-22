#include "OloEnginePCH.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Localization/LocalizationEvents.h"
#include "OloEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace OloEngine
{
    LocalizationManager::State& LocalizationManager::GetState()
    {
        static State s_State;
        return s_State;
    }

    std::shared_mutex& LocalizationManager::GetMutex()
    {
        static std::shared_mutex s_Mutex;
        return s_Mutex;
    }

    void LocalizationManager::Initialize(const std::filesystem::path& localizationDir)
    {
        State& state = GetState();

        std::vector<std::filesystem::path> files;
        std::error_code ec;
        if (std::filesystem::is_directory(localizationDir, ec))
        {
            for (const auto& entry : std::filesystem::directory_iterator(localizationDir, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;
                if (entry.path().extension() == ".ololocale")
                    files.push_back(entry.path());
            }
        }
        else
        {
            OLO_CORE_WARN("LocalizationManager: localization directory '{}' does not exist or is not a directory", localizationDir.string());
        }

        std::vector<std::pair<std::string, std::filesystem::path>> loaded;
        std::unordered_map<std::string, StringTable> newTables;
        std::unordered_map<std::string, std::filesystem::path> newSources;
        for (const auto& file : files)
        {
            StringTable table;
            if (!table.LoadFromYAML(file.string()))
                continue;
            const std::string code = table.GetLocaleInfo().Code;
            if (code.empty())
            {
                OLO_CORE_WARN("LocalizationManager: '{}' has empty locale code, skipping", file.string());
                continue;
            }
            newSources[code] = file;
            newTables.emplace(code, std::move(table));
            loaded.emplace_back(code, file);
        }

        {
            std::unique_lock lock(GetMutex());
            state.Tables = std::move(newTables);
            state.SourcePaths = std::move(newSources);

            // Preserve the existing active locale if it survives the reload;
            // otherwise pick the alphabetically first one so a fresh
            // Initialize call doesn't leave the manager mute.
            if (state.CurrentLocale.empty() || !state.Tables.contains(state.CurrentLocale))
            {
                if (!state.Tables.empty())
                {
                    std::vector<std::string> codes;
                    codes.reserve(state.Tables.size());
                    for (const auto& kv : state.Tables)
                        codes.push_back(kv.first);
                    std::sort(codes.begin(), codes.end());
                    state.CurrentLocale = codes.front();
                }
                else
                {
                    state.CurrentLocale.clear();
                }
            }
            state.Generation.fetch_add(1, std::memory_order_release);
        }
    }

    void LocalizationManager::Shutdown()
    {
        std::unique_lock lock(GetMutex());
        State& state = GetState();
        state.Tables.clear();
        state.SourcePaths.clear();
        state.CurrentLocale.clear();
        state.Listeners.clear();
        state.NextSubscriptionId = 1;
    }

    bool LocalizationManager::LoadLocale(const std::filesystem::path& filePath)
    {
        StringTable table;
        if (!table.LoadFromYAML(filePath.string()))
            return false;
        const std::string code = table.GetLocaleInfo().Code;
        if (code.empty())
        {
            OLO_CORE_WARN("LocalizationManager::LoadLocale: '{}' has empty locale code", filePath.string());
            return false;
        }

        std::unique_lock lock(GetMutex());
        State& state = GetState();
        state.SourcePaths[code] = filePath;
        state.Tables.insert_or_assign(code, std::move(table));
        if (state.CurrentLocale.empty())
            state.CurrentLocale = code;
        state.Generation.fetch_add(1, std::memory_order_release);
        return true;
    }

    bool LocalizationManager::SetCurrentLocale(const std::string& localeCode)
    {
        std::vector<LocaleChangedListener> listenersCopy;
        std::string previous;

        {
            std::unique_lock lock(GetMutex());
            State& state = GetState();
            if (!state.Tables.contains(localeCode))
            {
                OLO_CORE_WARN("LocalizationManager::SetCurrentLocale: locale '{}' is not loaded", localeCode);
                return false;
            }
            if (state.CurrentLocale == localeCode)
                return true;
            previous = state.CurrentLocale;
            state.CurrentLocale = localeCode;
            state.Generation.fetch_add(1, std::memory_order_release);
            listenersCopy.reserve(state.Listeners.size());
            for (const auto& kv : state.Listeners)
                listenersCopy.push_back(kv.second);
        }

        // Fire outside the lock so listeners can call back into the manager.
        LocaleChangedEvent evt(previous, localeCode);
        for (const auto& listener : listenersCopy)
        {
            if (listener)
                listener(evt);
        }
        return true;
    }

    bool LocalizationManager::ReloadCurrentLocale()
    {
        std::filesystem::path sourcePath;
        std::string code;
        {
            std::shared_lock lock(GetMutex());
            const State& state = GetState();
            if (state.CurrentLocale.empty())
                return false;
            code = state.CurrentLocale;
            const auto it = state.SourcePaths.find(code);
            if (it == state.SourcePaths.end())
                return false;
            sourcePath = it->second;
        }

        StringTable fresh;
        if (!fresh.LoadFromYAML(sourcePath.string()))
            return false;

        std::unique_lock lock(GetMutex());
        State& state = GetState();
        state.Tables.insert_or_assign(code, std::move(fresh));
        state.Generation.fetch_add(1, std::memory_order_release);
        return true;
    }

    std::string LocalizationManager::Get(const std::string& key)
    {
        // Fast path: shared lock, look up, return if present.
        {
            std::shared_lock lock(GetMutex());
            const State& state = GetState();
            if (state.CurrentLocale.empty())
            {
                // Don't track misses when no locale is active — that's a
                // setup error the developer already saw on Initialize.
                return state.MissingKeyFallback;
            }
            const auto it = state.Tables.find(state.CurrentLocale);
            if (it != state.Tables.end() && it->second.Has(key))
                return it->second.Get(key);
        }
        // Slow path: record the miss under a unique lock.
        {
            std::unique_lock lock(GetMutex());
            State& state = GetState();
            state.MissingKeys.insert(key);
            return state.MissingKeyFallback;
        }
    }

    std::string LocalizationManager::Format(const std::string& key, const TextFormatter::ParamMap& params)
    {
        std::string pattern;
        PluralRule rule = PluralRule::OneOther;
        std::string fallback;
        bool hit = false;
        {
            std::shared_lock lock(GetMutex());
            const State& state = GetState();
            fallback = state.MissingKeyFallback;
            if (state.CurrentLocale.empty())
                return fallback;
            const auto it = state.Tables.find(state.CurrentLocale);
            if (it != state.Tables.end() && it->second.Has(key))
            {
                pattern = it->second.Get(key);
                rule = it->second.GetLocaleInfo().Plural;
                hit = true;
            }
        }
        if (!hit)
        {
            std::unique_lock lock(GetMutex());
            GetState().MissingKeys.insert(key);
            return fallback;
        }
        return TextFormatter::Format(pattern, params, rule);
    }

    std::string LocalizationManager::FormatPlural(const std::string& key, const std::string& countParam, i32 count, TextFormatter::ParamMap params)
    {
        params[countParam] = std::to_string(count);
        return Format(key, params);
    }

    std::string LocalizationManager::GetCurrentLocale()
    {
        std::shared_lock lock(GetMutex());
        return GetState().CurrentLocale;
    }

    std::vector<LocaleDefinition> LocalizationManager::GetAvailableLocales()
    {
        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        std::vector<LocaleDefinition> out;
        out.reserve(state.Tables.size());
        for (const auto& kv : state.Tables)
            out.push_back(kv.second.GetLocaleInfo());
        std::sort(out.begin(), out.end(), [](const LocaleDefinition& a, const LocaleDefinition& b)
                  { return a.Code < b.Code; });
        return out;
    }

    bool LocalizationManager::HasKey(const std::string& key)
    {
        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        if (state.CurrentLocale.empty())
            return false;
        const auto it = state.Tables.find(state.CurrentLocale);
        if (it == state.Tables.end())
            return false;
        return it->second.Has(key);
    }

    std::string LocalizationManager::Get(const std::string& key, const std::string& localeCode)
    {
        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        const auto it = state.Tables.find(localeCode);
        if (it == state.Tables.end() || !it->second.Has(key))
            return state.MissingKeyFallback;
        return it->second.Get(key);
    }

    bool LocalizationManager::HasKey(const std::string& key, const std::string& localeCode)
    {
        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        const auto it = state.Tables.find(localeCode);
        if (it == state.Tables.end())
            return false;
        return it->second.Has(key);
    }

    std::vector<std::string> LocalizationManager::GetAllKeys(const std::string& localeCode)
    {
        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        const auto it = state.Tables.find(localeCode);
        if (it == state.Tables.end())
            return {};
        return it->second.GetAllKeys();
    }

    StringEntryMetadata LocalizationManager::GetMetadata(const std::string& key, const std::string& localeCode)
    {
        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        const auto it = state.Tables.find(localeCode);
        if (it == state.Tables.end())
            return {};
        return it->second.GetMetadata(key);
    }

    std::string LocalizationManager::ResolveLocalizedText(const std::string& value)
    {
        if (value.size() <= kLocalizationPrefix.size() || !value.starts_with(kLocalizationPrefix))
            return value;
        return Get(std::string(value.substr(kLocalizationPrefix.size())));
    }

    std::filesystem::path LocalizationManager::ResolveLocalizedAssetPath(const std::filesystem::path& basePath, const std::string& localeCode)
    {
        const std::string code = localeCode.empty() ? GetCurrentLocale() : localeCode;
        if (code.empty() || basePath.empty())
            return basePath;

        // Build the candidate as `<dir>/<stem>.<code><ext>`.
        // - `<ext>` includes the leading dot (".png" → ".png").
        // - For files with no extension, the candidate is `<dir>/<filename>.<code>`.
        std::filesystem::path parent = basePath.parent_path();
        std::string stem = basePath.stem().string();
        std::string ext = basePath.extension().string();
        if (stem.empty())
            return basePath;

        std::filesystem::path candidate = parent / (stem + "." + code + ext);
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec)
            return candidate;

        // Try the language-only fallback when the code includes a region
        // (e.g. de-AT → de). This mirrors NegotiateLocale's pass 2 so a
        // single regional file like `logo.de.png` covers `de`, `de-AT`,
        // `de-CH`, and `de-DE`.
        if (const auto dash = code.find('-'); dash != std::string::npos)
        {
            const std::string lang = code.substr(0, dash);
            std::filesystem::path langCandidate = parent / (stem + "." + lang + ext);
            if (std::filesystem::exists(langCandidate, ec) && !ec)
                return langCandidate;
        }
        return basePath;
    }

    bool LocalizationManager::SetKey(const std::string& localeCode, const std::string& key, const std::string& value)
    {
        std::unique_lock lock(GetMutex());
        State& state = GetState();
        const auto it = state.Tables.find(localeCode);
        if (it == state.Tables.end())
            return false;
        it->second.Set(key, value);
        state.Generation.fetch_add(1, std::memory_order_release);
        return true;
    }

    namespace
    {
        // ASCII → Latin-Extended diacritic substitution table. Pseudo-loc
        // intentionally stays in single-byte territory for the source side
        // (so we don't break alphabetical fonts), but uses UTF-8 multi-byte
        // sequences for the accented forms (which forces the renderer
        // through its non-ASCII path — another bug class the dev tool
        // surfaces). The mapping is approximate and not meant to read as
        // any real language.
        std::string PseudoifyAscii(const std::string& src)
        {
            // UTF-8 encodings of the substitution targets.
            constexpr const char* kLower[26] = {
                "á", "ḅ", "ć", "ḋ", "é", "f̌", "ǵ", "ḣ", "í", "ǰ", "ḱ", "ł", "ṁ",
                "ń", "ó", "ṗ", "q̇", "ŕ", "ś", "ť", "ú", "v̌", "ẅ", "ẋ", "ý", "ź"
            };
            constexpr const char* kUpper[26] = {
                "Á", "Ḅ", "Ć", "Ḋ", "É", "F̌", "Ǵ", "Ḣ", "Í", "J̌", "Ḱ", "Ł", "Ṁ",
                "Ń", "Ó", "Ṗ", "Q̇", "Ŕ", "Ś", "Ť", "Ú", "V̌", "Ẅ", "Ẋ", "Ý", "Ź"
            };

            std::string out;
            out.reserve(src.size() * 2);
            bool inToken = false; // Don't touch characters inside `{...}` substitution tokens — those must stay literal so formatting still works.
            for (sizet i = 0; i < src.size(); ++i)
            {
                const char c = src[i];
                if (c == '{')
                    inToken = true;
                if (inToken)
                {
                    out.push_back(c);
                    if (c == '}')
                        inToken = false;
                    continue;
                }
                if (c >= 'a' && c <= 'z')
                    out.append(kLower[c - 'a']);
                else if (c >= 'A' && c <= 'Z')
                    out.append(kUpper[c - 'A']);
                else
                    out.push_back(c);
            }
            return out;
        }
    } // namespace

    bool LocalizationManager::GeneratePseudoLocale(const std::string& sourceLocaleCode, const std::string& pseudoCode)
    {
        if (pseudoCode.empty())
            return false;

        // Take a snapshot of the source-locale keys under a shared lock so
        // we don't hold the mutex while running PseudoifyAscii on every value.
        std::vector<std::pair<std::string, std::string>> source;
        {
            std::shared_lock lock(GetMutex());
            const State& state = GetState();
            const auto it = state.Tables.find(sourceLocaleCode);
            if (it == state.Tables.end())
            {
                OLO_CORE_WARN("LocalizationManager::GeneratePseudoLocale: source locale '{}' not loaded", sourceLocaleCode);
                return false;
            }
            const auto keys = it->second.GetAllKeys();
            source.reserve(keys.size());
            for (const auto& k : keys)
                source.emplace_back(k, it->second.Get(k));
        }

        LocaleDefinition pseudoDef;
        pseudoDef.Code = pseudoCode;
        pseudoDef.Name = "Pseudo (" + sourceLocaleCode + ")";
        StringTable pseudo(pseudoDef);
        for (const auto& [k, v] : source)
        {
            pseudo.Set(k, "[!! " + PseudoifyAscii(v) + " !!]");
        }

        std::unique_lock lock(GetMutex());
        State& state = GetState();
        state.Tables.insert_or_assign(pseudoCode, std::move(pseudo));
        state.Generation.fetch_add(1, std::memory_order_release);
        return true;
    }

    std::vector<std::string> LocalizationManager::GetMissingKeysSnapshot()
    {
        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        std::vector<std::string> out(state.MissingKeys.begin(), state.MissingKeys.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    void LocalizationManager::ClearMissingKeys()
    {
        std::unique_lock lock(GetMutex());
        GetState().MissingKeys.clear();
    }

    bool LocalizationManager::SaveLocaleToFile(const std::string& localeCode, const std::filesystem::path& pathOverride)
    {
        StringTable snapshot;
        std::filesystem::path resolvedPath = pathOverride;
        {
            std::shared_lock lock(GetMutex());
            const State& state = GetState();
            const auto it = state.Tables.find(localeCode);
            if (it == state.Tables.end())
            {
                OLO_CORE_WARN("LocalizationManager::SaveLocaleToFile: locale '{}' not loaded", localeCode);
                return false;
            }
            snapshot = it->second;
            if (resolvedPath.empty())
            {
                const auto pathIt = state.SourcePaths.find(localeCode);
                if (pathIt == state.SourcePaths.end())
                {
                    OLO_CORE_WARN("LocalizationManager::SaveLocaleToFile: no remembered path for '{}'; pass pathOverride", localeCode);
                    return false;
                }
                resolvedPath = pathIt->second;
            }
        }

        // Hand-roll a minimal YAML emitter rather than wiring yaml-cpp here —
        // the format is fixed and trivial, and avoiding the emitter keeps the
        // diff small for translators reviewing PRs.
        std::error_code ec;
        if (auto parent = resolvedPath.parent_path(); !parent.empty())
            std::filesystem::create_directories(parent, ec);

        std::ofstream out(resolvedPath);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("LocalizationManager::SaveLocaleToFile: cannot open '{}' for writing", resolvedPath.string());
            return false;
        }

        const auto& loc = snapshot.GetLocaleInfo();
        const auto pluralName = [](PluralRule r) -> const char*
        {
            switch (r)
            {
                case PluralRule::OneOther:
                    return "one-other";
                case PluralRule::OtherOnly:
                    return "other-only";
                case PluralRule::FrenchLike:
                    return "french";
                case PluralRule::PolishLike:
                    return "polish";
                case PluralRule::RussianLike:
                    return "russian";
                case PluralRule::ArabicLike:
                    return "arabic";
            }
            return "one-other";
        };

        // Minimal YAML double-quoted-string emitter. Only escapes the chars
        // that need it: backslash, double-quote, and the C0 controls we'd
        // expect inside translator content (\n, \r, \t). Everything else
        // (including UTF-8 multi-byte sequences) passes through as bytes.
        const auto yamlQuote = [](const std::string& s) -> std::string
        {
            std::string out;
            out.reserve(s.size() + 2);
            out.push_back('"');
            for (char c : s)
            {
                switch (c)
                {
                    case '\\':
                        out.append("\\\\");
                        break;
                    case '"':
                        out.append("\\\"");
                        break;
                    case '\n':
                        out.append("\\n");
                        break;
                    case '\r':
                        out.append("\\r");
                        break;
                    case '\t':
                        out.append("\\t");
                        break;
                    default:
                        out.push_back(c);
                }
            }
            out.push_back('"');
            return out;
        };

        out << "locale: " << loc.Code << "\n";
        out << "name: " << yamlQuote(loc.Name) << "\n";
        out << "direction: " << (loc.Direction == TextDirection::RTL ? "rtl" : "ltr") << "\n";
        out << "plural_rule: " << pluralName(loc.Plural) << "\n";
        out << "thousand_separator: " << yamlQuote(loc.ThousandSeparator) << "\n";
        out << "decimal_separator: " << yamlQuote(loc.DecimalSeparator) << "\n";
        out << "currency_symbol: " << yamlQuote(loc.CurrencySymbol) << "\n";
        out << "currency_symbol_before: " << (loc.CurrencySymbolBefore ? "true" : "false") << "\n";
        out << "currency_decimals: " << loc.CurrencyDecimals << "\n";
        out << "list_joiner: " << yamlQuote(loc.ListJoiner) << "\n";
        out << "list_last_joiner: " << yamlQuote(loc.ListLastJoiner) << "\n";
        if (!loc.FontPath.empty())
            out << "font: " << yamlQuote(loc.FontPath) << "\n";
        if (!loc.FontFallbacks.empty())
        {
            out << "font_fallbacks:\n";
            for (const auto& fb : loc.FontFallbacks)
                out << "  - " << yamlQuote(fb) << "\n";
        }
        out << "\nstrings:\n";

        auto keys = snapshot.GetAllKeys();
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys)
        {
            out << "  " << key << ": " << yamlQuote(snapshot.Get(key)) << "\n";
        }
        return true;
    }

    bool LocalizationManager::SaveActiveLocaleToFile(const std::filesystem::path& path)
    {
        std::string locale;
        {
            std::shared_lock lock(GetMutex());
            locale = GetState().CurrentLocale;
        }

        std::error_code ec;
        if (auto parent = path.parent_path(); !parent.empty())
            std::filesystem::create_directories(parent, ec);

        std::ofstream out(path);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("LocalizationManager::SaveActiveLocaleToFile: cannot open '{}' for writing", path.string());
            return false;
        }
        out << "locale: " << locale << "\n";
        return true;
    }

    bool LocalizationManager::LoadActiveLocaleFromFile(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return false;
        std::ifstream in(path);
        if (!in.is_open())
            return false;
        std::stringstream ss;
        ss << in.rdbuf();
        try
        {
            YAML::Node root = YAML::Load(ss.str());
            const auto node = root["locale"];
            if (!node || !node.IsScalar())
                return false;
            const std::string code = node.as<std::string>();
            return SetCurrentLocale(code);
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_WARN("LocalizationManager::LoadActiveLocaleFromFile: YAML parse failure: {}", e.what());
            return false;
        }
    }

    std::string LocalizationManager::NegotiateLocale(const std::vector<std::string>& preferences)
    {
        const auto& prefs = preferences.empty() ? GetSystemPreferredLocales() : preferences;
        if (prefs.empty())
            return {};

        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        if (state.Tables.empty())
            return {};

        const auto languageTag = [](const std::string& code) -> std::string
        {
            const auto dash = code.find('-');
            return dash == std::string::npos ? code : code.substr(0, dash);
        };

        // Pass 1: exact match.
        for (const auto& pref : prefs)
        {
            if (state.Tables.contains(pref))
                return pref;
        }
        // Pass 2: language-only match against the preference's language tag.
        for (const auto& pref : prefs)
        {
            const auto lang = languageTag(pref);
            if (lang != pref && state.Tables.contains(lang))
                return lang;
        }
        // Pass 3: reverse — any loaded code whose language tag matches.
        for (const auto& pref : prefs)
        {
            const auto lang = languageTag(pref);
            for (const auto& kv : state.Tables)
            {
                if (languageTag(kv.first) == lang)
                    return kv.first;
            }
        }
        return {};
    }

    namespace
    {
        // Insert `sep` into the integer-magnitude `digits` string every 3
        // characters from the right. Caller has already stripped any sign.
        std::string GroupThousands(const std::string& digits, const std::string& sep)
        {
            if (sep.empty() || digits.size() <= 3)
                return digits;
            std::string out;
            out.reserve(digits.size() + (digits.size() / 3) * sep.size());
            const sizet firstGroup = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
            sizet i = 0;
            out.append(digits, 0, firstGroup);
            i += firstGroup;
            while (i < digits.size())
            {
                out.append(sep);
                out.append(digits, i, 3);
                i += 3;
            }
            return out;
        }

        // Resolve `localeCode` (or the active locale if empty) to a copy
        // of its number-formatting fields. Falls back to en-US defaults
        // when no locale matches.
        struct NumberFormat
        {
            std::string Thousand;
            std::string Decimal;
        };
        NumberFormat ResolveNumberFormat(const std::string& localeCode)
        {
            const std::string code = localeCode.empty() ? LocalizationManager::GetCurrentLocale() : localeCode;
            for (const auto& loc : LocalizationManager::GetAvailableLocales())
            {
                if (loc.Code == code)
                    return { loc.ThousandSeparator, loc.DecimalSeparator };
            }
            return { ",", "." };
        }
    } // namespace

    std::string LocalizationManager::FormatNumber(i64 value, const std::string& localeCode)
    {
        const auto fmt = ResolveNumberFormat(localeCode);
        const bool negative = value < 0;
        // Take the unsigned magnitude via i128-ish two's-complement trick so
        // INT64_MIN doesn't overflow when negated.
        const u64 magnitude = negative ? (~static_cast<u64>(value) + 1u) : static_cast<u64>(value);
        std::string digits = std::to_string(magnitude);
        std::string body = GroupThousands(digits, fmt.Thousand);
        return negative ? ("-" + body) : body;
    }

    namespace
    {
        // Snapshot the full LocaleDefinition for `localeCode` (or the active
        // locale if empty). Avoids re-walking GetAvailableLocales() at every
        // currency / date / list site.
        LocaleDefinition ResolveLocaleDef(const std::string& localeCode)
        {
            const std::string code = localeCode.empty() ? LocalizationManager::GetCurrentLocale() : localeCode;
            for (const auto& loc : LocalizationManager::GetAvailableLocales())
            {
                if (loc.Code == code)
                    return loc;
            }
            return {};
        }
    } // namespace

    std::string LocalizationManager::FormatCurrency(f64 amount, const std::string& localeCode, const std::string& symbolOverride)
    {
        const auto loc = ResolveLocaleDef(localeCode);
        const std::string symbol = symbolOverride.empty() ? loc.CurrencySymbol : symbolOverride;
        const i32 decimals = (loc.CurrencyDecimals < 0) ? 0 : loc.CurrencyDecimals;
        const std::string numberText = FormatNumber(amount, decimals, localeCode);
        // Negative amounts: keep the minus sign attached to the number, not
        // the symbol — "-$1.50" vs "$-1.50". FormatNumber emits the minus
        // ahead of the digits already; we just splice the symbol around it.
        if (loc.CurrencySymbolBefore)
            return symbol + numberText;
        return numberText + symbol;
    }

    std::string LocalizationManager::FormatList(const std::vector<std::string>& items, const std::string& localeCode)
    {
        if (items.empty())
            return {};
        if (items.size() == 1)
            return items.front();
        const auto loc = ResolveLocaleDef(localeCode);
        // For 2 items, only the last-joiner is used. For 3+ items, the regular
        // joiner glues all-but-the-last pair, then the last-joiner glues the
        // tail. Avoids the off-by-one "apples, oranges and pears" issues you
        // get with naive `accumulate + back().pop`.
        if (items.size() == 2)
            return items[0] + loc.ListLastJoiner + items[1];
        std::string result = items.front();
        for (sizet i = 1; i + 1 < items.size(); ++i)
            result += loc.ListJoiner + items[i];
        result += loc.ListLastJoiner + items.back();
        return result;
    }

    namespace
    {
        // Tiny pattern engine for date / time formatting. Supported tokens
        // (case-sensitive, all surrounded by `{ }` so they don't collide with
        // literal punctuation in the format string):
        //   {yyyy} 4-digit year      {yy}  2-digit year
        //   {MMMM} full month name   {MMM} short month name
        //   {MM}   zero-padded month {M}   month
        //   {dd}   zero-padded day   {d}   day
        //   {EEEE} full weekday name {EEE} short weekday name
        //   {HH}   24-h padded hour  {H}   24-h hour
        //   {hh}   12-h padded hour  {h}   12-h hour
        //   {mm}   zero-padded min   {m}   minute
        //   {ss}   zero-padded sec   {s}   second
        //   {a}    AM/PM (en)
        //
        // Month / weekday names are pulled from the active locale's regular
        // translation table under conventional keys:
        //   date.month.long.<1..12>, date.month.short.<1..12>
        //   date.weekday.long.<0..6>, date.weekday.short.<0..6>
        // where weekday 0 = Sunday. Locales that don't ship these keys fall
        // back to English defaults.
        constexpr const char* kEnglishMonthsLong[12] = {
            "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December"
        };
        constexpr const char* kEnglishMonthsShort[12] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        constexpr const char* kEnglishWeekdaysLong[7] = {
            "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
        };
        constexpr const char* kEnglishWeekdaysShort[7] = {
            "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
        };

        std::string ZeroPad(int v, int width)
        {
            std::string s = std::to_string(v);
            while (static_cast<int>(s.size()) < width)
                s.insert(s.begin(), '0');
            return s;
        }

        std::string LookupOrDefault(const std::string& key, const std::string& localeCode, const char* fallback)
        {
            if (!localeCode.empty() && LocalizationManager::HasKey(key, localeCode))
                return LocalizationManager::Get(key, localeCode);
            return fallback;
        }

        // Apply the pattern tokens documented above to the broken-down
        // calendar fields. Pattern characters outside `{ }` are emitted
        // verbatim, so a French short style might be `{dd}/{MM}/{yyyy}`
        // and an American medium style `{MMM} {d}, {yyyy}`.
        std::string ApplyDateTimePattern(std::string_view pattern, const std::tm& tm, const std::string& localeCode)
        {
            std::string out;
            out.reserve(pattern.size() + 16);
            const int month0 = std::clamp(tm.tm_mon, 0, 11);
            const int weekday0 = std::clamp(tm.tm_wday, 0, 6);
            const int year = tm.tm_year + 1900;
            const int hour24 = tm.tm_hour;
            const int hour12 = ((hour24 % 12) == 0) ? 12 : (hour24 % 12);
            for (sizet i = 0; i < pattern.size();)
            {
                if (pattern[i] != '{')
                {
                    out.push_back(pattern[i]);
                    ++i;
                    continue;
                }
                const sizet end = pattern.find('}', i + 1);
                if (end == std::string_view::npos)
                {
                    out.append(pattern.substr(i));
                    break;
                }
                const std::string_view tok = pattern.substr(i + 1, end - i - 1);
                if (tok == "yyyy")
                    out += ZeroPad(year, 4);
                else if (tok == "yy")
                    out += ZeroPad(year % 100, 2);
                else if (tok == "MMMM")
                    out += LookupOrDefault("date.month.long." + std::to_string(month0 + 1), localeCode, kEnglishMonthsLong[month0]);
                else if (tok == "MMM")
                    out += LookupOrDefault("date.month.short." + std::to_string(month0 + 1), localeCode, kEnglishMonthsShort[month0]);
                else if (tok == "MM")
                    out += ZeroPad(month0 + 1, 2);
                else if (tok == "M")
                    out += std::to_string(month0 + 1);
                else if (tok == "dd")
                    out += ZeroPad(tm.tm_mday, 2);
                else if (tok == "d")
                    out += std::to_string(tm.tm_mday);
                else if (tok == "EEEE")
                    out += LookupOrDefault("date.weekday.long." + std::to_string(weekday0), localeCode, kEnglishWeekdaysLong[weekday0]);
                else if (tok == "EEE")
                    out += LookupOrDefault("date.weekday.short." + std::to_string(weekday0), localeCode, kEnglishWeekdaysShort[weekday0]);
                else if (tok == "HH")
                    out += ZeroPad(hour24, 2);
                else if (tok == "H")
                    out += std::to_string(hour24);
                else if (tok == "hh")
                    out += ZeroPad(hour12, 2);
                else if (tok == "h")
                    out += std::to_string(hour12);
                else if (tok == "mm")
                    out += ZeroPad(tm.tm_min, 2);
                else if (tok == "m")
                    out += std::to_string(tm.tm_min);
                else if (tok == "ss")
                    out += ZeroPad(tm.tm_sec, 2);
                else if (tok == "s")
                    out += std::to_string(tm.tm_sec);
                else if (tok == "a")
                    out += (hour24 < 12) ? "AM" : "PM";
                else
                    out.append(pattern.substr(i, end - i + 1)); // unknown token left literal
                i = end + 1;
            }
            return out;
        }

        // Decompose a time_point into local-time broken-down fields. Uses
        // localtime_s on MSVC / localtime_r elsewhere — both thread-safe.
        std::tm ToLocalTm(std::chrono::system_clock::time_point tp)
        {
            const std::time_t secs = std::chrono::system_clock::to_time_t(tp);
            std::tm tm{};
#ifdef _WIN32
            ::localtime_s(&tm, &secs);
#else
            ::localtime_r(&secs, &tm);
#endif
            return tm;
        }

        std::string DateStyleDefaultPattern(LocalizationManager::DateStyle style)
        {
            // Lookup the locale's authored pattern under
            // date.format.{short|medium|long|full}. Falls back to a
            // reasonable US-English default per style.
            switch (style)
            {
                case LocalizationManager::DateStyle::Short:
                    return "{M}/{d}/{yy}";
                case LocalizationManager::DateStyle::Medium:
                    return "{MMM} {d}, {yyyy}";
                case LocalizationManager::DateStyle::Long:
                    return "{MMMM} {d}, {yyyy}";
                case LocalizationManager::DateStyle::Full:
                    return "{EEEE}, {MMMM} {d}, {yyyy}";
            }
            return "{MMM} {d}, {yyyy}";
        }

        std::string TimeStyleDefaultPattern(LocalizationManager::TimeStyle style)
        {
            switch (style)
            {
                case LocalizationManager::TimeStyle::Short:
                    return "{HH}:{mm}";
                case LocalizationManager::TimeStyle::Medium:
                    return "{HH}:{mm}:{ss}";
            }
            return "{HH}:{mm}";
        }
    } // namespace

    std::string LocalizationManager::FormatDate(std::chrono::system_clock::time_point tp, DateStyle style, const std::string& localeCode)
    {
        const std::string code = localeCode.empty() ? GetCurrentLocale() : localeCode;
        const char* styleSuffix = nullptr;
        switch (style)
        {
            case DateStyle::Short:
                styleSuffix = "short";
                break;
            case DateStyle::Medium:
                styleSuffix = "medium";
                break;
            case DateStyle::Long:
                styleSuffix = "long";
                break;
            case DateStyle::Full:
                styleSuffix = "full";
                break;
        }
        const std::string patternKey = std::string("date.format.") + styleSuffix;
        const std::string pattern = (!code.empty() && HasKey(patternKey, code))
                                        ? Get(patternKey, code)
                                        : DateStyleDefaultPattern(style);
        return ApplyDateTimePattern(pattern, ToLocalTm(tp), code);
    }

    std::string LocalizationManager::FormatTime(std::chrono::system_clock::time_point tp, TimeStyle style, const std::string& localeCode)
    {
        const std::string code = localeCode.empty() ? GetCurrentLocale() : localeCode;
        const char* styleSuffix = (style == TimeStyle::Medium) ? "medium" : "short";
        const std::string patternKey = std::string("time.format.") + styleSuffix;
        const std::string pattern = (!code.empty() && HasKey(patternKey, code))
                                        ? Get(patternKey, code)
                                        : TimeStyleDefaultPattern(style);
        return ApplyDateTimePattern(pattern, ToLocalTm(tp), code);
    }

    std::string LocalizationManager::FormatRelativeTime(std::chrono::system_clock::time_point tp, const std::string& localeCode)
    {
        const auto now = std::chrono::system_clock::now();
        const auto delta = std::chrono::duration_cast<std::chrono::seconds>(tp - now).count();
        const i64 magnitude = delta < 0 ? -delta : delta;
        const bool past = delta < 0;

        // Pick the unit and convert. Author-friendly template keys live under
        // time.relative.<unit>_{past|future} and accept {count} substitution.
        // Falls back to English-ish defaults if the locale doesn't ship them.
        struct Unit
        {
            i64 Seconds;
            const char* Key;
            const char* EnglishPastSingular;
            const char* EnglishPastPlural;
            const char* EnglishFutureSingular;
            const char* EnglishFuturePlural;
        };
        constexpr Unit kUnits[] = {
            { 60, "second",
              "{count} second ago", "{count} seconds ago",
              "in {count} second", "in {count} seconds" },
            { 60 * 60, "minute",
              "{count} minute ago", "{count} minutes ago",
              "in {count} minute", "in {count} minutes" },
            { 60 * 60 * 24, "hour",
              "{count} hour ago", "{count} hours ago",
              "in {count} hour", "in {count} hours" },
            { 60ll * 60 * 24 * 30, "day",
              "{count} day ago", "{count} days ago",
              "in {count} day", "in {count} days" },
        };
        const Unit* unit = nullptr;
        i64 unitCount = 0;
        const sizet unitCount_kUnits = std::size(kUnits);
        for (sizet idx = 0; idx < unitCount_kUnits; ++idx)
        {
            const Unit& u = kUnits[idx];
            if (magnitude < u.Seconds || idx == unitCount_kUnits - 1)
            {
                unit = &u;
                // Whole-unit count = magnitude / (size of one of this unit).
                // For the smallest unit (seconds), divisor = 1; otherwise
                // it's the previous unit's threshold. Round HALF-UP so that
                // "now + 5min" displays as "in 5 minutes" even when the
                // clock has drifted a few microseconds between the caller
                // setting up the time_point and us reading now() again.
                const i64 oneUnit = (idx == 0) ? 1 : kUnits[idx - 1].Seconds;
                unitCount = (magnitude + (oneUnit / 2)) / oneUnit;

                // After rounding it's possible the count crosses into the
                // NEXT unit (e.g. 3599 s rounds to 60 min, but 60 min is the
                // hour threshold). Promote so we say "in 1 hour" instead of
                // "in 60 minutes".
                if (idx + 1 < unitCount_kUnits && unitCount * oneUnit >= u.Seconds)
                {
                    const Unit& next = kUnits[idx + 1];
                    unit = &next;
                    unitCount = (magnitude + (u.Seconds / 2)) / u.Seconds;
                }
                break;
            }
        }
        if (!unit)
            return FormatDate(tp, DateStyle::Medium, localeCode);
        // Magnitude beyond 30 days falls back to absolute date.
        if (unit == &kUnits[std::size(kUnits) - 1] && magnitude >= unit->Seconds)
            return FormatDate(tp, DateStyle::Medium, localeCode);

        const std::string code = localeCode.empty() ? GetCurrentLocale() : localeCode;
        const std::string keyBase = std::string("time.relative.") + unit->Key + (past ? "_past" : "_future");
        if (!code.empty() && HasKey(keyBase, code))
        {
            return FormatPlural(keyBase, "count", static_cast<i32>(unitCount));
        }
        // English fallback path. Pick singular vs plural by raw count
        // since we don't have the locale's plural rule loaded here.
        const bool singular = (unitCount == 1);
        const char* tmpl =
            past
                ? (singular ? unit->EnglishPastSingular : unit->EnglishPastPlural)
                : (singular ? unit->EnglishFutureSingular : unit->EnglishFuturePlural);
        TextFormatter::ParamMap pm;
        pm["count"] = std::to_string(unitCount);
        return TextFormatter::Format(tmpl, pm);
    }

    std::string LocalizationManager::FormatNumber(f64 value, i32 decimals, const std::string& localeCode)
    {
        if (decimals < 0)
            decimals = 0;
        const auto fmt = ResolveNumberFormat(localeCode);
        const bool negative = value < 0.0;
        const f64 magnitude = negative ? -value : value;
        // `%.*f` rounds half-to-even on MSVC's CRT (which differs from
        // half-away-from-zero on glibc) — both acceptable for UI display.
        std::string formatted(64, '\0');
        const int n = std::snprintf(formatted.data(), formatted.size(), "%.*f", decimals, magnitude);
        if (n < 0)
            return "0";
        formatted.resize(static_cast<sizet>(n));
        // Split integer / fractional halves around the C-locale '.' that
        // snprintf produced, then re-glue with locale separators.
        std::string intPart;
        std::string fracPart;
        if (const auto dot = formatted.find('.'); dot != std::string::npos)
        {
            intPart = formatted.substr(0, dot);
            fracPart = formatted.substr(dot + 1);
        }
        else
        {
            intPart = formatted;
        }
        std::string body = GroupThousands(intPart, fmt.Thousand);
        if (!fracPart.empty())
            body += fmt.Decimal + fracPart;
        return negative ? ("-" + body) : body;
    }

    std::vector<std::string> LocalizationManager::GetSystemPreferredLocales()
    {
        std::vector<std::string> out;
#ifdef _WIN32
        // GetUserDefaultLocaleName returns the BCP-47 form already.
        wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {};
        if (::GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH) > 0)
        {
            // Narrow ASCII conversion: locale codes are always ASCII per BCP-47.
            std::string narrow;
            narrow.reserve(LOCALE_NAME_MAX_LENGTH);
            for (wchar_t w : buffer)
            {
                if (w == 0)
                    break;
                narrow.push_back(static_cast<char>(w));
            }
            if (!narrow.empty())
                out.push_back(std::move(narrow));
        }
#else
        // POSIX: parse $LC_ALL, $LANG. e.g. "de_DE.UTF-8" → "de-DE".
        const char* envOrder[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
        for (const char* var : envOrder)
        {
            const char* v = std::getenv(var);
            if (!v || !*v)
                continue;
            std::string s = v;
            if (s == "C" || s == "POSIX")
                continue;
            // Drop encoding suffix (".UTF-8") and modifier ("@euro").
            if (auto dot = s.find('.'); dot != std::string::npos)
                s = s.substr(0, dot);
            if (auto at = s.find('@'); at != std::string::npos)
                s = s.substr(0, at);
            // Normalise "_" to "-" for BCP-47 shape.
            std::replace(s.begin(), s.end(), '_', '-');
            if (!s.empty())
            {
                out.push_back(std::move(s));
                break; // first non-C var wins, matching glibc precedence
            }
        }
#endif
        return out;
    }

    void LocalizationManager::SetMissingKeyFallback(const std::string& fallback)
    {
        std::unique_lock lock(GetMutex());
        GetState().MissingKeyFallback = fallback;
    }

    u64 LocalizationManager::Subscribe(LocaleChangedListener listener)
    {
        if (!listener)
            return 0;
        std::unique_lock lock(GetMutex());
        State& state = GetState();
        const u64 id = state.NextSubscriptionId++;
        state.Listeners.emplace(id, std::move(listener));
        return id;
    }

    void LocalizationManager::Unsubscribe(u64 subscriptionId)
    {
        if (subscriptionId == 0)
            return;
        std::unique_lock lock(GetMutex());
        GetState().Listeners.erase(subscriptionId);
    }

    u64 LocalizationManager::GetGeneration()
    {
        // No locking — the counter is atomic and the read tolerates stale values
        // by design (consumers re-check next frame). The release-acquire pairing
        // sits between the writer's fetch_add and the reader's load.
        return GetState().Generation.load(std::memory_order_acquire);
    }

    void LocalizationManager::ResetForTesting()
    {
        std::unique_lock lock(GetMutex());
        State& state = GetState();
        state.Tables.clear();
        state.SourcePaths.clear();
        state.CurrentLocale.clear();
        state.MissingKeyFallback = "???";
        state.Listeners.clear();
        state.NextSubscriptionId = 1;
        state.MissingKeys.clear();
        // Bump rather than reset so test fixtures that cached a previous
        // generation see a fresh value on the first post-reset query.
        state.Generation.fetch_add(1, std::memory_order_release);
    }
} // namespace OloEngine
