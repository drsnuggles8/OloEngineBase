#pragma once

// Pure, engine-light rendering of a structured MCP tool payload into the
// HUMAN-facing content block of a dual-audience tool result (issue #673 Tier 2;
// MCP spec 2025-06-18 `Annotations.audience` on a ContentBlock).
//
// A dual-audience result carries the SAME data twice:
//   * a compact JSON block tagged audience ["assistant"] — what the model
//     parses (it also receives the identical `structuredContent`), and
//   * the Markdown report built here, tagged audience ["user"] — what the human
//     watching the same session reads.
// Clients that honour annotations show each side only what it asked for;
// clients that ignore them show both, which is exactly why the machine mirror
// is emitted compact — the pair then costs about what the single
// pretty-printed block of ToolResult::Structured() did.
//
// Markdown, not space-aligned plain text: a client that renders the user block
// as Markdown collapses runs of spaces and soft line breaks, which would
// destroy a space-aligned table. Cells are ALSO padded, so the same string
// still reads as a tidy table in a raw terminal or log.
//
// Engine-free by design (nlohmann + std only), so it unit-tests directly
// against synthetic payloads — the same pattern as McpPassTimings.h /
// McpFrameBreakdown.h / McpRenderExplain.h.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OloEngine::MCP::AudienceReport
{
    using Json = nlohmann::json;

    // The human block is a GLANCEABLE summary, never the archive: the assistant
    // block and `structuredContent` always carry the payload in full, so every
    // bound below elides rather than loses.
    inline constexpr sizet kMaxTableRows = 24;       // rows printed per table.
    inline constexpr sizet kMaxInlineListItems = 12; // items printed for a scalar array.
    inline constexpr sizet kMaxCellChars = 64;       // characters printed per table cell.
    inline constexpr sizet kMaxParagraphChars = 600; // characters printed for a long free-text field.
    // Below this depth a nested object/table is rendered as its own section;
    // at or beyond it, as one compact-JSON cell. Bounds the report against a
    // deeply nested payload without needing a per-tool opinion.
    inline constexpr int kMaxDepth = 3;

    namespace Detail
    {
        [[nodiscard]] inline bool IsScalar(const Json& value)
        {
            return !value.is_object() && !value.is_array();
        }

        // An array every element of which is an object — the shape that renders
        // as a table. An empty array is NOT one (there are no columns to infer).
        [[nodiscard]] inline bool IsObjectArray(const Json& value)
        {
            if (!value.is_array() || value.empty())
                return false;
            for (const Json& element : value)
            {
                if (!element.is_object())
                    return false;
            }
            return true;
        }

        // Truncate to at most `maxBytes` WITHOUT splitting a UTF-8 sequence.
        // nlohmann::json::dump() throws on invalid UTF-8, so a byte-wise resize
        // through a multi-byte codepoint (an entity name, an asset path) would
        // turn a cosmetic bound into a failed tool response.
        [[nodiscard]] inline std::string TruncateUtf8(std::string text, sizet maxBytes)
        {
            if (text.size() <= maxBytes)
                return text;
            sizet cut = maxBytes;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u)
                --cut;
            text.resize(cut);
            text.append("...");
            return text;
        }

        // One table/field cell: strings raw (no JSON quoting — this is prose,
        // not a payload), everything else via dump(). Newlines would end the
        // Markdown row early and a literal '|' would open a phantom column, so
        // both are neutralised.
        [[nodiscard]] inline std::string Cell(const Json& value)
        {
            const std::string source = value.is_string() ? value.get<std::string>()
                                       : value.is_null() ? std::string("-")
                                                         : value.dump();
            std::string out;
            out.reserve(source.size());
            for (const char c : source)
            {
                if (c == '\n' || c == '\r' || c == '\t')
                    out.push_back(' ');
                else if (c == '|')
                    out.append("\\|");
                else
                    out.push_back(c);
            }
            return TruncateUtf8(std::move(out), kMaxCellChars);
        }

        [[nodiscard]] inline std::string Pad(std::string text, sizet width)
        {
            if (text.size() < width)
                text.append(width - text.size(), ' ');
            return text;
        }

        inline void EmitRow(const std::vector<std::string>& cells, const std::vector<sizet>& widths,
                            std::string& out)
        {
            out += '|';
            for (sizet c = 0; c < cells.size(); ++c)
            {
                out += ' ';
                out += Pad(cells[c], widths[c]);
                out += " |";
            }
            out += '\n';
        }

        inline void EmitSeparator(const std::vector<sizet>& widths, std::string& out)
        {
            out += '|';
            for (const sizet width : widths)
            {
                out += ' ';
                out.append(width, '-');
                out += " |";
            }
            out += '\n';
        }

        // A Markdown table over an array of objects. Columns are the union of
        // the rows' keys in first-appearance order, so a row that omits an
        // optional field still lines up (missing cells render as "-").
        inline void RenderTable(const Json& rows, std::string& out)
        {
            std::vector<std::string> columns;
            for (const Json& row : rows)
            {
                for (auto it = row.begin(); it != row.end(); ++it)
                {
                    if (std::find(columns.begin(), columns.end(), it.key()) == columns.end())
                        columns.push_back(it.key());
                }
            }
            if (columns.empty())
            {
                out += "_(rows carry no fields)_\n";
                return;
            }

            const sizet shown = std::min<sizet>(rows.size(), kMaxTableRows);
            std::vector<std::vector<std::string>> cells;
            cells.reserve(shown);
            for (sizet i = 0; i < shown; ++i)
            {
                std::vector<std::string> line;
                line.reserve(columns.size());
                for (const std::string& column : columns)
                {
                    const auto found = rows[i].find(column);
                    line.push_back(found == rows[i].end() ? std::string("-") : Cell(*found));
                }
                cells.push_back(std::move(line));
            }

            std::vector<sizet> widths;
            widths.reserve(columns.size());
            for (const std::string& column : columns)
                widths.push_back(column.size());
            for (const auto& line : cells)
            {
                for (sizet c = 0; c < columns.size(); ++c)
                    widths[c] = std::max(widths[c], line[c].size());
            }

            EmitRow(columns, widths, out);
            EmitSeparator(widths, out);
            for (const auto& line : cells)
                EmitRow(line, widths, out);
            if (rows.size() > shown)
            {
                out += "_... " + std::to_string(rows.size() - shown) +
                       " more row(s) omitted; the JSON block has the full list._\n";
            }
        }

        // Split one object's members into the four things they can render as,
        // preserving key order within each bucket, then emit scalars first (the
        // at-a-glance summary), prose second, and the structural sections last.
        inline void RenderObject(const Json& object, int depth, std::string& out)
        {
            std::vector<std::pair<std::string, std::string>> fields;
            std::vector<std::pair<std::string, std::string>> paragraphs;
            std::vector<std::pair<std::string, const Json*>> sections;
            std::vector<std::pair<std::string, const Json*>> tables;

            for (auto it = object.begin(); it != object.end(); ++it)
            {
                const Json& value = it.value();
                if (value.is_object())
                {
                    if (value.empty())
                        fields.emplace_back(it.key(), "(empty)");
                    else if (depth >= kMaxDepth)
                        fields.emplace_back(it.key(), Cell(value));
                    else
                        sections.emplace_back(it.key(), &value);
                }
                else if (IsObjectArray(value))
                {
                    if (depth >= kMaxDepth)
                        fields.emplace_back(it.key(), Cell(value));
                    else
                        tables.emplace_back(it.key(), &value);
                }
                else if (value.is_array())
                {
                    if (value.empty())
                    {
                        fields.emplace_back(it.key(), "(none)");
                    }
                    else
                    {
                        std::string joined;
                        sizet emitted = 0;
                        for (const Json& element : value)
                        {
                            if (emitted == kMaxInlineListItems)
                            {
                                joined += ", ...";
                                break;
                            }
                            if (emitted > 0)
                                joined += ", ";
                            joined += Cell(element);
                            ++emitted;
                        }
                        fields.emplace_back(it.key(), TruncateUtf8(std::move(joined), kMaxParagraphChars));
                    }
                }
                else if (value.is_string() && value.get<std::string>().size() > kMaxCellChars)
                {
                    // A `note` / `warning` / explainer sentence: a table cell
                    // would elide the part that matters, so give it a paragraph.
                    paragraphs.emplace_back(it.key(),
                                            TruncateUtf8(value.get<std::string>(), kMaxParagraphChars));
                }
                else
                {
                    fields.emplace_back(it.key(), Cell(value));
                }
            }

            if (!fields.empty())
            {
                const std::vector<std::string> header = { "field", "value" };
                std::vector<sizet> widths = { header[0].size(), header[1].size() };
                for (const auto& [key, value] : fields)
                {
                    widths[0] = std::max(widths[0], key.size());
                    widths[1] = std::max(widths[1], value.size());
                }
                EmitRow(header, widths, out);
                EmitSeparator(widths, out);
                for (const auto& [key, value] : fields)
                    EmitRow({ key, value }, widths, out);
                out += '\n';
            }

            for (const auto& [key, text] : paragraphs)
                out += "**" + key + "**\n\n" + text + "\n\n";

            for (const auto& [key, nested] : sections)
            {
                out += "**" + key + "**\n\n";
                RenderObject(*nested, depth + 1, out);
            }

            for (const auto& [key, rows] : tables)
            {
                out += "**" + key + "** (" + std::to_string(rows->size()) + ")\n\n";
                RenderTable(*rows, out);
                out += '\n';
            }
        }
    } // namespace Detail

    // Render `data` as the Markdown report for the user-audience content block.
    // `title` becomes the heading (omitted when empty). Always returns a
    // non-empty, newline-terminated string, so the block is never blank.
    [[nodiscard]] inline std::string Render(const Json& data, std::string_view title)
    {
        std::string out;
        if (!title.empty())
        {
            out += "### ";
            out.append(title);
            out += "\n\n";
        }

        if (data.is_object() && !data.empty())
            Detail::RenderObject(data, 0, out);
        else if (data.is_object() || data.is_null())
            out += "_(no data)_\n";
        else if (Detail::IsObjectArray(data))
            Detail::RenderTable(data, out);
        else
            out += Detail::Cell(data) + "\n";

        while (!out.empty() && out.back() == '\n')
            out.pop_back();
        out += '\n';
        return out;
    }
} // namespace OloEngine::MCP::AudienceReport
