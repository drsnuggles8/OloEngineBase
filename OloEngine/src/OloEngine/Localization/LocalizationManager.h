#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Localization/LocaleDefinition.h"
#include "OloEngine/Localization/StringTable.h"
#include "OloEngine/Localization/TextFormatter.h"
#include "OloEngine/Threading/SharedMutex.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

        // Persist the active locale code to a tiny YAML file (e.g.
        // <project>/userprefs/locale.yaml). The shape is `{ locale: <code> }`
        // — intentionally separate from full SaveGame state so it survives
        // savegame deletions / new playthroughs. Returns false on I/O error.
        [[nodiscard]] static bool SaveActiveLocaleToFile(const std::filesystem::path& path);

        // Restore the active locale from a file written by SaveActiveLocaleToFile.
        // Silently no-ops (returns false) if the file doesn't exist or the
        // recorded locale isn't loaded. Use this on game boot after Initialize.
        [[nodiscard]] static bool LoadActiveLocaleFromFile(const std::filesystem::path& path);

        // Pick the best loaded locale for a BCP-47-style preference list,
        // following standard negotiation:
        //   1. Exact match (e.g. "de-AT" → "de-AT" if loaded)
        //   2. Language-only match (e.g. "de-AT" → "de")
        //   3. Reverse: any loaded locale starting with the same language
        //      tag (e.g. preference "de" matches loaded "de-AT")
        //   4. Returns empty if nothing matches.
        // Does NOT switch — caller is expected to feed the result into
        // SetCurrentLocale(). Pass an empty list to negotiate against the
        // OS user locale (via GetSystemPreferredLocales).
        [[nodiscard("Store this!")]] static std::string NegotiateLocale(const std::vector<std::string>& preferences = {});

        // Query the OS for the user's preferred locale codes, in priority
        // order. Implementation per-platform:
        //   - Windows: GetUserDefaultLocaleName + the registered fallback chain
        //   - Linux/macOS: $LANG and $LC_* parsed (no full ICU integration)
        // The strings come back as BCP-47 ("en-US", "de-DE", ...). Empty
        // list if the OS doesn't expose anything sensible.
        [[nodiscard("Store this!")]] static std::vector<std::string> GetSystemPreferredLocales();

        // Locale-aware number formatting. Uses the active locale's
        // ThousandSeparator / DecimalSeparator (or the named locale if
        // `localeCode` is non-empty). `FormatNumber(i64)` groups by thousands;
        // `FormatNumber(f64, decimals)` rounds + appends a decimal section.
        // Empty thousand_separator suppresses grouping. Negative numbers
        // keep their leading `-` separately from the grouping pass.
        [[nodiscard("Store this!")]] static std::string FormatNumber(i64 value, const std::string& localeCode = {});
        [[nodiscard("Store this!")]] static std::string FormatNumber(f64 value, i32 decimals = 2, const std::string& localeCode = {});

        // Locale-aware currency formatting. Uses the active locale's
        // CurrencySymbol / CurrencySymbolBefore / CurrencyDecimals + the
        // same thousand / decimal separators FormatNumber honours. Pass
        // `symbolOverride` to force a specific currency glyph (e.g. for
        // multi-currency games where the active locale's symbol isn't
        // what you want for this particular price).
        [[nodiscard("Store this!")]] static std::string FormatCurrency(f64 amount, const std::string& localeCode = {}, const std::string& symbolOverride = {});

        // Glue a list of pieces together with the active locale's joiners.
        // "apples, oranges, and pears" (en) / "apples, oranges und pears" (de).
        // Empty list → empty string; single item passes through unchanged.
        [[nodiscard("Store this!")]] static std::string FormatList(const std::vector<std::string>& items, const std::string& localeCode = {});

        // Date / time formatting. We deliberately keep the API minimal —
        // every full ICU-style formatter we'd need lives behind a wrapper
        // call here and uses the locale's month/day name tables (stored as
        // regular translation keys, prefixed `date.month.long.<N>` etc.)
        // plus a small pattern syntax. See LocalizationManager.cpp for
        // the supported pattern tokens.
        enum class DateStyle : u8
        {
            Short = 0,  // "3/5/26"
            Medium = 1, // "Mar 5, 2026"
            Long = 2,   // "March 5, 2026"
            Full = 3,   // "Saturday, March 5, 2026"
        };
        enum class TimeStyle : u8
        {
            Short = 0, // "14:30" or "2:30 PM"
            Medium = 1 // "14:30:15"
        };

        [[nodiscard("Store this!")]] static std::string FormatDate(std::chrono::system_clock::time_point tp, DateStyle style = DateStyle::Medium, const std::string& localeCode = {});
        [[nodiscard("Store this!")]] static std::string FormatTime(std::chrono::system_clock::time_point tp, TimeStyle style = TimeStyle::Short, const std::string& localeCode = {});

        // Relative time — "3 minutes ago" / "in 5 hours". Picks the largest
        // unit whose magnitude is >= 1 and resolves a per-locale plural-
        // aware template (`time.relative.minutes_past` / `..._future`).
        // Falls back to the absolute date when the delta exceeds 30 days.
        [[nodiscard("Store this!")]] static std::string FormatRelativeTime(std::chrono::system_clock::time_point tp, const std::string& localeCode = {});

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

        // Generic "resolve if prefixed" helper for legacy single-string
        // fields (Quest.Title, Item.DisplayName, dialogue choice ports,
        // etc.). If `value` starts with the prefix `@key:`, the rest is
        // treated as a localization key and looked up in the active locale;
        // otherwise `value` is returned verbatim. Lets POD data structures
        // host either a literal string or a translation key in a single
        // field without restructuring every consumer.
        [[nodiscard("Store this!")]] static std::string ResolveLocalizedText(const std::string& value);
        static constexpr std::string_view kLocalizationPrefix = "@key:";

        // Locale-aware asset lookup. Given a base path like
        // `assets/ui/logo.png`, returns the locale-specific variant
        // `assets/ui/logo.de.png` when it exists on disk for the active
        // locale (or any explicit `localeCode` override), otherwise the
        // base path unchanged.
        //
        // Convention: `<dir>/<stem>.<localeCode>.<ext>`. Used for art
        // assets with baked-in text, localized voice-over audio, etc.
        // Game code calls this opt-in — most assets don't need
        // localisation and the helper isn't routed through the asset
        // manager automatically to avoid a stat per load.
        [[nodiscard("Store this!")]] static std::filesystem::path ResolveLocalizedAssetPath(const std::filesystem::path& basePath, const std::string& localeCode = {});

        // Per-locale accessors. Bypass the active-locale concept entirely —
        // intended for editor tooling that needs to compare locales side by
        // side (e.g. "is this key missing in 'de' compared to 'en'?"). All
        // three return the missing-key fallback / empty / false when the
        // locale code itself isn't loaded.
        [[nodiscard("Store this!")]] static std::string Get(const std::string& key, const std::string& localeCode);
        [[nodiscard("Store this!")]] static bool HasKey(const std::string& key, const std::string& localeCode);
        [[nodiscard("Store this!")]] static std::vector<std::string> GetAllKeys(const std::string& localeCode);

        // Forward to StringTable::GetMetadata for the named locale. Returns
        // a default-constructed struct when the locale or key isn't loaded.
        // Used by the editor panel to show context tooltips and by the
        // lint pass to validate translations against the source locale's
        // max_length budget.
        [[nodiscard("Store this!")]] static StringEntryMetadata GetMetadata(const std::string& key, const std::string& localeCode);

        // Insert or overwrite a single key in `localeCode`'s table. Returns
        // false if the locale isn't loaded. Bumps the generation on success
        // so observers pick up the change. Intended for editor tooling (CSV
        // import, hot edits) — the YAML files on disk are the source of
        // truth for shipped content.
        static bool SetKey(const std::string& localeCode, const std::string& key, const std::string& value);

        // Generate (and register) a pseudo-locale by transforming every value
        // of `sourceLocaleCode`'s table with `[!! ... !!]` markers and ASCII→
        // diacritic substitution. The pseudo locale is registered under
        // `pseudoCode` (default "pseudo") and can be switched into via
        // SetCurrentLocale("pseudo"). Used during development to surface:
        //   - Hardcoded strings (any text NOT wrapped in `[!! ... !!]`
        //     wasn't going through LocalizationManager).
        //   - Length-expansion bugs (the pseudo text is ~30 % longer than
        //     the source, matching the typical English→German ratio).
        // Re-call with the same `pseudoCode` to refresh the snapshot after
        // editing the source locale. Returns false if the source locale
        // isn't loaded.
        static bool GeneratePseudoLocale(const std::string& sourceLocaleCode = "en",
                                         const std::string& pseudoCode = "pseudo");

        // Persist a single locale's in-memory table back to its `.ololocale`
        // YAML on disk (the source path remembered at load time, or `pathOverride`
        // when supplied). Used by the editor panel's edit-in-place flow.
        // Returns false if the locale isn't loaded or the file can't be written.
        // Always writes the locale-metadata block + a `strings:` section
        // containing every key currently in the table.
        [[nodiscard]] static bool SaveLocaleToFile(const std::string& localeCode, const std::filesystem::path& pathOverride = {});

        // Accumulator-style reporting for runtime translation gaps. Every
        // Get/Format/HasKey miss against the active locale appends to an
        // internal set; tooling (editor panel, CI lint pass) snapshots and
        // clears the set on its own cadence. Empty until Get/Format actually
        // hits a missing key — no overhead in the happy path.
        [[nodiscard("Store this!")]] static std::vector<std::string> GetMissingKeysSnapshot();
        static void ClearMissingKeys();

        // Override the string returned when a key is missing. Default: "???".
        // The "???" sentinel is deliberately ugly so missing translations are
        // visible during testing.
        static void SetMissingKeyFallback(const std::string& fallback);

        // Register a listener for LocaleChangedEvent. Returns a non-zero
        // subscription id; pass it to Unsubscribe() to remove the listener.
        static u64 Subscribe(LocaleChangedListener listener);
        static void Unsubscribe(u64 subscriptionId);

        // Monotonically increasing counter that bumps every time the active
        // locale changes, the active locale's table is reloaded, or a new
        // table is loaded. Lets pull-based consumers (LocalizationSystem,
        // editor inspectors) detect "anything localization-relevant changed"
        // in O(1) without subscribing to events. Reads are lock-free and
        // safe from any thread.
        [[nodiscard("Store this!")]] static u64 GetGeneration();

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
            std::string MissingKeyFallback{ "???" };
            std::unordered_map<u64, LocaleChangedListener> Listeners;
            u64 NextSubscriptionId = 1;
            std::atomic<u64> Generation{ 0 };
            // Accumulator for unique keys that missed lookup in the active
            // locale. Guarded by the manager's main shared_mutex (writes are
            // rare relative to successful lookups; we take a unique_lock on
            // the miss path).
            std::unordered_set<std::string> MissingKeys;
        };

        static State& GetState();
        static FSharedMutex& GetMutex();
    };
} // namespace OloEngine
