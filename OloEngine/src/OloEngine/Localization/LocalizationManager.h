#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Localization/LocaleDefinition.h"
#include "OloEngine/Localization/StringTable.h"
#include "OloEngine/Localization/TextFormatter.h"

#include <filesystem>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class LocaleChangedEvent;

    // Central, process-wide localization service. Holds a set of loaded
    // StringTables keyed by locale code (e.g. "en", "de") and exposes the
    // currently active table for lookups.
    //
    // Thread safety: all public methods are safe to call from any thread.
    // Reads (Get / Format / HasKey / GetCurrentLocale / GetAvailableLocales)
    // take a shared lock; mutations (LoadLocale / SetCurrentLocale /
    // ReloadCurrentLocale / Initialize / Shutdown) take an exclusive lock.
    // Listeners are invoked outside the lock to avoid reentrant deadlocks if
    // a listener calls back into the manager.
    class LocalizationManager
    {
      public:
        using LocaleChangedListener = std::function<void(const LocaleChangedEvent&)>;

        // Scans `localizationDir` for .ololocale files and loads them all.
        // Safe to call repeatedly — replaces the previous state. The first
        // locale alphabetically becomes the active one unless one was already
        // set by a prior SetCurrentLocale().
        static void Initialize(const std::filesystem::path& localizationDir);

        // Release all loaded tables and clear the current locale. Listeners
        // are NOT notified — Shutdown is for app-tear-down, not a UI-visible
        // locale switch.
        static void Shutdown();

        // Load a single .ololocale file into the manager's table set, keyed
        // by the code declared inside the file. Overwrites any existing
        // entry for that code.
        static bool LoadLocale(const std::filesystem::path& filePath);

        // Switch the active locale. If `localeCode` isn't loaded yet, returns
        // false and leaves state untouched. Fires LocaleChangedEvent to every
        // listener registered via Subscribe().
        static bool SetCurrentLocale(const std::string& localeCode);

        // Re-load the active locale's source file from disk and replace the
        // in-memory table. Used by the editor's hot-reload pipeline. Does NOT
        // fire LocaleChangedEvent (the active code didn't change) — listeners
        // that care about content changes can subscribe to the asset-reload
        // event instead.
        static bool ReloadCurrentLocale();

        // Looked-up text for `key` in the current locale. Returns the fallback
        // string ("???" by default) if the key isn't in the active table or
        // no locale is currently active. Returned by value (not by ref) so
        // callers can safely use the result across a locale switch on another
        // thread.
        [[nodiscard("Store this!")]] static std::string Get(const std::string& key);

        // Get the active locale's raw template and apply named-parameter
        // substitution + plural-form resolution against it. Plural rule is
        // taken from the active locale's LocaleDefinition.
        [[nodiscard("Store this!")]] static std::string Format(const std::string& key, const TextFormatter::ParamMap& params);

        // Shorthand for Format() with a stringified count parameter. See
        // TextFormatter::FormatPlural.
        [[nodiscard("Store this!")]] static std::string FormatPlural(const std::string& key, const std::string& countParam, i32 count, TextFormatter::ParamMap params = {});

        [[nodiscard("Store this!")]] static std::string GetCurrentLocale();

        // Locale-info snapshots for every loaded table, in alphabetic order
        // of code. Useful for populating a language-picker UI.
        [[nodiscard("Store this!")]] static std::vector<LocaleDefinition> GetAvailableLocales();

        [[nodiscard("Store this!")]] static bool HasKey(const std::string& key);

        // Override the string returned when a key is missing. Default: "???".
        // The "???" sentinel is deliberately ugly so missing translations are
        // visible during testing.
        static void SetMissingKeyFallback(const std::string& fallback);

        // Register a listener for LocaleChangedEvent. Returns a non-zero
        // subscription id; pass it to Unsubscribe() to remove the listener.
        static u64 Subscribe(LocaleChangedListener listener);
        static void Unsubscribe(u64 subscriptionId);

        // Test-only: wipe every piece of static state. Distinct from Shutdown
        // because Initialize is allowed to re-populate from the same dir; in
        // unit tests we need a true blank slate between cases. NOT thread-safe
        // — call only when no other thread is touching the manager.
        static void ResetForTesting();

      private:
        struct State
        {
            std::unordered_map<std::string, StringTable> Tables;
            std::unordered_map<std::string, std::filesystem::path> SourcePaths;
            std::string CurrentLocale;
            std::string MissingKeyFallback{"???"};
            std::unordered_map<u64, LocaleChangedListener> Listeners;
            u64 NextSubscriptionId = 1;
        };

        static State& GetState();
        static std::shared_mutex& GetMutex();
    };
} // namespace OloEngine
