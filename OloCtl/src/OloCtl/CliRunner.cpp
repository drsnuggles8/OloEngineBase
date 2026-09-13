#include "OloCtl/CliRunner.h"

#include "OloCtl/ArgumentBinder.h"
#include "OloCtl/CommandTree.h"
#include "OloCtl/EventsFollow.h"
#include "OloCtl/HelpText.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <ostream>
#include <utility>

namespace OloCtl
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr const char* kVersion = "oloctl 0.1.0 (OloEngine automation control plane, issue #1125)";

        // Reserved option -> does it take a value?
        struct ReservedOption
        {
            const char* Name;
            bool TakesValue;
        };

        constexpr std::array kReservedOptions{
            ReservedOption{ "url", true },
            ReservedOption{ "token", true },
            ReservedOption{ "port", true },
            ReservedOption{ "discovery-file", true },
            ReservedOption{ "timeout", true },
            ReservedOption{ "arguments-json", true },
            ReservedOption{ "json", false },
            ReservedOption{ "structured", false },
            ReservedOption{ "compact", false },
            ReservedOption{ "verbose", false },
            ReservedOption{ "help", false },
            ReservedOption{ "version", false },
        };

        const ReservedOption* FindReserved(const std::string& name)
        {
            const auto it = std::find_if(kReservedOptions.begin(), kReservedOptions.end(),
                                         [&name](const ReservedOption& option)
                                         { return name == option.Name; });
            return it == kReservedOptions.end() ? nullptr : &*it;
        }

        bool ParsePositiveInt(const std::string& text, int& out)
        {
            errno = 0;
            char* end = nullptr;
            const long value = std::strtol(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0' || errno == ERANGE || value <= 0 || value > 2147483647L)
                return false;
            out = static_cast<int>(value);
            return true;
        }

        bool ApplyReserved(const ReservedOption& option, const std::string& value, CliOptions& options,
                           std::string& outError)
        {
            const std::string name = option.Name;
            if (name == "url")
                options.Url = value;
            else if (name == "token")
                options.Token = value;
            else if (name == "discovery-file")
                options.DiscoveryFile = value;
            else if (name == "port")
            {
                if (!ParsePositiveInt(value, options.Port) || options.Port > 65535)
                {
                    outError = "--port expects a TCP port in 1-65535, got '" + value + "'.";
                    return false;
                }
            }
            else if (name == "timeout")
            {
                if (!ParsePositiveInt(value, options.TimeoutMs))
                {
                    outError = "--timeout expects a positive number of milliseconds, got '" + value + "'.";
                    return false;
                }
            }
            else if (name == "arguments-json")
            {
                Json parsed = Json::parse(value, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object())
                {
                    outError = "--arguments-json expects a JSON object, got '" + value + "'.";
                    return false;
                }
                options.ArgumentsBase = std::move(parsed);
            }
            else if (name == "json")
            {
                // The default. Accepted so a script can say what it means, and because
                // the issue's acceptance criterion is spelled with it.
                options.StructuredOnly = false;
            }
            else if (name == "structured")
                options.StructuredOnly = true;
            else if (name == "compact")
                options.Compact = true;
            else if (name == "verbose")
                options.Verbose = true;
            else if (name == "help")
                options.Help = true;
            else if (name == "version")
                options.Version = true;
            return true;
        }

        void Print(std::ostream& out, const Json& payload, bool compact)
        {
            out << (compact ? payload.dump() : payload.dump(2)) << '\n';
        }

        void ReportCatalogue(std::ostream& err, const Catalogue& catalogue, const CommandTree& tree, bool verbose)
        {
            // Rejections and ambiguity are reported on EVERY run, not only under
            // --verbose and not only when one is hit: both mean a registered command
            // is hard or impossible to reach from here, and a user cannot see that by
            // looking at the CLI.
            for (const RejectedEntry& rejected : catalogue.Rejected)
            {
                err << "oloctl: ignoring catalogue entry " << rejected.Index;
                if (!rejected.Name.empty())
                    err << " (" << rejected.Name << ')';
                err << ": " << rejected.Reason << '\n';
            }
            for (const std::string& ambiguous : tree.AmbiguousPaths())
            {
                err << "oloctl: `" << ambiguous
                    << "` is ambiguous - more than one registered command derives that spelling. "
                       "Use `oloctl call <registry-name>`.\n";
            }
            // A group whose name is one of oloctl's own subcommands can never be
            // reached by typing it, so say so rather than letting it look absent.
            for (const TreeGroup& group : tree.Groups())
            {
                if (group.Name != "help" && group.Name != "version" && group.Name != "catalogue" &&
                    group.Name != "call" && group.Name != "events")
                {
                    continue;
                }
                err << "oloctl: the group `" << group.Name
                    << "` shares its name with an oloctl subcommand and cannot be reached that way. Its "
                    << group.Leaves.size()
                    << " command(s) are still callable with `oloctl call <registry-name>`.\n";
            }
            if (verbose)
            {
                err << "oloctl: " << catalogue.Entries.size() << " commands in " << tree.Groups().size()
                    << " groups from " << catalogue.Source << '\n';
            }
        }

        // A command argument whose flag spelling oloctl has taken for itself.
        //
        // ParseCommandLine has already eaten such an option, so it can never reach
        // the command. If the user actually TYPED it, dispatching anyway would run
        // the command with an argument silently missing and exit 0 - a wrong answer
        // that looks like a right one - so that case is refused. If they did not, a
        // warning is enough: they need to know the flag is unavailable before they
        // reach for it.
        //
        // Returns false when the invocation must not proceed.
        bool CheckReservedCollisions(std::ostream& err, const CatalogueEntry& entry,
                                     const std::vector<std::string>& consumed)
        {
            const auto properties = entry.InputSchema.find("properties");
            if (properties == entry.InputSchema.end() || !properties->is_object())
                return true;

            bool ok = true;
            for (const auto& [name, schema] : properties->items())
            {
                const std::string flag = FlagSpelling(name);
                if (!IsReservedOption(flag) && !IsReservedOption(name))
                    continue;
                const bool typed = std::find(consumed.begin(), consumed.end(), flag) != consumed.end() ||
                                   std::find(consumed.begin(), consumed.end(), name) != consumed.end();
                err << "oloctl: " << entry.Name << " declares an argument `" << name << "`, and `--" << flag
                    << "` is an oloctl option, so it cannot be given that way"
                    << (typed ? " - and you gave it" : "") << ". Set it with --arguments-json '{\"" << name
                    << "\": ...}'.\n";
                ok = ok && !typed;
            }
            return ok;
        }

        int InvokeAndPrint(const CatalogueEntry& entry, const CliOptions& options,
                           const std::vector<std::string>& flags, const std::vector<std::string>& consumedReserved,
                           ICommandSource& source, std::ostream& out, std::ostream& err)
        {
            // THE WRITE GATE. It sits before the arguments are even bound, so nothing
            // about a project-mutating command reaches the host: no request is built,
            // and there is no code path from here that could assert consent. See
            // ICommandSource for why that is structural rather than a check to relax.
            if (entry.Authority == AuthorityClass::ProjectWrite)
            {
                err << "oloctl: " << entry.Name
                    << " mutates the project, and oloctl is read-only. Writes land in a later pass and will go "
                       "through the editor's write-consent gate, the same one an MCP tools/call goes through - "
                       "not a CLI-specific bypass. Use the editor, or an MCP client with consent enabled.\n";
                return ExitCode::Refused;
            }
            if (entry.Authority == AuthorityClass::Unknown)
            {
                err << "oloctl: " << entry.Name
                    << " cannot be run: this editor's catalogue does not report whether the command mutates the "
                       "project, and guessing would be a way around the consent model. The editor is older than "
                       "the `projectWrite` field; rebuild it, or run the command through the editor's MCP "
                       "endpoint.\n";
                return ExitCode::Refused;
            }

            if (!CheckReservedCollisions(err, entry, consumedReserved))
                return ExitCode::Usage;

            const BindResult bound = BindArguments(entry.InputSchema, flags, options.ArgumentsBase);
            if (!bound.Ok)
            {
                err << "oloctl: " << bound.Error << "\nRun `oloctl call " << entry.Name << " --help`.\n";
                return ExitCode::Usage;
            }

            if (options.Verbose)
                err << "oloctl: invoking " << entry.Name << ' ' << bound.Arguments.dump() << '\n';

            const ICommandSource::InvokeOutcome outcome = source.Invoke(entry.Name, bound.Arguments);
            if (!outcome.Ok)
            {
                err << "oloctl: " << outcome.Error << '\n';
                return ExitCode::Connection;
            }

            const auto isError = outcome.Result.find("isError");
            const bool failed = isError != outcome.Result.end() && isError->is_boolean() && isError->get<bool>();

            if (options.StructuredOnly)
            {
                // A FAILED command carries its message in `content` and has no
                // `structuredContent`, so the two conditions must be told apart before
                // either is reported. Answering "no structuredContent" here would
                // discard the host's explanation of what went wrong AND hand back exit
                // 5, which says nothing failed.
                if (failed)
                {
                    err << "oloctl: " << entry.Name << " failed: "
                        << outcome.Result.value("content", Json::array()).dump() << '\n';
                    return ExitCode::CommandError;
                }
                const auto structured = outcome.Result.find("structuredContent");
                if (structured == outcome.Result.end())
                {
                    err << "oloctl: " << entry.Name
                        << " returned no structuredContent. Drop --structured to see the text result.\n";
                    return ExitCode::NoStructured;
                }
                Print(out, *structured, options.Compact);
                return ExitCode::Ok;
            }

            Print(out, outcome.Result, options.Compact);
            return failed ? ExitCode::CommandError : ExitCode::Ok;
        }

        Json BuildCatalogueDump(const Catalogue& catalogue)
        {
            Json commands = Json::array();
            for (const CatalogueEntry& entry : catalogue.Entries)
            {
                const DerivedPath path = DerivePath(entry);
                Json command{ { "name", entry.Name },
                              { "group", path.Group },
                              { "command", path.Command },
                              { "description", entry.Description },
                              { "authority", ToString(entry.Authority) },
                              { "runnable", entry.Authority == AuthorityClass::ReadOnly },
                              { "listedByHost", entry.ListedByHost },
                              { "inputSchema", entry.InputSchema } };
                if (!entry.Title.empty())
                    command["title"] = entry.Title;
                if (!entry.Toolset.empty())
                    command["toolset"] = entry.Toolset;
                if (!entry.OutputSchema.is_null())
                    command["outputSchema"] = entry.OutputSchema;
                commands.push_back(std::move(command));
            }

            Json rejected = Json::array();
            for (const RejectedEntry& entry : catalogue.Rejected)
            {
                rejected.push_back(
                    Json{ { "index", entry.Index }, { "name", entry.Name }, { "reason", entry.Reason } });
            }

            return Json{ { "source", catalogue.Source },
                         { "commandCount", catalogue.Entries.size() },
                         { "commands", std::move(commands) },
                         { "rejected", std::move(rejected) } };
        }
    } // namespace

    bool IsReservedOption(const std::string& name)
    {
        return FindReserved(name) != nullptr;
    }

    ParsedCommandLine ParseCommandLine(const std::vector<std::string>& args)
    {
        ParsedCommandLine parsed;
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            const std::string& token = args[i];
            if (token.size() < 3 || token.compare(0, 2, "--") != 0)
            {
                parsed.Rest.push_back(token);
                continue;
            }

            std::string name = token.substr(2);
            std::string value;
            bool hasValue = false;
            if (const std::size_t equals = name.find('='); equals != std::string::npos)
            {
                value = name.substr(equals + 1);
                name = name.substr(0, equals);
                hasValue = true;
            }

            const ReservedOption* option = FindReserved(name);
            if (option == nullptr)
            {
                // Not ours; hand it to the command untouched, in its original spelling.
                parsed.Rest.push_back(token);
                continue;
            }

            if (option->TakesValue && !hasValue)
            {
                if (i + 1 >= args.size())
                {
                    parsed.Error = "--" + name + " expects a value.";
                    return parsed;
                }
                value = args[++i];
            }
            if (!option->TakesValue && hasValue)
            {
                parsed.Error = "--" + name + " takes no value.";
                return parsed;
            }
            if (!ApplyReserved(*option, value, parsed.Options, parsed.Error))
                return parsed;
            if (std::find(parsed.ConsumedReserved.begin(), parsed.ConsumedReserved.end(), name) ==
                parsed.ConsumedReserved.end())
            {
                parsed.ConsumedReserved.push_back(name);
            }
        }

        parsed.Ok = true;
        return parsed;
    }

    int RunCli(const ParsedCommandLine& command, ICommandSource& source, std::ostream& out, std::ostream& err)
    {
        if (!command.Ok)
        {
            err << "oloctl: " << command.Error << "\nRun `oloctl help`.\n";
            return ExitCode::Usage;
        }

        const CliOptions& options = command.Options;
        const std::vector<std::string>& rest = command.Rest;

        if (options.Version || (!rest.empty() && rest[0] == "version"))
        {
            out << kVersion << '\n';
            return ExitCode::Ok;
        }

        // `events follow` needs no command tree - it loops one registry name it
        // knows - so it is dispatched before the catalogue fetch, like `version`.
        // An editor whose catalogue cannot be read can still be followed, and the
        // fetch's cost is not paid on every follower start.
        if (!rest.empty() && rest[0] == "events")
            return RunEventsVerb(options, rest, source, out, err);

        const bool wantsRootHelp = rest.empty() || rest[0] == "help";
        const bool noArguments = rest.empty() && !options.Help;

        // Everything below needs the catalogue, because everything below is derived
        // from it. Root help is the one thing that still means something without one.
        std::string connectionError;
        const Catalogue* catalogue = source.FetchCatalogue(connectionError);
        if (catalogue == nullptr)
        {
            if (wantsRootHelp)
            {
                std::ostream& destination = noArguments ? err : out;
                destination << RenderOfflineHelp(connectionError);
                return noArguments ? ExitCode::Usage : ExitCode::Ok;
            }
            err << "oloctl: " << connectionError << '\n';
            return ExitCode::Connection;
        }

        const CommandTree tree = CommandTree::Build(*catalogue);
        ReportCatalogue(err, *catalogue, tree, options.Verbose);

        if (wantsRootHelp)
        {
            std::ostream& destination = noArguments ? err : out;
            destination << RenderRootHelp(*catalogue, tree);
            return noArguments ? ExitCode::Usage : ExitCode::Ok;
        }

        if (rest[0] == "catalogue")
        {
            if (rest.size() > 1)
            {
                err << "oloctl: `catalogue` takes no arguments, got '" << rest[1] << "'.\n";
                return ExitCode::Usage;
            }
            Print(out, BuildCatalogueDump(*catalogue), options.Compact);
            return ExitCode::Ok;
        }

        if (rest[0] == "call")
        {
            if (rest.size() < 2)
            {
                err << "oloctl: `call` needs a registry name, e.g. `oloctl call olo_scene_summary`. "
                       "`oloctl catalogue` lists every name.\n";
                return ExitCode::Usage;
            }
            const CatalogueEntry* entry = catalogue->FindByName(rest[1]);
            if (entry == nullptr)
            {
                err << "oloctl: no command named '" << rest[1] << "' is registered. `oloctl catalogue` lists "
                    << catalogue->Entries.size() << " names.\n";
                return ExitCode::Usage;
            }
            if (options.Help)
            {
                out << RenderCommandHelp(*entry, DerivePath(*entry));
                return ExitCode::Ok;
            }
            return InvokeAndPrint(*entry, options, { rest.begin() + 2, rest.end() }, command.ConsumedReserved,
                                  source, out, err);
        }

        const TreeGroup* group = tree.FindGroup(rest[0]);
        if (group == nullptr)
        {
            err << "oloctl: no command group '" << rest[0] << "'. Groups:";
            for (const TreeGroup& candidate : tree.Groups())
                err << ' ' << candidate.Name;
            err << ".\n";
            return ExitCode::Usage;
        }

        if (rest.size() < 2)
        {
            out << RenderGroupHelp(*catalogue, *group);
            return ExitCode::Ok;
        }

        const CommandTree::Resolution resolution = tree.Resolve(rest[0], rest[1]);
        switch (resolution.Status)
        {
            case CommandTree::ResolveStatus::UnknownCommand:
                err << "oloctl: no command '" << rest[1] << "' in group '" << rest[0]
                    << "'. Run `oloctl " << rest[0] << "` for the list.\n";
                return ExitCode::Usage;
            case CommandTree::ResolveStatus::Ambiguous:
                err << "oloctl: `" << rest[0] << ' ' << rest[1] << "` matches";
                for (const std::string& candidate : resolution.Candidates)
                    err << ' ' << candidate;
                err << ". Use `oloctl call <registry-name>`.\n";
                return ExitCode::Usage;
            case CommandTree::ResolveStatus::UnknownGroup:
                // FindGroup already succeeded, so this cannot happen; reported rather
                // than assumed away.
                err << "oloctl: internal error - group '" << rest[0] << "' vanished between lookup and resolve.\n";
                return ExitCode::Usage;
            case CommandTree::ResolveStatus::Ok:
                break;
        }

        const CatalogueEntry& entry = catalogue->Entries[resolution.EntryIndex];
        if (options.Help)
        {
            out << RenderCommandHelp(entry, DerivePath(entry));
            return ExitCode::Ok;
        }
        return InvokeAndPrint(entry, options, { rest.begin() + 2, rest.end() }, command.ConsumedReserved, source,
                              out, err);
    }
} // namespace OloCtl
