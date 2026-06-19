// httplib.h MUST be included before any header that pulls in <windows.h> so that
// <winsock2.h> wins the include race on Windows (windows.h would otherwise drag in
// the legacy <winsock.h> and cause redefinition errors). OloEditor has no PCH, so
// this single ordering rule is enough.
#include "OloEnginePCH.h"
#include <httplib.h>

#include "MCP/McpServer.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Task/NamedThreads.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace OloEngine::MCP
{
    namespace
    {
        // ---- JSON-RPC 2.0 envelope helpers -------------------------------------
        constexpr int kParseError = -32700;
        constexpr int kInvalidRequest = -32600;
        constexpr int kMethodNotFound = -32601;
        constexpr int kInvalidParams = -32602;

        Json MakeResult(const Json& id, Json result)
        {
            return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } };
        }

        Json MakeError(const Json& id, int code, const std::string& message)
        {
            return Json{ { "jsonrpc", "2.0" },
                         { "id", id },
                         { "error", { { "code", code }, { "message", message } } } };
        }

        // Random lowercase-hex string of `bytes` bytes (so 2*bytes characters).
        // Used for the auth token and session ids. std::random_device is seeded
        // per call; this is a localhost secret, not a cryptographic key exchange.
        std::string GenerateHexToken(std::size_t bytes)
        {
            std::random_device rd;
            std::mt19937_64 gen(((static_cast<u64>(rd()) << 32) ^ rd()) ^
                                static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::uniform_int_distribution<u32> dist(0, 255);

            static constexpr char kHex[] = "0123456789abcdef";
            std::string out;
            out.reserve(bytes * 2);
            for (std::size_t i = 0; i < bytes; ++i)
            {
                const auto b = static_cast<u8>(dist(gen));
                out.push_back(kHex[b >> 4]);
                out.push_back(kHex[b & 0x0F]);
            }
            return out;
        }

        // Length-independent, content-constant-time string compare for the bearer
        // token, to avoid leaking the token length/prefix via response timing.
        bool ConstantTimeEquals(std::string_view a, std::string_view b)
        {
            // Fold length difference into the accumulator instead of early-out.
            u32 diff = static_cast<u32>(a.size() ^ b.size());
            const std::size_t n = std::min(a.size(), b.size());
            for (std::size_t i = 0; i < n; ++i)
                diff |= static_cast<u8>(a[i]) ^ static_cast<u8>(b[i]);
            return diff == 0;
        }

        // Write/remove the discovery file (host/port/token/url) used for attach
        // without copy-paste. Best-effort — failures are logged, never fatal.
        void WriteDiscoveryFile(const std::string& path, u16 port, const std::string& token)
        {
            if (path.empty())
                return;
            Json j;
            j["host"] = "127.0.0.1";
            j["port"] = port;
            j["token"] = token;
            j["url"] = "http://127.0.0.1:" + std::to_string(port) + "/mcp";

            std::ofstream out(path, std::ios::trunc | std::ios::binary);
            if (!out)
            {
                OLO_CORE_WARN("[MCP] Could not write discovery file: {}", path);
                return;
            }
            out << j.dump(2);
        }

        void RemoveDiscoveryFile(const std::string& path)
        {
            if (path.empty())
                return;
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(path), ec);
        }

        // Scrub absolute filesystem paths from text (Windows drive-letter paths and
        // POSIX /home//Users paths) so project layout / usernames don't leak when
        // redaction is enabled. Heuristic, best-effort.
        std::string RedactPathsInText(const std::string& text)
        {
            static const std::regex winPath(R"([A-Za-z]:[\\/][^\s"'<>|]*)");
            static const std::regex posixHome(R"(/(?:home|Users)/[^\s"'<>|]*)");
            std::string out = std::regex_replace(text, winPath, "<path>");
            out = std::regex_replace(out, posixHome, "<path>");
            return out;
        }

        // Apply redaction in place to every text content block of an MCP content array.
        void RedactContentArray(Json& content)
        {
            if (!content.is_array())
                return;
            for (auto& block : content)
            {
                if (block.is_object() && block.value("type", std::string{}) == "text" && block.contains("text") && block["text"].is_string())
                    block["text"] = RedactPathsInText(block["text"].get<std::string>());
            }
        }

    } // namespace

    // ---- ToolResult ------------------------------------------------------------

    ToolResult ToolResult::Text(const std::string& text)
    {
        ToolResult r;
        r.Content = Json::array({ Json{ { "type", "text" }, { "text", text } } });
        r.IsError = false;
        return r;
    }

    ToolResult ToolResult::Error(const std::string& message)
    {
        ToolResult r;
        r.Content = Json::array({ Json{ { "type", "text" }, { "text", message } } });
        r.IsError = true;
        return r;
    }

    // ---- McpServer -------------------------------------------------------------

    McpServer::McpServer(EditorMcpContext context)
        : m_Context(std::move(context))
    {
    }

    McpServer::~McpServer()
    {
        Stop();
    }

    void McpServer::RegisterTool(ToolDef tool)
    {
        m_Tools.push_back(std::move(tool));
    }

    void McpServer::RegisterResource(ResourceDef resource)
    {
        m_Resources.push_back(std::move(resource));
    }

    void McpServer::RegisterPrompt(PromptDef prompt)
    {
        m_Prompts.push_back(std::move(prompt));
    }

    bool McpServer::Start(u16 port)
    {
        if (m_Running.load(std::memory_order_acquire))
            return false;

        m_Token = GenerateHexToken(16);

        m_Http = CreateScope<httplib::Server>();

        m_Http->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res)
                     { HandlePost(req, res); });

        // We never push server-initiated messages, so the Streamable-HTTP GET
        // stream is unsupported — 405 is the spec-sanctioned response.
        m_Http->Get("/mcp", [](const httplib::Request&, httplib::Response& res)
                    { res.status = 405; });

        // Explicit session teardown.
        m_Http->Delete("/mcp", [this](const httplib::Request& req, httplib::Response& res)
                       {
            if (req.has_header("Mcp-Session-Id"))
            {
                const std::string sid = req.get_header_value("Mcp-Session-Id");
                std::lock_guard lock(m_SessionMutex);
                m_Sessions.erase(sid);
            }
            res.status = 200; });

        if (!m_Http->bind_to_port("127.0.0.1", static_cast<int>(port)))
        {
            OLO_CORE_ERROR("[MCP] Failed to bind 127.0.0.1:{} — is the port already in use?", port);
            m_Http.reset();
            m_Token.clear();
            return false;
        }

        m_Port = port;
        m_Running.store(true, std::memory_order_release);
        m_ListenThread = std::thread([this]
                                     {
            m_Http->listen_after_bind();
            m_Running.store(false, std::memory_order_release); });

        WriteDiscoveryFile(DiscoveryFilePath(m_Port), m_Port, m_Token);

        OLO_CORE_INFO("[MCP] Read-only diagnostics server listening on http://127.0.0.1:{}/mcp", port);
        return true;
    }

    void McpServer::Stop()
    {
        if (!m_Http)
            return;

        // Signal first so any handler blocked in MarshalRead aborts promptly
        // instead of deadlocking against this thread (Stop runs on the game thread).
        m_Running.store(false, std::memory_order_release);

        m_Http->stop();
        if (m_ListenThread.joinable())
            m_ListenThread.join();
        // Destroying the Server joins its internal worker pool, so no handler is
        // running once this returns — safe to clear the token afterwards.
        m_Http.reset();

        RemoveDiscoveryFile(DiscoveryFilePath(m_Port));

        {
            std::lock_guard lock(m_SessionMutex);
            m_Sessions.clear();
        }
        m_Token.clear();

        OLO_CORE_INFO("[MCP] Diagnostics server stopped");
    }

    std::string McpServer::DiscoveryFilePath(u16 port)
    {
        // An explicit override wins: the launching tool picks the exact path it will
        // read back, so parallel worktree editors never collide regardless of port.
        if (const char* overridePath = std::getenv("OLO_MCP_DISCOVERY_FILE");
            overridePath != nullptr && *overridePath != '\0')
            return overridePath;

        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec)
            return {};

        // Default port keeps the legacy single-file name (back-compat for the panel /
        // manual attach and the docs); any other port namespaces by port so two
        // editors on distinct ports don't overwrite each other's host/token.
        if (port == DefaultPort)
            return (dir / "oloengine-mcp.json").string();
        return (dir / ("oloengine-mcp-" + std::to_string(port) + ".json")).string();
    }

    Json McpServer::MarshalRead(const std::function<Json()>& readJob, std::chrono::milliseconds timeout)
    {
        auto promise = std::make_shared<std::promise<Json>>();
        std::future<Json> future = promise->get_future();
        const bool stillRunning = m_Running.load(std::memory_order_acquire);

        // Enqueue onto the game thread; it drains this at the next frame boundary
        // (Application::Run, before the scene is stepped) — a consistent snapshot.
        OloEngine::Tasks::EnqueueGameThreadTask(
            [promise, readJob]()
            {
                try
                {
                    promise->set_value(readJob());
                }
                catch (...)
                {
                    promise->set_exception(std::current_exception());
                }
            },
            "MCP_MainThreadRead");

        if (!stillRunning)
            throw std::runtime_error("MCP server is not running");

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready)
                return future.get();

            // If the server is being torn down, bail rather than block teardown.
            if (!m_Running.load(std::memory_order_acquire))
                throw std::runtime_error("MCP server stopping; main-thread read aborted");

            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Timed out waiting for the editor main thread (is the editor responsive?)");
        }
    }

    void McpServer::HandlePost(const httplib::Request& req, httplib::Response& res)
    {
        const auto sendJson = [&res](const Json& body, int status)
        {
            res.status = status;
            res.set_content(body.dump(), "application/json");
        };

        // 1. Origin check (DNS-rebinding defence).
        if (req.has_header("Origin") && !IsOriginAllowed(req.get_header_value("Origin")))
        {
            res.status = 403;
            sendJson(MakeError(Json(nullptr), kInvalidRequest, "Origin not allowed"), 403);
            return;
        }

        // 2. Bearer-token auth.
        if (!CheckAuth(req))
        {
            res.set_header("WWW-Authenticate", "Bearer");
            sendJson(MakeError(Json(nullptr), kInvalidRequest, "Unauthorized"), 401);
            return;
        }

        // 3. Session validation (only when the client presents one).
        if (req.has_header("Mcp-Session-Id"))
        {
            const std::string sid = req.get_header_value("Mcp-Session-Id");
            std::lock_guard lock(m_SessionMutex);
            if (!m_Sessions.contains(sid))
            {
                // Unknown/expired session — tell the client to re-initialize.
                sendJson(MakeError(Json(nullptr), kInvalidRequest, "Unknown session"), 404);
                return;
            }
        }

        // 4-5. Parse + route the JSON-RPC body. All framing (parse error, batch
        // handling, notification suppression, the initialize session-id side
        // effect) lives in the transport-agnostic seam so it can be unit tested.
        const FramedResponse framed = ProcessRequestBody(req.body);

        // Echo the freshly minted session id on a successful initialize so
        // subsequent requests can be correlated (and old sessions invalidated
        // across server restarts).
        if (!framed.SessionId.empty())
            res.set_header("Mcp-Session-Id", framed.SessionId);

        if (framed.Body.is_null())
        {
            res.status = framed.Status; // 202 — notification / all-notification batch
            return;
        }
        sendJson(framed.Body, framed.Status);
    }

    Json McpServer::HandleMessage(const Json& message)
    {
        return DispatchRpc(message);
    }

    McpServer::FramedResponse McpServer::ProcessRequestBody(const std::string& body)
    {
        FramedResponse out;

        // Parse the JSON-RPC body.
        Json parsed;
        try
        {
            parsed = Json::parse(body);
        }
        catch (const std::exception&)
        {
            out.Body = MakeError(Json(nullptr), kParseError, "Parse error");
            return out;
        }

        // Batch: array of messages → array of responses (notifications drop out).
        if (parsed.is_array())
        {
            // An empty batch is itself an invalid JSON-RPC request (spec §6).
            if (parsed.empty())
            {
                out.Body = MakeError(Json(nullptr), kInvalidRequest, "Invalid Request");
                return out;
            }
            Json responses = Json::array();
            for (const auto& message : parsed)
            {
                Json response = DispatchRpc(message);
                if (!response.is_null())
                    responses.push_back(std::move(response));
            }
            if (responses.empty())
            {
                out.Status = 202; // all notifications — nothing to return
                return out;
            }
            out.Body = std::move(responses);
            return out;
        }

        // Single message.
        // Detect a successful initialize for the session-id side effect. Read
        // "method" defensively (see DispatchRpc): value() would throw on a
        // non-string method.
        std::string method;
        if (parsed.is_object() && parsed.contains("method") && parsed["method"].is_string())
            method = parsed["method"].get<std::string>();
        Json response = DispatchRpc(parsed);

        // A successful initialize mints + registers a session id; the transport
        // surfaces it in the Mcp-Session-Id header.
        if (method == "initialize" && response.contains("result"))
        {
            std::string sid = GenerateHexToken(16);
            {
                std::lock_guard lock(m_SessionMutex);
                m_Sessions.insert(sid);
            }
            out.SessionId = std::move(sid);
        }

        if (response.is_null())
        {
            out.Status = 202; // notification — nothing to return
            return out;
        }
        out.Body = std::move(response);
        return out;
    }

    Json McpServer::DispatchRpc(const Json& request)
    {
        if (!request.is_object())
            return MakeError(Json(nullptr), kInvalidRequest, "Invalid Request");

        const bool hasId = request.contains("id");
        const Json id = hasId ? request["id"] : Json(nullptr);

        // Read "method" defensively: nlohmann's value() throws type_error.302 when
        // the key is present but not a string, so a malformed `"method": 123` would
        // escape as an exception instead of a clean JSON-RPC error. Treat any
        // non-string (or absent) method as missing — handled as Invalid Request below.
        std::string method;
        if (request.contains("method") && request["method"].is_string())
            method = request["method"].get<std::string>();

        // Notifications (no id) get no response; we have no notification side effects.
        if (!hasId)
            return Json(nullptr);

        if (method.empty())
            return MakeError(id, kInvalidRequest, "Invalid Request: missing method");

        if (method == "initialize")
            return HandleInitialize(id, request.value("params", Json::object()));
        if (method == "ping")
            return MakeResult(id, Json::object());
        if (method == "tools/list")
            return HandleToolsList(id);
        if (method == "tools/call")
            return HandleToolsCall(id, request.value("params", Json::object()));
        if (method == "resources/list")
            return HandleResourcesList(id);
        if (method == "resources/read")
            return HandleResourcesRead(id, request.value("params", Json::object()));
        if (method == "prompts/list")
            return HandlePromptsList(id);
        if (method == "prompts/get")
            return HandlePromptsGet(id, request.value("params", Json::object()));

        return MakeError(id, kMethodNotFound, "Method not found: " + method);
    }

    Json McpServer::HandleInitialize(const Json& id, const Json& params)
    {
        // Echo the client's protocol version when we recognise it, else advertise
        // our latest. Transport framing is identical across these revisions.
        static constexpr std::array<std::string_view, 3> kSupported = {
            "2025-06-18", "2025-03-26", "2024-11-05"
        };
        std::string version{ "2025-06-18" };
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
        {
            const std::string requested = params["protocolVersion"].get<std::string>();
            if (std::find(kSupported.begin(), kSupported.end(), requested) != kSupported.end())
                version = requested;
        }

        Json result;
        result["protocolVersion"] = version;
        result["capabilities"] = Json{ { "tools", { { "listChanged", false } } },
                                       { "resources", { { "subscribe", false }, { "listChanged", false } } },
                                       { "prompts", { { "listChanged", false } } } };
        result["serverInfo"] = Json{ { "name", "OloEditor" },
                                     { "title", "OloEngine Editor Diagnostics" },
                                     { "version", "0.0.1" } };
        result["instructions"] =
            "Read-only diagnostics for a running OloEngine editor session. Use olo_log_tail "
            "to see the most recent engine log messages, olo_events_tail for a 'what just "
            "happened?' timeline (scene load, play/stop, entity spawn/destroy, asset reload, "
            "script error — poll incrementally with sinceId), and olo_scene_summary to inspect "
            "the active scene. Everything exposed here is read-only — no tool mutates the project.";
        return MakeResult(id, result);
    }

    Json McpServer::HandleToolsList(const Json& id) const
    {
        Json tools = Json::array();
        for (const auto& tool : m_Tools)
        {
            Json entry;
            entry["name"] = tool.Name;
            entry["description"] = tool.Description;
            entry["inputSchema"] = tool.InputSchema.is_null()
                                       ? Json{ { "type", "object" } }
                                       : tool.InputSchema;
            tools.push_back(std::move(entry));
        }
        return MakeResult(id, Json{ { "tools", std::move(tools) } });
    }

    Json McpServer::HandleToolsCall(const Json& id, const Json& params)
    {
        if (!params.contains("name") || !params["name"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'name' is required");

        const std::string name = params["name"].get<std::string>();
        const ToolDef* tool = FindTool(name);
        if (tool == nullptr)
            return MakeError(id, kInvalidParams, "Unknown tool: " + name);

        const Json arguments = (params.contains("arguments") && params["arguments"].is_object())
                                   ? params["arguments"]
                                   : Json::object();

        ToolResult result;
        try
        {
            result = tool->Handler(*this, arguments);
        }
        catch (const std::exception& e)
        {
            result = ToolResult::Error(std::string("Tool failed: ") + e.what());
        }

        if (RedactPaths())
            RedactContentArray(result.Content);

        return MakeResult(id, Json{ { "content", std::move(result.Content) }, { "isError", result.IsError } });
    }

    const ToolDef* McpServer::FindTool(const std::string& name) const
    {
        for (const auto& tool : m_Tools)
        {
            if (tool.Name == name)
                return &tool;
        }
        return nullptr;
    }

    Json McpServer::HandleResourcesList(const Json& id) const
    {
        Json resources = Json::array();
        for (const auto& resource : m_Resources)
        {
            resources.push_back(Json{ { "uri", resource.Uri },
                                      { "name", resource.Name },
                                      { "description", resource.Description },
                                      { "mimeType", resource.MimeType } });
        }
        return MakeResult(id, Json{ { "resources", std::move(resources) } });
    }

    Json McpServer::HandleResourcesRead(const Json& id, const Json& params)
    {
        if (!params.contains("uri") || !params["uri"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'uri' is required");

        const std::string uri = params["uri"].get<std::string>();
        const ResourceDef* resource = FindResource(uri);
        if (resource == nullptr)
            return MakeError(id, kInvalidParams, "Unknown resource: " + uri);

        std::string text;
        try
        {
            text = resource->Reader(*this);
        }
        catch (const std::exception& e)
        {
            return MakeError(id, kInvalidRequest, std::string("Failed to read resource: ") + e.what());
        }

        if (RedactPaths())
            text = RedactPathsInText(text);

        return MakeResult(id, Json{ { "contents",
                                      Json::array({ Json{ { "uri", uri },
                                                          { "mimeType", resource->MimeType },
                                                          { "text", std::move(text) } } }) } });
    }

    const ResourceDef* McpServer::FindResource(const std::string& uri) const
    {
        for (const auto& resource : m_Resources)
        {
            if (resource.Uri == uri)
                return &resource;
        }
        return nullptr;
    }

    Json McpServer::HandlePromptsList(const Json& id) const
    {
        Json prompts = Json::array();
        for (const auto& prompt : m_Prompts)
        {
            prompts.push_back(Json{ { "name", prompt.Name },
                                    { "title", prompt.Title },
                                    { "description", prompt.Description } });
        }
        return MakeResult(id, Json{ { "prompts", std::move(prompts) } });
    }

    Json McpServer::HandlePromptsGet(const Json& id, const Json& params) const
    {
        if (!params.contains("name") || !params["name"].is_string())
            return MakeError(id, kInvalidParams, "Invalid params: 'name' is required");

        const std::string name = params["name"].get<std::string>();
        const PromptDef* prompt = FindPrompt(name);
        if (prompt == nullptr)
            return MakeError(id, kInvalidParams, "Unknown prompt: " + name);

        Json messages = Json::array({ Json{ { "role", "user" },
                                            { "content", { { "type", "text" }, { "text", prompt->Text } } } } });
        return MakeResult(id, Json{ { "description", prompt->Description }, { "messages", std::move(messages) } });
    }

    const PromptDef* McpServer::FindPrompt(const std::string& name) const
    {
        for (const auto& prompt : m_Prompts)
        {
            if (prompt.Name == name)
                return &prompt;
        }
        return nullptr;
    }

    bool McpServer::CheckAuth(const httplib::Request& req) const
    {
        if (!req.has_header("Authorization"))
            return false;
        return CheckBearerAuth(req.get_header_value("Authorization"), m_Token);
    }

    bool McpServer::CheckBearerAuth(std::string_view authorizationHeader, std::string_view expectedToken)
    {
        // An empty expected token means the server isn't running (no token has been
        // generated) — reject everything, including an empty presented token.
        if (expectedToken.empty())
            return false;

        constexpr std::string_view kPrefix = "Bearer ";
        if (authorizationHeader.size() <= kPrefix.size() || authorizationHeader.substr(0, kPrefix.size()) != kPrefix)
            return false;

        return ConstantTimeEquals(authorizationHeader.substr(kPrefix.size()), expectedToken);
    }

    bool McpServer::IsOriginAllowed(std::string_view origin)
    {
        // A browser-originated request carries an Origin; non-browser agents
        // (Claude Code/Desktop) send none — accept those. When present, the host
        // must be loopback.
        if (origin.empty() || origin == "null")
            return true;
        const auto schemeEnd = origin.find("://");
        if (schemeEnd == std::string_view::npos)
            return false;
        const auto hostStart = schemeEnd + 3;
        if (hostStart >= origin.size())
            return false;

        std::string_view host;
        if (origin[hostStart] == '[')
        {
            // Bracketed IPv6 literal (e.g. http://[::1]:7345). The address itself
            // is full of ':' separators, so the host runs to the closing ']', not
            // the first ':'. Keep the brackets so it matches the allowlist form.
            const auto bracketEnd = origin.find(']', hostStart);
            if (bracketEnd == std::string_view::npos)
                return false;
            host = origin.substr(hostStart, bracketEnd - hostStart + 1);
        }
        else
        {
            const auto hostEnd = origin.find_first_of(":/", hostStart);
            host = origin.substr(hostStart, hostEnd == std::string_view::npos ? std::string_view::npos : hostEnd - hostStart);
        }
        return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
    }
} // namespace OloEngine::MCP
