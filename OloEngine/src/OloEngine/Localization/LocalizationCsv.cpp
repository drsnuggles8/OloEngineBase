#include "OloEnginePCH.h"
#include "OloEngine/Localization/LocalizationCsv.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Core/Log.h"

#include <fstream>
#include <set>
#include <sstream>

namespace OloEngine
{
    namespace
    {
        // RFC-4180 quoting: a field needs wrapping in double-quotes when it
        // contains a comma, double-quote, CR, or LF. Embedded double-quotes
        // are escaped by doubling. UTF-8 is opaque to the codec.
        bool NeedsQuoting(const std::string& field)
        {
            for (char c : field)
            {
                if (c == ',' || c == '"' || c == '\r' || c == '\n')
                    return true;
            }
            return false;
        }

        std::string QuoteField(const std::string& field)
        {
            if (!NeedsQuoting(field))
                return field;
            std::string out;
            out.reserve(field.size() + 2);
            out.push_back('"');
            for (char c : field)
            {
                if (c == '"')
                    out.push_back('"');
                out.push_back(c);
            }
            out.push_back('"');
            return out;
        }

        // Single-pass field-by-field CSV row parser.
        //
        // RFC 4180 semantics: a record ends at an unquoted CR/LF (the next
        // record starts on the following line). A field starts as
        // "unquoted-or-yet-undecided"; if its very first character is a
        // double-quote we transition to a quoted state where commas/newlines
        // are part of the data and only a closing `"` (with a following
        // separator/EOL) ends the field. Doubled quotes inside a quoted
        // field decode to one literal quote.
        //
        // Returns the index of the byte AFTER the record terminator (or
        // `content.size()` when the row was the last in the file). Fields
        // are appended to `outRow`.
        // Returns the index of the byte AFTER the consumed record. The
        // sentinel `kParseRowError` signals a malformed quoted field (either
        // unterminated at EOF, or with garbage characters after the closing
        // quote like `"hello"x`)
        // (the file ran out of bytes while we were still inside `"..."`);
        // callers must treat that as a hard parse failure rather than
        // silently flushing the partial field.
        inline constexpr sizet kParseRowError = static_cast<sizet>(-1);

        sizet ParseRow(const std::string& content, sizet start, std::vector<std::string>& outRow)
        {
            outRow.clear();
            std::string field;
            bool inQuotes = false;
            bool fieldStarted = false;
            sizet i = start;
            while (i < content.size())
            {
                const char c = content[i];
                if (inQuotes)
                {
                    if (c == '"')
                    {
                        // Lookahead for the escape pair `""` — collapses to a single `"` in the data.
                        if (i + 1 < content.size() && content[i + 1] == '"')
                        {
                            field.push_back('"');
                            i += 2;
                            continue;
                        }
                        // Closing quote — what follows must be a field /
                        // record terminator (comma, CR, LF) or end of input.
                        // Anything else (e.g. `"hello"x`) is malformed and
                        // would otherwise be silently appended to the field
                        // by the non-quoted branch on the next iteration.
                        if (i + 1 < content.size())
                        {
                            const char after = content[i + 1];
                            if (after != ',' && after != '\r' && after != '\n')
                            {
                                outRow.clear();
                                return kParseRowError;
                            }
                        }
                        inQuotes = false;
                        ++i;
                    }
                    else
                    {
                        field.push_back(c);
                        ++i;
                    }
                    continue;
                }
                if (!fieldStarted && c == '"')
                {
                    inQuotes = true;
                    fieldStarted = true;
                    ++i;
                    continue;
                }
                if (c == ',')
                {
                    outRow.push_back(std::move(field));
                    field.clear();
                    fieldStarted = false;
                    ++i;
                    continue;
                }
                if (c == '\r')
                {
                    outRow.push_back(std::move(field));
                    // CRLF → consume both.
                    if (i + 1 < content.size() && content[i + 1] == '\n')
                        return i + 2;
                    return i + 1;
                }
                if (c == '\n')
                {
                    outRow.push_back(std::move(field));
                    return i + 1;
                }
                field.push_back(c);
                fieldStarted = true;
                ++i;
            }
            if (inQuotes)
            {
                // Unterminated quoted field — the file ended while we were
                // still inside `"..."`. Silently flushing would mask data
                // corruption (e.g. a translator's quote-in-value that lost
                // its closing pair).
                outRow.clear();
                return kParseRowError;
            }
            // EOF mid-row, not inside quotes — flush the trailing field as
            // the final record of the file.
            outRow.push_back(std::move(field));
            return i;
        }
    } // namespace

