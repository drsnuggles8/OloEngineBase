#pragma once

// The oloctl command line: parse, resolve, invoke, print (issue #1125).
//
// Everything the CLI decides lives here, over an ICommandSource and two streams,
// so the whole surface is exercisable without a socket, a process or a temp
// file — which is what makes "adding a command to the registry makes it appear
// in the CLI" a test rather than a manual check.
//
// OUTPUT DISCIPLINE, and it is the reason this takes two streams rather than
// writing to std::cout directly: the command's payload is the ONLY thing that
// ever reaches `out`. Warnings, the connection report, the refusal message, the
// list of catalogue entries the host offered and this build could not read —
// all of it goes to `err`. `oloctl scene list-entities | jq` therefore works
// from a CI step even on a run that also warned about something.

#include "OloCtl/CommandSource.h"

#include <nlohmann/json.hpp>

#include <iosfwd>
#include <string>
#include <vector>

namespace OloCtl
{
    // Process exit codes. A script distinguishes "the editor is not running" from
    // "the command said no" from "I typed it wrong", so these are separate and
    // documented in --help.
    namespace ExitCode
    {
        inline constexpr int Ok = 0;
        inline constexpr int CommandError = 1; // the command ran and reported isError
        inline constexpr int Usage = 2;        // unknown group/command/option, bad value
        inline constexpr int Connection = 3;   // the editor could not be reached
        inline constexpr int Refused = 4;      // write path closed, or authority unknown
        inline constexpr int NoStructured = 5; // --structured, and the result carried none
    } // namespace ExitCode

    // The option names that belong to oloctl and are never passed to a command,
    // wherever they appear on the line. Exposed so the runner can WARN when a
    // command's schema declares a property of the same name instead of quietly
    // swallowing the user's argument — that argument is still settable through
    // --arguments-json.
    [[nodiscard]] bool IsReservedOption(const std::string& name);

    struct CliOptions
    {
        // Connection. Empty/zero means "not given"; the source decides what to do.
        std::string Url;
        std::string Token;
        std::string DiscoveryFile;
        int Port = 0;
        int TimeoutMs = 30000;

        // Output.
        bool Compact = false;
        bool StructuredOnly = false;
        bool Verbose = false;
        bool Help = false;
        bool Version = false;
        nlohmann::json ArgumentsBase = nlohmann::json::object();
    };

    struct ParsedCommandLine
    {
        bool Ok = false;
        CliOptions Options;
        // Everything that was not a reserved option, in order: the group and command
        // (or `call` and a registry name) followed by the command's own flags.
        std::vector<std::string> Rest;
        // The reserved options this line actually used, by the spelling that was
        // typed. Kept because a command may declare an argument of the same name:
        // the option was consumed HERE and can never reach that command, so the
        // runner must refuse rather than dispatch a call quietly missing an
        // argument the user supplied.
        std::vector<std::string> ConsumedReserved;
        std::string Error;
    };

    // Pull the reserved options out of `args` wherever they appear, leaving the
    // rest untouched and in order. A reserved option that takes a value consumes
    // the next token; use `--option=value` when a value would otherwise look like
    // an option.
    [[nodiscard]] ParsedCommandLine ParseCommandLine(const std::vector<std::string>& args);

    // Run one invocation to completion and return the process exit code. `source`
    // is only touched when the request actually needs it, so `oloctl help` on a
    // machine with no editor running still prints usage.
    [[nodiscard]] int RunCli(const ParsedCommandLine& command, ICommandSource& source, std::ostream& out,
                             std::ostream& err);
} // namespace OloCtl
