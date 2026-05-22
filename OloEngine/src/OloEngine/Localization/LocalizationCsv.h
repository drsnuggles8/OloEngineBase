#pragma once

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine
{
    // CSV import/export for the localization tables loaded in
    // LocalizationManager. The CSV shape is one row per key, one column per
    // locale. The first column ("key") names the translation key; subsequent
    // columns are headed by locale codes (e.g. "en", "de"). Missing
    // translations show up as empty cells, NOT as the manager's missing-key
    // fallback — round-tripping a missing cell through the importer leaves
    // that key unset in that locale (rather than baking the fallback string).
    //
    // RFC 4180 quoting: cells that contain commas, quotes, or newlines are
    // wrapped in double-quotes and embedded quotes are doubled. The importer
    // applies the same rules in reverse. UTF-8 throughout.
    //
    // Usage example (typical translator workflow):
    //   1. Author writes English strings, calls ExportToCsv("translations.csv").
    //   2. Translator opens the CSV in Excel / a spreadsheet, fills the `de`
    //      column, sends it back.
    //   3. Author calls ImportFromCsv("translations.csv") — every existing
    //      table gets the new strings layered in (overwriting collisions).
    //      The manager generation bumps so all listeners refresh.
    class LocalizationCsv
    {
      public:
        struct ImportResult
        {
            u32 LocalesUpdated = 0;
            u32 RowsImported = 0;
            std::vector<std::string> Warnings;
        };

        // Write every currently-loaded locale to a single CSV at `path`.
        // The locale-code order is alphabetical (matches GetAvailableLocales).
        // Returns false on I/O error; succeeds with an empty CSV (just the
        // header row) if no locales are loaded.
        [[nodiscard]] static bool ExportToCsv(const std::filesystem::path& path);

        // Read a CSV at `path` and merge its translations into the
        // LocalizationManager's existing tables. Locales referenced by the
        // CSV's columns must already be loaded — unknown locale codes are
        // recorded as warnings, NOT created. Empty cells are skipped (the
        // existing value, if any, stays). On success the manager generation
        // is bumped, so any LocalizationSystem / editor panel observers
        // pick up the new strings on their next tick.
        [[nodiscard]] static ImportResult ImportFromCsv(const std::filesystem::path& path);
    };
} // namespace OloEngine
