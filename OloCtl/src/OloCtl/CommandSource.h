#pragma once

// Where the commands come from, and where a call goes (issue #1125).
//
// oloctl's whole dependency on the outside world is these two operations. The
// production implementation reaches a running editor over the MCP endpoint
// (McpHttpCommandSource); the registry implementation runs against an
// AutomationRegistry in the same process (RegistryCommandSource.h), which is how
// a test proves that registering a command makes it appear in the CLI without
// standing up a socket.
//
// NOTE WHAT IS NOT HERE: there is no way to assert write consent. That is not an
// omission to be filled in later by adding a parameter — a CLI has no standing
// to grant consent on the user's behalf, and the registry default-denies
// precisely so a caller that has not thought about it cannot mutate anything.
// Write support means routing through the editor's consent gate, and it changes
// this interface when it lands (see AutomationWriteConsent).

#include "OloCtl/CommandCatalogue.h"

#include <nlohmann/json.hpp>

#include <string>

namespace OloCtl
{
    class ICommandSource
    {
      public:
        ICommandSource() = default;
        virtual ~ICommandSource() = default;

        ICommandSource(const ICommandSource&) = delete;
        ICommandSource& operator=(const ICommandSource&) = delete;
        ICommandSource(ICommandSource&&) = delete;
        ICommandSource& operator=(ICommandSource&&) = delete;

        // The whole registered surface, not a profile-filtered view of it. Returns
        // nullptr with `outError` set — as a sentence, for stderr — when the host
        // cannot be reached or answered with something unusable.
        //
        // LIFETIME: the catalogue is owned by the source and stays valid until the
        // NEXT call to FetchCatalogue, which may replace it — a registry can swap
        // its command list while a frontend holds it (script-command live reload,
        // #607), so an implementation that re-reads is honest rather than broken.
        // Anything derived from a catalogue (a CommandTree holds indices into it)
        // is invalidated at the same moment.
        [[nodiscard]] virtual const Catalogue* FetchCatalogue(std::string& outError) = 0;

        struct InvokeOutcome
        {
            // False for a TRANSPORT failure: the call never reached a handler. A
            // command that ran and returned an error is Ok with Result.isError true,
            // because those are different things to a script reading the exit code.
            bool Ok = false;
            // The host's tools/call `result` object: { content, isError,
            // structuredContent? }. Printed to stdout verbatim.
            nlohmann::json Result;
            std::string Error;
        };

        [[nodiscard]] virtual InvokeOutcome Invoke(const std::string& name, const nlohmann::json& arguments) = 0;
    };
} // namespace OloCtl