    bool LocalizationCsv::ExportToCsv(const std::filesystem::path& path)
    {
        const auto locales = LocalizationManager::GetAvailableLocales();

        // Union of every key across every loaded locale, sorted alphabetically
        // so the output is stable across runs (translators diff-compare CSVs).
        std::set<std::string> allKeys;
        for (const auto& loc : locales)
        {
            for (auto& k : LocalizationManager::GetAllKeys(loc.Code))
                allKeys.insert(std::move(k));
        }

        std::ofstream out(path, std::ios::binary);
        if (!out.is_open())
        {
            OLO_CORE_ERROR("LocalizationCsv::ExportToCsv: cannot open '{}' for writing", path.string());
            return false;
        }

        // Header row: "key,<locale1>,<locale2>,..."
        out << "key";
        for (const auto& loc : locales)
            out << ',' << QuoteField(loc.Code);
        out << "\r\n";

        for (const auto& key : allKeys)
        {
            out << QuoteField(key);
            for (const auto& loc : locales)
            {
                out << ',';
                if (LocalizationManager::HasKey(key, loc.Code))
                    out << QuoteField(LocalizationManager::Get(key, loc.Code));
                // else: empty cell — round-trip-safe.
            }
            out << "\r\n";
        }
        OLO_CORE_INFO("LocalizationCsv::ExportToCsv: wrote {} keys × {} locales to '{}'",
                      allKeys.size(), locales.size(), path.string());
        return true;
    }

    LocalizationCsv::ImportResult LocalizationCsv::ImportFromCsv(const std::filesystem::path& path)
    {
        ImportResult result{};
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            result.Warnings.push_back("cannot open '" + path.string() + "' for reading");
            OLO_CORE_ERROR("LocalizationCsv::ImportFromCsv: cannot open '{}'", path.string());
            return result;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string content = ss.str();

        // Skip a UTF-8 BOM if present — Excel writes one on export and a
        // naive parse would treat the BOM as part of the "key" header cell.
        sizet cursor = 0;
        if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF && static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF)
            cursor = 3;

        std::vector<std::string> header;
        cursor = ParseRow(content, cursor, header);
        if (cursor == kParseRowError)
        {
            result.Warnings.push_back("malformed quoted field in CSV header — aborting import");
            return result;
        }
        if (header.empty() || header[0] != "key")
        {
            result.Warnings.push_back("missing or malformed header row (first column must be 'key')");
            return result;
        }

        // Build the list of locale codes from the header columns and
        // pre-validate each — emit a warning for unknown locales rather
        // than silently dropping the column, but keep going for the others.
        const auto loaded = LocalizationManager::GetAvailableLocales();
        std::set<std::string> loadedCodes;
        for (const auto& loc : loaded)
            loadedCodes.insert(loc.Code);

        std::vector<std::string> columnLocales(header.size() - 1);
        std::set<std::string> updatedLocales;
        for (sizet col = 1; col < header.size(); ++col)
        {
            columnLocales[col - 1] = header[col];
            if (!loadedCodes.contains(header[col]))
            {
                result.Warnings.push_back("CSV column '" + header[col] + "' is not a loaded locale; rows for it will be skipped");
            }
        }

        // Body rows.
        std::vector<std::string> row;
        while (cursor < content.size())
        {
            cursor = ParseRow(content, cursor, row);
            if (cursor == kParseRowError)
            {
                result.Warnings.push_back("malformed quoted field in CSV body — aborting at row " + std::to_string(result.RowsImported + 1));
                break;
            }
            if (row.empty() || row[0].empty())
                continue; // blank line — skip without flagging
            const std::string& key = row[0];
            bool anyApplied = false;
            for (sizet col = 1; col < row.size() && col - 1 < columnLocales.size(); ++col)
            {
                const std::string& localeCode = columnLocales[col - 1];
                const std::string& value = row[col];
                if (value.empty())
                    continue; // empty cell = "no translation supplied"; leave the existing value alone
                if (!LocalizationManager::SetKey(localeCode, key, value))
                    continue; // unknown locale — already warned in the header pass
                updatedLocales.insert(localeCode);
                anyApplied = true;
            }
            if (anyApplied)
                ++result.RowsImported;
        }
        result.LocalesUpdated = static_cast<u32>(updatedLocales.size());

        OLO_CORE_INFO("LocalizationCsv::ImportFromCsv: imported {} rows touching {} locale(s) from '{}'",
                      result.RowsImported, result.LocalesUpdated, path.string());
        return result;
    }
} // namespace OloEngine
