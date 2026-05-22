using System;
using System.Collections.Generic;

namespace OloEngine
{
    // Managed-side wrapper around the engine's LocalizationManager.
    //
    // Mirrors the Lua surface: lookup, named-parameter format, plural-form
    // resolution, locale switching, current-locale query, key presence.
    // Use this for all user-facing strings produced by C# scripts so the
    // text follows the active locale.
    public static class Localization
    {
        public static string Get(string key)
        {
            if (string.IsNullOrEmpty(key))
                throw new ArgumentException("key cannot be null or empty", nameof(key));
            return InternalCalls.Localization_Get(key);
        }

        // Substitute `{name}` tokens in the looked-up template. Returns the
        // missing-key fallback (`???` by default) if `key` isn't loaded.
        public static string Format(string key, IDictionary<string, string> parameters = null)
        {
            if (string.IsNullOrEmpty(key))
                throw new ArgumentException("key cannot be null or empty", nameof(key));
            var (keys, values) = FlattenParams(parameters);
            return InternalCalls.Localization_Format(key, keys, values);
        }

        // Substitute named parameters AND resolve `{count:singular|plural}`
        // tokens against the active locale's plural rule. `countParam`
        // identifies which key in `parameters` (or the auto-injected one if
        // omitted) carries the count value.
        public static string FormatPlural(string key, string countParam, int count, IDictionary<string, string> parameters = null)
        {
            if (string.IsNullOrEmpty(key))
                throw new ArgumentException("key cannot be null or empty", nameof(key));
            if (string.IsNullOrEmpty(countParam))
                throw new ArgumentException("countParam cannot be null or empty", nameof(countParam));
            var (keys, values) = FlattenParams(parameters);
            return InternalCalls.Localization_FormatPlural(key, countParam, count, keys, values);
        }

        // Returns false if `localeCode` isn't loaded; the current locale is
        // left unchanged.
        public static bool SetLocale(string localeCode)
        {
            if (string.IsNullOrEmpty(localeCode))
                throw new ArgumentException("localeCode cannot be null or empty", nameof(localeCode));
            return InternalCalls.Localization_SetLocale(localeCode);
        }

        public static string GetCurrentLocale()
            => InternalCalls.Localization_GetCurrentLocale();

        public static bool HasKey(string key)
        {
            if (string.IsNullOrEmpty(key))
                return false;
            return InternalCalls.Localization_HasKey(key);
        }

        // Quest titles, item names, and similar gameplay strings live in
        // single fields that historically held literals. Authors can now
        // opt those fields into localization by writing `@key:item.sword.name`
        // — call this from any UI rendering path to honour that convention.
        // Plain (un-prefixed) values pass through unchanged.
        public static string ResolveLocalizedText(string value)
        {
            if (value == null)
                return string.Empty;
            return InternalCalls.Localization_ResolveLocalizedText(value);
        }

        // Locale-aware integer formatting. Uses the active locale's
        // thousand_separator (e.g. "," for en, "." for de). Pass
        // `localeCode` to format against a specific locale without
        // switching the active one.
        public static string FormatNumber(long value, string localeCode = null)
            => InternalCalls.Localization_FormatInt(value, localeCode ?? string.Empty);

        public static string FormatNumber(double value, int decimals = 2, string localeCode = null)
            => InternalCalls.Localization_FormatFloat(value, decimals, localeCode ?? string.Empty);

        // Wipe the runtime's missing-key accumulator. Typically called by
        // the editor panel after the user has reviewed and addressed the
        // list. The accumulator otherwise grows for the life of the session.
        public static void ClearMissingKeys()
            => InternalCalls.Localization_ClearMissingKeys();

        // Synthesize a pseudo-locale by wrapping every value of
        // `sourceCode`'s table with `[!! Ḧëļļö !!]`-style markers. Returns
        // false if the source locale isn't loaded. Once generated, the
        // pseudo locale shows up under `pseudoCode` (default "pseudo")
        // and can be selected via SetLocale("pseudo"). Use during
        // development to surface any text that isn't going through the
        // localization layer.
        public static bool GeneratePseudoLocale(string sourceCode = "en", string pseudoCode = "pseudo")
            => InternalCalls.Localization_GeneratePseudoLocale(sourceCode ?? "en", pseudoCode ?? "pseudo");

        // Locale-aware currency formatting. Uses the active locale's
        // currency symbol + thousands/decimal separators + decimal-count
        // unless `symbolOverride` is non-null (multi-currency UI in a
        // single-locale game).
        public static string FormatCurrency(double amount, string localeCode = null, string symbolOverride = null)
            => InternalCalls.Localization_FormatCurrency(amount, localeCode ?? string.Empty, symbolOverride ?? string.Empty);

        // List joiner. "apples, oranges, and pears" in English; the active
        // locale's list_joiner / list_last_joiner govern the separators.
        public static string FormatList(System.Collections.Generic.IEnumerable<string> items, string localeCode = null)
        {
            if (items == null)
                return string.Empty;
            var array = new System.Collections.Generic.List<string>(items).ToArray();
            return InternalCalls.Localization_FormatList(array, localeCode ?? string.Empty);
        }

        public enum DateStyle  { Short = 0, Medium = 1, Long = 2, Full = 3 }
        public enum TimeStyle  { Short = 0, Medium = 1 }

        // Locale-aware date / time formatting. C# DateTime values are
        // converted to Unix seconds before crossing the Mono boundary —
        // a single deterministic round-trip rather than DateTime <->
        // System.DateTime marshalling games.
        public static string FormatDate(System.DateTimeOffset when, DateStyle style = DateStyle.Medium, string localeCode = null)
            => InternalCalls.Localization_FormatDate(when.ToUnixTimeSeconds(), (int)style, localeCode ?? string.Empty);

        public static string FormatTime(System.DateTimeOffset when, TimeStyle style = TimeStyle.Short, string localeCode = null)
            => InternalCalls.Localization_FormatTime(when.ToUnixTimeSeconds(), (int)style, localeCode ?? string.Empty);

        // Relative time: "3 minutes ago" / "in 5 hours". Falls back to an
        // absolute date when the delta exceeds 30 days.
        public static string FormatRelativeTime(System.DateTimeOffset when, string localeCode = null)
            => InternalCalls.Localization_FormatRelativeTime(when.ToUnixTimeSeconds(), localeCode ?? string.Empty);

        // Split a key->value dictionary into two arrays of equal length —
        // the marshaling shape the engine expects (see InternalCalls.cs and
        // ScriptGlue.cpp for the rationale).
        private static (string[] keys, string[] values) FlattenParams(IDictionary<string, string> parameters)
        {
            if (parameters == null || parameters.Count == 0)
                return (null, null);
            var keys = new string[parameters.Count];
            var values = new string[parameters.Count];
            int i = 0;
            foreach (var kv in parameters)
            {
                keys[i] = kv.Key;
                values[i] = kv.Value ?? string.Empty;
                ++i;
            }
            return (keys, values);
        }
    }
}
