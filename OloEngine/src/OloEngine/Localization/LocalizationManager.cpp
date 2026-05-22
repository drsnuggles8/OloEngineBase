#include "OloEnginePCH.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Localization/LocalizationEvents.h"
#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <system_error>

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
        return true;
    }

    std::string LocalizationManager::Get(const std::string& key)
    {
        std::shared_lock lock(GetMutex());
        const State& state = GetState();
        if (state.CurrentLocale.empty())
            return state.MissingKeyFallback;
        const auto it = state.Tables.find(state.CurrentLocale);
        if (it == state.Tables.end())
            return state.MissingKeyFallback;
        if (!it->second.Has(key))
            return state.MissingKeyFallback;
        return it->second.Get(key);
    }

    std::string LocalizationManager::Format(const std::string& key, const TextFormatter::ParamMap& params)
    {
        std::string pattern;
        PluralRule rule = PluralRule::OneOther;
        std::string fallback;
        {
            std::shared_lock lock(GetMutex());
            const State& state = GetState();
            fallback = state.MissingKeyFallback;
            if (state.CurrentLocale.empty())
                return fallback;
            const auto it = state.Tables.find(state.CurrentLocale);
            if (it == state.Tables.end())
                return fallback;
            if (!it->second.Has(key))
                return fallback;
            pattern = it->second.Get(key);
            rule = it->second.GetLocaleInfo().Plural;
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
    }
} // namespace OloEngine
