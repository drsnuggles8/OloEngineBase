#pragma once

// oloctl over an in-process registry (issue #1125).
//
// The same CLI, with the socket taken out: the catalogue comes straight from an
// AutomationRegistry and an invocation goes straight through
// AutomationRegistry::Invoke. Both halves are the production ones —
// Automation::DescribeCatalogue is what the editor's own catalogue surface
// emits, and OloCtl::ParseCatalogue is what the HTTP source feeds — so this is
// the two ends of the live path joined without the wire in between, not a
// re-implementation of either.
//
// WHY IT EXISTS. It is how "registering a command makes it appear in the CLI"
// becomes an assertion rather than a manual check: a test registers one command
// in a bare registry and runs the real RunCli against it. It is also the shape a
// headless host would use, which is the direction #1132 goes once it unblocks.
//
// Header-only: the oloctl EXECUTABLE never includes it (it links nothing from
// the engine tree), while a caller that already has a registry compiled in pays
// nothing to gain a CLI over it.
//
// THE WRITE PATH IS CLOSED HERE TOO, and structurally: Invoke() calls
// AutomationRegistry::Invoke with the default consent, which is Withheld, and
// there is no parameter anywhere on this class that could change that. A
// ProjectWrite command reaching this far — it should not; CliRunner refuses one
// before binding its arguments — is refused a second time by the registry.

#include "Automation/AutomationCatalogue.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationResult.h"

#include "OloCtl/CommandCatalogue.h"
#include "OloCtl/CommandSource.h"

#include <optional>
#include <string>
#include <utility>

namespace OloCtl
{
    class RegistryCommandSource final : public ICommandSource
    {
      public:
        // `registry` and `host` must outlive this source.
        RegistryCommandSource(const OloEngine::Automation::AutomationRegistry& registry,
                              OloEngine::Automation::IAutomationHost& host, std::string source)
            : m_Registry(registry), m_Host(host), m_Source(std::move(source))
        {
        }

        [[nodiscard]] const Catalogue* FetchCatalogue(std::string& /*outError*/) override
        {
            // Re-read on every call rather than caching: a registry can swap its
            // command list while a caller holds it (script-tool live reload), and a
            // frontend that cached would keep offering commands that are gone.
            m_Catalogue = ParseCatalogue(OloEngine::Automation::DescribeCatalogue(*m_Registry.Snapshot()), m_Source);
            return &*m_Catalogue;
        }

        [[nodiscard]] InvokeOutcome Invoke(const std::string& name, const nlohmann::json& arguments) override
        {
            using OloEngine::Automation::AutomationCommand;
            using OloEngine::Automation::AutomationInvocation;
            using OloEngine::Automation::AutomationResult;

            InvokeOutcome outcome;
            // ONE snapshot for the invocation AND for the shaping decision below. A
            // second snapshot taken afterwards could let a command-list swap between
            // them shape the result by a different command's declaration than the one
            // that actually ran.
            const OloEngine::Automation::AutomationRegistry::CommandSnapshot snapshot = m_Registry.Snapshot();
            const AutomationInvocation invocation = m_Registry.Invoke(snapshot, m_Host, name, arguments);

            // How a refusal is reported is the transport's business
            // (AutomationInvocation's contract), and the shape that keeps this
            // frontend in step with the MCP one is the MCP adapter's:
            //
            //   InvalidArguments -> a COMMAND-LEVEL error result, isError true. The
            //     spec's SEP-1303 rule, and the reason HandleToolsCall does the same:
            //     the message is for whoever wrote the arguments, and a protocol error
            //     is often swallowed by a client shim before it gets there. The
            //     registry composes the message in exactly HandleToolsCall's wording,
            //     so the two paths print the same bytes for the same mistake.
            //   everything else -> the call never reached a handler. There is no
            //     result to print and no isError to read, so the CLI reports a
            //     failure rather than inventing an empty payload.
            if (invocation.Outcome == AutomationInvocation::Status::InvalidArguments)
            {
                outcome.Ok = true;
                outcome.Result = OloEngine::Automation::DescribeResult(
                    OloEngine::Automation::AutomationResult::Error(invocation.Message));
                return outcome;
            }
            if (!invocation.Ran())
            {
                outcome.Error = invocation.Message;
                return outcome;
            }
            // The dual-audience re-shape a command may have declared (#673). Applied
            // here, not left to the transport, so this source and the MCP one return
            // the same `content` for the same command -- which is what lets the
            // acceptance test use this source as a stand-in for the live path.
            // Session-level policy the transport adds on top (path redaction) is
            // deliberately NOT reproduced: it belongs to a session, not to a result.
            AutomationResult shaped = invocation.Result;
            if (const AutomationCommand* command =
                    OloEngine::Automation::AutomationRegistry::Find(*snapshot, name))
            {
                OloEngine::Automation::ApplyDualAudienceContent(*command, shaped);
            }

            outcome.Ok = true;
            outcome.Result = OloEngine::Automation::DescribeResult(std::move(shaped));
            return outcome;
        }

      private:
        const OloEngine::Automation::AutomationRegistry& m_Registry;
        OloEngine::Automation::IAutomationHost& m_Host;
        std::string m_Source;
        std::optional<Catalogue> m_Catalogue;
    };
} // namespace OloCtl
