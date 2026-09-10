#pragma once

// oloctl's connection to a running editor (issue #1125).
//
// The editor already hosts a localhost-only MCP endpoint, and #1123 made every
// command reachable through it run through the same AutomationRegistry seam a
// non-MCP caller uses. So oloctl attaches to that endpoint rather than starting
// an engine: one running editor, one live scene, one set of answers, and no
// second copy of the schema knowledge — which is the failure the automation
// control plane exists to avoid.
//
// Driving a HEADLESS host is out of scope here and is its own epic (#1132),
// still blocked on the command registry + event bus (#1131).
//
// FINDING THE EDITOR. The editor writes a discovery file with its host, port and
// bearer token (McpServer::DiscoveryFilePath). oloctl reads it. When several
// editors are running — the normal state in a worktree-per-task setup — it
// reports every candidate and stops, rather than picking one: attaching to the
// wrong editor answers every question about the wrong scene, and looks exactly
// like a correct answer.

#include "OloCtl/CommandCatalogue.h"
#include "OloCtl/CommandSource.h"

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace OloCtl
{
    struct ConnectionRequest
    {
        std::string Url; // explicit endpoint; needs Token
        std::string Token;
        std::string DiscoveryFile; // explicit discovery file
        int Port = 0;              // narrow the discovery search to one port
        int TimeoutMs = 30000;
    };

    struct Endpoint
    {
        std::string Url;
        std::string Token;
        std::string Origin; // how it was found, for --verbose and error messages
    };

    // Resolve `request` to one endpoint, or fail with a sentence saying what was
    // searched and what was found. Never picks between candidates.
    [[nodiscard]] std::optional<Endpoint> ResolveEndpoint(const ConnectionRequest& request, std::string& outError);

    class McpHttpCommandSource final : public ICommandSource
    {
      public:
        // `err` receives connection diagnostics: which editor was attached to, and
        // any degradation in how the catalogue had to be obtained.
        McpHttpCommandSource(ConnectionRequest request, std::ostream& err, bool verbose);
        ~McpHttpCommandSource() override;

        [[nodiscard]] const Catalogue* FetchCatalogue(std::string& outError) override;
        [[nodiscard]] InvokeOutcome Invoke(const std::string& name, const nlohmann::json& arguments) override;

      private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace OloCtl
