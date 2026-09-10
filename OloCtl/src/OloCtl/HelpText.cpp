#include "OloCtl/HelpText.h"

#include "OloCtl/ArgumentBinder.h"

#include <algorithm>
#include <sstream>

namespace OloCtl
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr const char* kUsage =
            "oloctl - a command-line frontend over a running OloEngine editor's automation registry.\n"
            "\n"
            "Usage:\n"
            "  oloctl [options] <group> <command> [--option value ...]\n"
            "  oloctl [options] call <registry-name> [--option value ...]\n"
            "  oloctl [options] catalogue\n"
            "  oloctl [options] <group>\n"
            "  oloctl help | version\n"
            "\n"
            "The command tree is GENERATED from the editor's registry on every run: oloctl carries no\n"
            "list of commands, so a command added to the registry appears here with no change to this\n"
            "tool. `call` addresses a command by its registry name and always works, including for a\n"
            "name the tree could not give an unambiguous spelling.\n"
            "\n"
            "Connection (an OloEditor must be running with its MCP diagnostics server started):\n"
            "  --url <url>              MCP endpoint, e.g. http://127.0.0.1:7345/mcp. Needs a token:\n"
            "                           --token, or OLOCTL_TOKEN for a loopback URL.\n"
            "  --token <token>          Bearer token, as shown in the editor's MCP Server panel.\n"
            "                           OLOCTL_TOKEN supplies it instead, keeping it out of argv\n"
            "                           and shell history; --token wins when both are set, and\n"
            "                           OLOCTL_TOKEN is only attached to a loopback --url.\n"
            "                           --token without --url is an error: a token alone names\n"
            "                           no editor.\n"
            "  --port <n>               Use the discovery file for this port.\n"
            "  --discovery-file <path>  Read host/port/token from this file.\n"
            "  --timeout <ms>           Per-request timeout. Default 30000.\n"
            "\n"
            "With none of those, oloctl looks for exactly one editor discovery file in the system temp\n"
            "directory and reports every candidate if there is more than one. It never picks for you.\n"
            "\n"
            "Output:\n"
            "  --json                   Print the host's result object. The default.\n"
            "  --structured             Print only `structuredContent`.\n"
            "  --compact                One line instead of indented.\n"
            "  --arguments-json <json>  Seed the argument object; named options override its keys.\n"
            "  --verbose                Report the connection and the catalogue on stderr.\n"
            "\n"
            "The payload goes to stdout and nothing else ever does, so `oloctl ... | jq` works from a\n"
            "CI step. Diagnostics, warnings and errors go to stderr.\n"
            "\n"
            "Values are typed by the command's declared schema: --count 3 sends a number, --name x a\n"
            "string. An option whose schema declares no type takes JSON if the text parses as JSON and\n"
            "the raw string otherwise. Repeat an option to build an array.\n"
            "\n"
            "Writes are CLOSED, not merely unimplemented: a command that mutates the project is refused\n"
            "before a request is built (exit 4). Write support is a later pass and will go through the\n"
            "same consent machinery as MCP, never a CLI-specific bypass.\n"
            "\n"
            "oloctl resolves nothing against the working directory. A path in an argument is resolved\n"
            "by the EDITOR, relative to the editor's own working directory (OloEditor/).\n"
            "\n"
            "Exit codes: 0 ok - 1 the command reported an error - 2 usage - 3 cannot reach the editor\n"
            "  - 4 refused (write path closed) - 5 no structured content in the result.\n";

        std::string FirstSentence(const std::string& description, std::size_t limit)
        {
            std::string text = description;
            if (const std::size_t newline = text.find('\n'); newline != std::string::npos)
                text = text.substr(0, newline);
            if (const std::size_t stop = text.find(". "); stop != std::string::npos && stop + 1 < limit)
                return text.substr(0, stop + 1);
            if (text.size() <= limit)
                return text;
            std::size_t cut = limit;
            if (const std::size_t space = text.rfind(' ', cut); space != std::string::npos && space + 20 > limit)
                cut = space;
            // Never split a UTF-8 sequence: continuation bytes are 10xxxxxx.
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                --cut;
            return text.substr(0, cut) + "...";
        }

        void PadTo(std::ostringstream& out, std::size_t written, std::size_t column)
        {
            out << std::string(written >= column ? 1 : column - written, ' ');
        }

        std::string TypeLabel(const Json& schema)
        {
            if (!schema.is_object())
                return "value";
            const auto type = schema.find("type");
            if (type == schema.end())
                return "value";
            if (type->is_array())
            {
                std::string label;
                for (const Json& element : *type)
                {
                    if (!element.is_string())
                        continue;
                    if (!label.empty())
                        label += '|';
                    label += element.get<std::string>();
                }
                return label.empty() ? "value" : label;
            }
            if (!type->is_string())
                return "value";
            std::string label = type->get<std::string>();
            if (label == "array")
            {
                const auto items = schema.find("items");
                if (items != schema.end() && items->is_object())
                    label += '[' + TypeLabel(*items) + ']';
            }
            return label;
        }

        bool IsRequired(const Json& inputSchema, const std::string& name)
        {
            const auto required = inputSchema.find("required");
            if (required == inputSchema.end() || !required->is_array())
                return false;
            return std::any_of(required->begin(), required->end(),
                               [&name](const Json& element)
                               { return element.is_string() && element.get<std::string>() == name; });
        }

        void AppendConstraints(std::ostringstream& out, const Json& schema)
        {
            if (!schema.is_object())
                return;
            const auto enumeration = schema.find("enum");
            if (enumeration != schema.end() && enumeration->is_array() && !enumeration->empty())
            {
                out << " one of: ";
                for (std::size_t i = 0; i < enumeration->size(); ++i)
                {
                    if (i != 0)
                        out << ", ";
                    const Json& value = (*enumeration)[i];
                    out << (value.is_string() ? value.get<std::string>() : value.dump());
                }
                out << '.';
            }
            const auto minimum = schema.find("minimum");
            const auto maximum = schema.find("maximum");
            if (minimum != schema.end() && maximum != schema.end())
                out << " range " << minimum->dump() << "-" << maximum->dump() << '.';
            else if (minimum != schema.end())
                out << " minimum " << minimum->dump() << '.';
            else if (maximum != schema.end())
                out << " maximum " << maximum->dump() << '.';
            const auto fallback = schema.find("default");
            if (fallback != schema.end())
                out << " Default " << fallback->dump() << '.';
        }
    } // namespace

    std::string RenderOfflineHelp(const std::string& connectionError)
    {
        std::ostringstream out;
        out << kUsage << '\n';
        out << "The command tree is not shown: " << connectionError << '\n';
        out << "Start the editor and its MCP Server panel, then try again.\n";
        return out.str();
    }

    std::string RenderRootHelp(const Catalogue& catalogue, const CommandTree& tree)
    {
        std::ostringstream out;
        out << kUsage << '\n';
        out << "Command groups (" << catalogue.Entries.size() << " commands from " << catalogue.Source << "):\n";
        for (const TreeGroup& group : tree.Groups())
        {
            out << "  " << group.Name;
            PadTo(out, group.Name.size() + 2, 22);
            out << group.Leaves.size() << (group.Leaves.size() == 1 ? " command" : " commands") << '\n';
        }
        out << "\nRun `oloctl <group>` to list its commands, or `oloctl <group> <command> --help`.\n";
        return out.str();
    }

    std::string RenderGroupHelp(const Catalogue& catalogue, const TreeGroup& group)
    {
        std::ostringstream out;
        out << "oloctl " << group.Name << " - " << group.Leaves.size()
            << (group.Leaves.size() == 1 ? " command\n\n" : " commands\n\n");
        for (const TreeLeaf& leaf : group.Leaves)
        {
            const CatalogueEntry& entry = catalogue.Entries[leaf.EntryIndex];
            out << "  " << leaf.Command;
            PadTo(out, leaf.Command.size() + 2, 30);
            out << FirstSentence(entry.Title.empty() ? entry.Description : entry.Title, 90);
            if (entry.Authority == AuthorityClass::ProjectWrite)
                out << "  [write - refused]";
            else if (entry.Authority == AuthorityClass::Unknown)
                out << "  [authority unknown - refused]";
            out << '\n';
        }
        out << "\nRun `oloctl " << group.Name << " <command> --help` for one command's arguments.\n";
        return out.str();
    }

    std::string RenderCommandHelp(const CatalogueEntry& entry, const DerivedPath& path)
    {
        std::ostringstream out;
        out << "oloctl " << path.Group << ' ' << path.Command;
        if (!entry.Title.empty())
            out << " - " << entry.Title;
        out << "\n\nRegistry name: " << entry.Name << "  (oloctl call " << entry.Name << " ...)\n";
        out << "Authority:     " << ToString(entry.Authority);
        if (entry.Authority != AuthorityClass::ReadOnly)
            out << " - refused by oloctl; this frontend is read-only";
        out << '\n';
        if (!entry.Toolset.empty())
            out << "Toolset:       " << entry.Toolset << '\n';
        if (!entry.ListedByHost)
            out << "Listing:       hidden from the editor's tools/list under its exposure profile, "
                   "which does not affect this CLI\n";

        if (!entry.Description.empty())
            out << '\n'
                << entry.Description << '\n';

        const auto properties = entry.InputSchema.find("properties");
        if (properties == entry.InputSchema.end() || !properties->is_object() || properties->empty())
        {
            out << "\nTakes no arguments.\n";
            return out.str();
        }

        out << "\nArguments:\n";
        for (const auto& [name, schema] : properties->items())
        {
            const std::string flag = "--" + FlagSpelling(name) + ' ' + '<' + TypeLabel(schema) + '>';
            out << "  " << flag;
            PadTo(out, flag.size() + 2, 34);
            if (IsRequired(entry.InputSchema, name))
                out << "(required) ";
            if (schema.is_object())
            {
                const auto description = schema.find("description");
                if (description != schema.end() && description->is_string())
                    out << description->get<std::string>();
            }
            AppendConstraints(out, schema);
            out << '\n';
        }

        if (entry.OutputSchema.is_object() && !entry.OutputSchema.empty())
            out << "\nReturns a structured payload; `--structured` prints just that object.\n";
        return out.str();
    }
} // namespace OloCtl
