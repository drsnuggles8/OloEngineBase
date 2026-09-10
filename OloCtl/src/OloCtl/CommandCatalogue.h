#pragma once

// The catalogue oloctl generates its command surface from (issue #1125).
//
// oloctl has NO table of commands. Everything it can do it learns at runtime
// from a catalogue — an array of command descriptions in the shape
// Automation::DescribeForFrontend emits, whether that array arrived over the
// wire from a running editor or straight out of an in-process registry. That is
// the whole design: adding a command to the registry adds it to the CLI, and no
// edit here is involved.
//
// So this file parses, it does not declare. The one thing it insists on is that
// a description it cannot understand is REPORTED rather than dropped — a
// silently skipped entry would look exactly like a command that was never
// registered, which is the failure mode a generated frontend exists to rule out.
//
// Deliberately free of OloEngine/Core/Base.h: the oloctl executable links
// nothing from the engine (it speaks JSON over a socket), so pulling in the
// logging stack for the sake of `u32` would mean linking OloEngine into a CLI
// that has no use for it. Standard-library types throughout.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace OloCtl
{
    // Whether running a command mutates the user's project.
    //
    // `Unknown` is a real, distinct state and not a synonym for ReadOnly: it means
    // the catalogue did not report `projectWrite` at all — an editor older than
    // #1125, or a catalogue assembled from a plain MCP `tools/list`, which models
    // no authority class. oloctl REFUSES an Unknown command rather than assuming
    // it is safe. Guessing here would be a way around the consent model, which is
    // exactly what AutomationRegistry's header forbids.
    enum class AuthorityClass
    {
        ReadOnly,
        ProjectWrite,
        Unknown,
    };

    [[nodiscard]] const char* ToString(AuthorityClass authority);

    // One command, as the CLI needs to see it.
    struct CatalogueEntry
    {
        // The registry name, e.g. "olo_scene_list_entities". This is what gets sent
        // back to the host; the group/command spelling the user types is derived
        // from it (CommandTree) and never travels.
        std::string Name;
        std::string Title;
        std::string Description;
        // Grouping category, from the `_meta` toolset key. May be empty.
        std::string Toolset;
        // Always an object: an entry that declared none is normalised to
        // {"type":"object"}, matching what the host's own serializer emits.
        nlohmann::json InputSchema;
        // Null when the command declared no structured-result schema.
        nlohmann::json OutputSchema;
        AuthorityClass Authority = AuthorityClass::Unknown;
        // False for a command the host's exposure profile hides from `tools/list`
        // (#1124). It stays fully usable — exposure filters listing, never dispatch
        // — so oloctl surfaces every entry and only reports this in `oloctl
        // catalogue`. A CLI has no context window to protect.
        bool ListedByHost = true;
    };

    // Why one entry in the source array could not be turned into a CatalogueEntry.
    struct RejectedEntry
    {
        // The entry's index in the source array. The only handle we are guaranteed
        // to have — a malformed entry may not even carry a name.
        std::size_t Index = 0;
        std::string Name; // empty when the entry had no usable name
        std::string Reason;
    };

    struct Catalogue
    {
        std::vector<CatalogueEntry> Entries;
        // Human-readable provenance, e.g. "olo_tool_search on http://127.0.0.1:7345/mcp".
        // Printed to stderr under --verbose and carried in `oloctl catalogue`, so a
        // surprising command list can be traced to where it came from.
        std::string Source;
        // Non-empty means the host offered something this build cannot read. Never
        // silently empty-by-omission: the caller prints these to stderr.
        std::vector<RejectedEntry> Rejected;

        [[nodiscard]] const CatalogueEntry* FindByName(const std::string& name) const;
    };

    // Turn an array of command descriptions into a Catalogue. `source` is recorded
    // verbatim as provenance.
    //
    // A non-array `entries` yields an empty catalogue with one rejection saying so,
    // rather than throwing: a host that answered with the wrong shape is a
    // diagnosable condition, not a crash.
    [[nodiscard]] Catalogue ParseCatalogue(const nlohmann::json& entries, std::string source);
} // namespace OloCtl
