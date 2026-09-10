// httplib.h must precede anything that could pull in <windows.h>, so that
// <winsock2.h> wins the include race on Windows — the same ordering rule
// McpServer.cpp states at its own top.
#include <httplib.h>

#include "OloCtl/McpHttpCommandSource.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace OloCtl
{
    namespace
    {
        using Json = nlohmann::json;

        // The port McpServer binds when the host names none. A discovery file for
        // this port keeps the legacy unnumbered name.
        constexpr int kDefaultPort = 7345;
        constexpr const char* kLegacyDiscoveryName = "oloengine-mcp.json";
        constexpr const char* kDiscoveryPrefix = "oloengine-mcp-";

        // The newest revision McpServer::kSupportedProtocolVersions accepts. Sent as
        // the requested version in `initialize`; whatever the server negotiates back
        // is what gets stamped on later requests, so a server that only speaks an
        // older revision still works.
        constexpr const char* kRequestedProtocolVersion = "2025-11-25";

        std::optional<std::string> EnvVar(const char* name)
        {
#if defined(_WIN32)
            char* buffer = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr)
                return std::nullopt;
            std::string value(buffer);
            std::free(buffer);
            if (value.empty())
                return std::nullopt;
            return value;
#else
            const char* value = std::getenv(name);
            if (value == nullptr || *value == '\0')
                return std::nullopt;
            return std::string(value);
#endif
        }

        std::filesystem::path DiscoveryPathForPort(const std::filesystem::path& directory, int port)
        {
            if (port == kDefaultPort)
                return directory / kLegacyDiscoveryName;
            return directory / (std::string(kDiscoveryPrefix) + std::to_string(port) + ".json");
        }

        // Read one discovery file. Fails rather than half-succeeds: the editor writes
        // it with a plain ofstream, so a read can land mid-write and see truncated
        // JSON, and a half-read token would produce a 401 that looks like the wrong
        // token rather than a race.
        bool ReadDiscoveryFile(const std::filesystem::path& path, Endpoint& out, std::string& outError)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                outError = "cannot read " + path.string();
                return false;
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();

            Json parsed = Json::parse(buffer.str(), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                outError = path.string() + " is not valid JSON (the editor may be writing it right now; retry)";
                return false;
            }
            const auto url = parsed.find("url");
            const auto token = parsed.find("token");
            if (url == parsed.end() || !url->is_string() || token == parsed.end() || !token->is_string())
            {
                outError = path.string() + " has no string `url` and `token`";
                return false;
            }
            out.Url = url->get<std::string>();
            out.Token = token->get<std::string>();
            out.Origin = path.string();
            return true;
        }

        std::vector<std::filesystem::path> FindDiscoveryFiles(const std::filesystem::path& directory)
        {
            std::vector<std::filesystem::path> found;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                const std::string name = entry.path().filename().string();
                const bool matches = name == kLegacyDiscoveryName ||
                                     (name.rfind(kDiscoveryPrefix, 0) == 0 && name.size() > 5 &&
                                      name.compare(name.size() - 5, 5, ".json") == 0);
                if (matches)
                    found.push_back(entry.path());
            }
            std::sort(found.begin(), found.end());
            return found;
        }

        // Split "http://127.0.0.1:7345/mcp" into the scheme+authority httplib::Client
        // takes and the path it posts to.
        bool SplitUrl(const std::string& url, std::string& outBase, std::string& outPath, std::string& outError)
        {
            const std::size_t schemeEnd = url.find("://");
            if (schemeEnd == std::string::npos)
            {
                outError = "'" + url + "' is not an http(s) URL";
                return false;
            }
            const std::size_t pathStart = url.find('/', schemeEnd + 3);
            outBase = pathStart == std::string::npos ? url : url.substr(0, pathStart);
            outPath = pathStart == std::string::npos ? "/mcp" : url.substr(pathStart);
            return true;
        }
    } // namespace

    std::optional<Endpoint> ResolveEndpoint(const ConnectionRequest& request, std::string& outError)
    {
        // PRECEDENCE, and the order is load-bearing: everything the user typed on
        // this command line beats the environment, which beats the search.
        //
        //   --url/--token  ->  --discovery-file  ->  --port  ->  OLO_MCP_DISCOVERY_FILE  ->  scan
        //
        // The env var is LAST of the four because `run-oloengine`'s driver.ps1 sets
        // it in the launching shell when it attaches an editor, so a worktree session
        // inherits it. Reading it ahead of --port would make `oloctl --port <other
        // editor>` attach to THIS worktree's editor instead, answer about the wrong
        // scene, and exit 0 — the silent wrong answer this whole function exists to
        // prevent.
        if (!request.Url.empty())
        {
            // OLOCTL_TOKEN so the bearer token need not go through argv. A discovery
            // file never puts it there; `--url --token` does, and argv is readable by
            // other local processes and lands in shell history, which the discovery
            // file does not. `--token` still wins when both are given.
            std::string token = request.Token;
            std::string origin = "--url";
            if (token.empty())
            {
                if (const std::optional<std::string> fromEnv = EnvVar("OLOCTL_TOKEN"))
                {
                    token = *fromEnv;
                    origin = "--url with OLOCTL_TOKEN";
                }
            }
            if (token.empty())
            {
                outError = "--url needs a token: the editor's MCP endpoint rejects an unauthenticated request. "
                           "Pass --token, or set OLOCTL_TOKEN to keep it out of your shell history. The token "
                           "is in the editor's MCP Server panel.";
                return std::nullopt;
            }
            return Endpoint{ request.Url, token, origin };
        }
        if (!request.Token.empty())
        {
            outError = "--token without --url. Give both, or neither and let the discovery file supply them.";
            return std::nullopt;
        }
        // OLOCTL_TOKEN alone says nothing about WHICH editor to reach, and silently
        // ignoring it would leave someone believing they had configured a connection.
        if (EnvVar("OLOCTL_TOKEN").has_value())
        {
            outError = "OLOCTL_TOKEN is set but no --url was given. OLOCTL_TOKEN only supplies the token for an "
                       "explicit --url; a discovery file already carries its own.";
            return std::nullopt;
        }

        Endpoint endpoint;
        std::string readError;
        if (!request.DiscoveryFile.empty())
        {
            if (!ReadDiscoveryFile(request.DiscoveryFile, endpoint, readError))
            {
                outError = "--discovery-file: " + readError;
                return std::nullopt;
            }
            return endpoint;
        }

        // The temp directory is only needed by the two paths that search it, so a
        // machine with an unreadable one still works through the explicit options
        // above and through the environment below.
        const auto tempDirectory = [](std::string& failure) -> std::optional<std::filesystem::path>
        {
            std::error_code ec;
            std::filesystem::path directory = std::filesystem::temp_directory_path(ec);
            if (ec)
            {
                failure = "the system temp directory is unreadable (" + ec.message() +
                          "), so no editor discovery file can be found. Pass --url and --token, or "
                          "--discovery-file.";
                return std::nullopt;
            }
            return directory;
        };

        if (request.Port != 0)
        {
            const std::optional<std::filesystem::path> temp = tempDirectory(outError);
            if (!temp)
                return std::nullopt;
            const std::filesystem::path path = DiscoveryPathForPort(*temp, request.Port);
            if (!ReadDiscoveryFile(path, endpoint, readError))
            {
                outError = "--port " + std::to_string(request.Port) + ": " + readError +
                           ". Is an editor running on that port?";
                return std::nullopt;
            }
            return endpoint;
        }

        if (const std::optional<std::string> fromEnv = EnvVar("OLO_MCP_DISCOVERY_FILE"))
        {
            if (!ReadDiscoveryFile(*fromEnv, endpoint, readError))
            {
                outError = "OLO_MCP_DISCOVERY_FILE: " + readError;
                return std::nullopt;
            }
            return endpoint;
        }

        const std::optional<std::filesystem::path> temp = tempDirectory(outError);
        if (!temp)
            return std::nullopt;

        const std::vector<std::filesystem::path> candidates = FindDiscoveryFiles(*temp);
        if (candidates.empty())
        {
            outError = "no running editor found: nothing matching " + std::string(kLegacyDiscoveryName) + " or " +
                       kDiscoveryPrefix + "<port>.json in " + temp->string() +
                       ". Start OloEditor and its MCP Server panel, or pass --url and --token.";
            return std::nullopt;
        }
        if (candidates.size() > 1)
        {
            std::ostringstream message;
            message << candidates.size() << " editors advertise an MCP endpoint; oloctl will not pick one. "
                    << "Re-run with --port <n> or --discovery-file <path>. Found:";
            for (const std::filesystem::path& candidate : candidates)
                message << "\n  " << candidate.string();
            outError = message.str();
            return std::nullopt;
        }
        if (!ReadDiscoveryFile(candidates.front(), endpoint, readError))
        {
            outError = readError;
            return std::nullopt;
        }
        return endpoint;
    }

    struct McpHttpCommandSource::Impl
    {
        ConnectionRequest Request;
        std::ostream& Err;
        bool Verbose = false;

        Endpoint Where;
        std::string Path;
        std::unique_ptr<httplib::Client> Http;
        std::string SessionId;
        std::string ProtocolVersion;
        bool Connected = false;

        std::optional<Catalogue> Cached;
        long long NextId = 1;

        Impl(ConnectionRequest request, std::ostream& err, bool verbose)
            : Request(std::move(request)), Err(err), Verbose(verbose)
        {
        }

        bool Connect(std::string& outError)
        {
            if (Connected)
                return true;

            const std::optional<Endpoint> endpoint = ResolveEndpoint(Request, outError);
            if (!endpoint)
                return false;
            Where = *endpoint;

            std::string base;
            if (!SplitUrl(Where.Url, base, Path, outError))
                return false;

            Http = std::make_unique<httplib::Client>(base);
            Http->set_read_timeout(Request.TimeoutMs / 1000, (Request.TimeoutMs % 1000) * 1000);
            Http->set_write_timeout(Request.TimeoutMs / 1000, (Request.TimeoutMs % 1000) * 1000);
            Http->set_connection_timeout(0, 2000 * 1000);

            // A real MCP handshake rather than firing tools/call at a cold endpoint:
            // it settles the protocol revision (the server 400s an unsupported
            // MCP-Protocol-Version header) and mints the session id later requests
            // carry, and it is the one call whose failure diagnoses the connection.
            Json result;
            if (!Call("initialize",
                      Json{ { "protocolVersion", kRequestedProtocolVersion },
                            { "capabilities", Json::object() },
                            { "clientInfo", Json{ { "name", "oloctl" }, { "version", "0.1.0" } } } },
                      result, outError))
            {
                return false;
            }
            if (const auto version = result.find("protocolVersion");
                version != result.end() && version->is_string())
            {
                ProtocolVersion = version->get<std::string>();
            }
            Notify("notifications/initialized", Json::object());
            Connected = true;

            if (Verbose)
            {
                std::string serverName = "unknown";
                if (const auto info = result.find("serverInfo"); info != result.end() && info->is_object())
                {
                    if (const auto name = info->find("name"); name != info->end() && name->is_string())
                        serverName = name->get<std::string>();
                }
                Err << "oloctl: attached to " << serverName << " at " << Where.Url << " (via " << Where.Origin
                    << "), protocol " << (ProtocolVersion.empty() ? "unnegotiated" : ProtocolVersion) << '\n';
            }
            return true;
        }

        httplib::Headers BuildHeaders() const
        {
            httplib::Headers headers{ { "Authorization", "Bearer " + Where.Token },
                                      { "Accept", "application/json" } };
            if (!SessionId.empty())
                headers.emplace("Mcp-Session-Id", SessionId);
            if (!ProtocolVersion.empty())
                headers.emplace("MCP-Protocol-Version", ProtocolVersion);
            return headers;
        }

        // One JSON-RPC request. `outResult` is the `result` member on success;
        // `outError` is a sentence on any failure — transport, HTTP status, malformed
        // body, or a JSON-RPC error object.
        bool Call(const std::string& method, const Json& params, Json& outResult, std::string& outError)
        {
            const Json request{ { "jsonrpc", "2.0" }, { "id", NextId++ }, { "method", method }, { "params", params } };
            const httplib::Result response = Http->Post(Path, BuildHeaders(), request.dump(), "application/json");
            if (!response)
            {
                outError = "cannot reach the editor at " + Where.Url + " (" +
                           httplib::to_string(response.error()) + "). Found via " + Where.Origin +
                           "; is that editor still running?";
                return false;
            }
            if (response->status == 401)
            {
                outError = "the editor rejected the bearer token from " + Where.Origin +
                           ". Restarting the MCP server mints a new token; re-read the discovery file.";
                return false;
            }
            if (response->status < 200 || response->status >= 300)
            {
                outError = "the editor answered HTTP " + std::to_string(response->status) + " to " + method + ": " +
                           response->body;
                return false;
            }
            if (response->has_header("Mcp-Session-Id"))
                SessionId = response->get_header_value("Mcp-Session-Id");

            Json parsed = Json::parse(response->body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                outError = "the editor answered " + method + " with something that is not a JSON-RPC object";
                return false;
            }
            if (const auto error = parsed.find("error"); error != parsed.end() && error->is_object())
            {
                const auto message = error->find("message");
                outError = "the editor refused " + method + ": " +
                           (message != error->end() && message->is_string() ? message->get<std::string>()
                                                                            : error->dump());
                return false;
            }
            const auto result = parsed.find("result");
            if (result == parsed.end())
            {
                outError = "the editor's answer to " + method + " carried neither `result` nor `error`";
                return false;
            }
            outResult = *result;
            return true;
        }

        void Notify(const std::string& method, const Json& params)
        {
            const Json notification{ { "jsonrpc", "2.0" }, { "method", method }, { "params", params } };
            // Best-effort: a notification has no response, and a server that ignores
            // `notifications/initialized` is still usable.
            static_cast<void>(Http->Post(Path, BuildHeaders(), notification.dump(), "application/json"));
        }
    };

    McpHttpCommandSource::McpHttpCommandSource(ConnectionRequest request, std::ostream& err, bool verbose)
        : m_Impl(std::make_unique<Impl>(std::move(request), err, verbose))
    {
    }

    McpHttpCommandSource::~McpHttpCommandSource() = default;

    const Catalogue* McpHttpCommandSource::FetchCatalogue(std::string& outError)
    {
        if (m_Impl->Cached)
            return &*m_Impl->Cached;
        if (!m_Impl->Connect(outError))
            return nullptr;

        // olo_tool_search over the WHOLE registry, not tools/list. tools/list is
        // filtered by the editor's exposure profile (#1124), which exists to protect
        // an agent's context window — a budget a CLI does not have. A profile-filtered
        // CLI would silently lack most of its commands, and `olo_capability` would be
        // the only way to find out. `limit` is the schema's maximum.
        Json searchResult;
        Json searchArguments{ { "limit", 200 }, { "includeSchemas", true } };
        if (!m_Impl->Call("tools/call",
                          Json{ { "name", "olo_tool_search" }, { "arguments", std::move(searchArguments) } },
                          searchResult, outError))
        {
            return nullptr;
        }

        const auto structured = searchResult.find("structuredContent");
        if (structured == searchResult.end() || !structured->is_object())
        {
            const auto isError = searchResult.find("isError");
            const bool failed = isError != searchResult.end() && isError->is_boolean() && isError->get<bool>();
            outError = failed ? "the editor's olo_tool_search failed: " + searchResult.value("content", Json()).dump()
                              : "the editor's olo_tool_search returned no structured catalogue";
            return nullptr;
        }

        const auto tools = structured->find("tools");
        if (tools == structured->end() || !tools->is_array())
        {
            outError = "the editor's olo_tool_search returned no `tools` array";
            return nullptr;
        }

        const auto matched = structured->find("matched");
        const auto returned = structured->find("returned");
        if (matched != structured->end() && returned != structured->end() && matched->is_number_unsigned() &&
            returned->is_number_unsigned() && matched->get<std::size_t>() > returned->get<std::size_t>())
        {
            // Loud rather than a short list that looks complete. The gateway caps
            // `limit` at 200; a registry past that needs paging in the gateway, not a
            // truncated CLI.
            m_Impl->Err << "oloctl: the editor reports " << matched->get<std::size_t>()
                        << " commands but returned only " << returned->get<std::size_t>()
                        << "; the rest are missing from this command tree. File it against the automation "
                           "registry - olo_tool_search caps `limit` at 200.\n";
        }

        m_Impl->Cached =
            ParseCatalogue(*tools, "olo_tool_search on " + m_Impl->Where.Url + " (via " + m_Impl->Where.Origin + ')');
        return &*m_Impl->Cached;
    }

    ICommandSource::InvokeOutcome McpHttpCommandSource::Invoke(const std::string& name, const Json& arguments)
    {
        InvokeOutcome outcome;
        if (!m_Impl->Connect(outcome.Error))
            return outcome;

        // A plain tools/call, exactly what an MCP client sends. Not olo_tool_execute:
        // that is a rewrite gateway for clients that refuse to call an unlisted name,
        // and going through it would put a second dispatch shape between oloctl and
        // the command — which is what makes the payload comparison in the acceptance
        // test meaningful.
        Json result;
        if (!m_Impl->Call("tools/call", Json{ { "name", name }, { "arguments", arguments } }, result, outcome.Error))
            return outcome;

        outcome.Ok = true;
        outcome.Result = std::move(result);
        return outcome;
    }
} // namespace OloCtl
