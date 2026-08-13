#include "OloEnginePCH.h"
#include "MCP/McpClient.h"

#include "OloEngine/Core/Log.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace OloEngine::MCP
{
    namespace
    {
        // The two revisions this client speaks, one per era (issue #777).
        //
        // Modern (2026-07-28, the stateless core): no handshake at all — every
        // request carries its protocol version, our identity and our per-request
        // capabilities in `params._meta`. The routing headers (Mcp-Method /
        // Mcp-Name) and header/body validation that revision also mandates are
        // Streamable-HTTP concerns and do NOT apply to this stdio client.
        //
        // Legacy (2025-06-18): the newest revision whose features we rely on
        // (outputSchema/structuredContent pass-through). The child may echo an
        // older one — transport framing is identical, so we accept any reply.
        constexpr const char* kModernProtocolVersion = "2026-07-28";
        constexpr const char* kLegacyProtocolVersion = "2025-06-18";

        // UnsupportedProtocolVersionError (spec 2026-07-28). Reserved by the spec,
        // so seeing it is itself proof the peer is a modern server — which is why
        // it steers us to a version retry rather than to the legacy fallback.
        constexpr int kUnsupportedProtocolVersionCode = -32022;

        // Reserved `_meta` keys carrying what `initialize` used to carry once.
        constexpr const char* kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
        constexpr const char* kMetaClientInfo = "io.modelcontextprotocol/clientInfo";
        constexpr const char* kMetaClientCapabilities = "io.modelcontextprotocol/clientCapabilities";

        [[nodiscard]] Json ClientInfo()
        {
            return Json{ { "name", "OloEditor" }, { "version", "0.0.1" } };
        }

        // True when `versions` (a JSON array of revision strings) names a revision
        // this client can speak in the MODERN era.
        [[nodiscard]] bool OffersModernVersion(const Json& versions)
        {
            if (!versions.is_array())
                return false;
            return std::any_of(versions.begin(), versions.end(),
                               [](const Json& v)
                               { return v.is_string() && v.get<std::string>() == kModernProtocolVersion; });
        }

        // True when `versions` names a revision we can speak in the LEGACY era.
        // A dual-era child answers server/discover but may still list only
        // handshake-era revisions; that is a fallback, not a failure. Any
        // "2025-*"/"2024-*" entry qualifies because HandleInitialize-style
        // negotiation lets the child echo whichever of those it prefers.
        [[nodiscard]] bool OffersLegacyVersion(const Json& versions)
        {
            if (!versions.is_array())
                return false;
            return std::any_of(versions.begin(), versions.end(),
                               [](const Json& v)
                               {
                                   if (!v.is_string())
                                       return false;
                                   const std::string s = v.get<std::string>();
                                   return s.rfind("2025-", 0) == 0 || s.rfind("2024-", 0) == 0;
                               });
        }

        [[nodiscard]] std::string JoinVersions(const Json& versions)
        {
            std::string out;
            if (!versions.is_array())
                return out;
            for (const Json& v : versions)
            {
                if (!v.is_string())
                    continue;
                if (!out.empty())
                    out += ", ";
                out += v.get<std::string>();
            }
            return out;
        }
    } // namespace

    // ---- McpClientConnection: protocol + concurrency ---------------------------

    McpClientConnection::McpClientConnection(McpClientConfig config,
                                             std::function<void(const std::string&)> onDeath)
        : m_Config(std::move(config)), m_OnDeath(std::move(onDeath))
    {
    }

    McpClientConnection::~McpClientConnection()
    {
        Shutdown();
    }

    std::shared_ptr<McpClientConnection> McpClientConnection::Connect(
        const McpClientConfig& config, const McpClientTransportFactory& factory,
        std::function<void(const std::string& alias)> onDeath, std::string& outError)
    {
        outError.clear();
        std::shared_ptr<McpClientConnection> connection(new McpClientConnection(config, std::move(onDeath)));

        // Weak captures: the transport's reader-thread callbacks must not keep
        // the connection alive (the connection owns the transport — a strong
        // capture would be a permanent reference cycle, the exact leak the Lua
        // script runtime's olo.call_tool closure had to break the same way).
        std::weak_ptr<McpClientConnection> weak = connection;
        std::string transportError;
        auto transport = factory(
            config.Command,
            [weak](std::string line)
            {
                if (const std::shared_ptr<McpClientConnection> self = weak.lock())
                    self->OnLine(line);
            },
            [weak]()
            {
                if (const std::shared_ptr<McpClientConnection> self = weak.lock())
                    self->OnTransportClosed();
            },
            transportError);
        if (!transport)
        {
            outError = transportError.empty() ? "failed to start the child process" : transportError;
            return nullptr;
        }
        connection->m_Transport = std::move(transport);
        connection->m_Alive.store(true, std::memory_order_release);

        // Decide the era BEFORE anything else: it determines both the opening
        // sequence and whether every later request carries `_meta`.
        if (!connection->NegotiateEra(outError))
        {
            connection->Shutdown();
            return nullptr;
        }

        // Legacy era only: the initialize -> notifications/initialized handshake.
        // A modern child has no handshake — `server/discover` already told us
        // everything initialize used to, and sending `initialize` to it would be
        // an unknown method.
        if (connection->m_Era == McpProtocolEra::Legacy)
        {
            const Json initParams{ { "protocolVersion", connection->m_ProtocolVersion },
                                   { "capabilities", Json::object() },
                                   { "clientInfo", ClientInfo() } };
            const std::optional<Json> initResponse =
                connection->SendRequest("initialize", initParams, config.HandshakeTimeout);
            if (!initResponse || !initResponse->contains("result"))
            {
                outError = "MCP initialize failed (no/invalid response from '" + config.Command + "')";
                connection->Shutdown();
                return nullptr;
            }
            // The handshake NEGOTIATES: the child echoes the revision it actually
            // agreed to, which may be older or newer than the one we asked for.
            // Record that, not our request — otherwise the panel and the connect log
            // report a version nobody is speaking, and the first thing anyone
            // debugging an era problem looks at is wrong.
            const Json& initResult = (*initResponse)["result"];
            if (initResult.is_object() && initResult.contains("protocolVersion") &&
                initResult["protocolVersion"].is_string())
            {
                connection->m_ProtocolVersion = initResult["protocolVersion"].get<std::string>();
            }
            connection->SendNotification("notifications/initialized", Json::object());
        }

        const std::optional<Json> toolsResponse =
            connection->SendRequest("tools/list", Json::object(), config.HandshakeTimeout);
        if (!toolsResponse || !toolsResponse->contains("result") ||
            !(*toolsResponse)["result"].contains("tools") || !(*toolsResponse)["result"]["tools"].is_array())
        {
            outError = "MCP tools/list failed (no/invalid response from '" + config.Command + "')";
            connection->Shutdown();
            return nullptr;
        }

        connection->BuildBridgedTools((*toolsResponse)["result"]["tools"], connection);
        return connection;
    }

    bool McpClientConnection::NegotiateEra(std::string& outError)
    {
        // Default assumption: legacy. Every path below either confirms it or
        // upgrades us, so a probe that fails in ANY unrecognised way (including a
        // child that never answers) lands on the handshake — which is exactly the
        // spec's stdio backward-compatibility rule, and what every MCP server in
        // the wild speaks today.
        m_Era = McpProtocolEra::Legacy;
        m_ProtocolVersion = kLegacyProtocolVersion;

        // The probe must be self-describing: `server/discover` is a modern method
        // and a modern server validates `_meta` on it like any other request. Send
        // it explicitly rather than via RequestMeta(), which is still legacy here.
        const Json probeParams{ { "_meta",
                                  Json{ { kMetaProtocolVersion, kModernProtocolVersion },
                                        { kMetaClientInfo, ClientInfo() },
                                        { kMetaClientCapabilities, Json::object() } } } };
        const std::chrono::milliseconds probeTimeout =
            std::min(m_Config.DiscoverProbeTimeout, m_Config.HandshakeTimeout);
        const std::optional<Json> response = SendRequest("server/discover", probeParams, probeTimeout);

        // No reply at all (timeout, or the child died mid-probe): legacy. Note the
        // ordering guarantee this relies on — a late `server/discover` response
        // arriving after we fall back finds no waiter in m_Pending and is dropped
        // by OnLine, so it cannot be mistaken for a later request's answer.
        if (!response)
            return true;

        // A DiscoverResult: definitively modern. Take the newest revision we both
        // speak; a dual-era child that lists only handshake revisions sends us
        // back to the legacy path rather than failing.
        if (response->contains("result") && (*response)["result"].is_object())
        {
            const Json& result = (*response)["result"];
            const Json versions = result.value("supportedVersions", Json::array());
            if (OffersModernVersion(versions))
            {
                m_Era = McpProtocolEra::Modern;
                m_ProtocolVersion = kModernProtocolVersion;
                return true;
            }
            if (OffersLegacyVersion(versions) || !versions.is_array() || versions.empty())
                return true; // dual-era (or uninformative) — the handshake still works
            outError = "MCP client '" + m_Config.Alias + "': the server speaks only " + JoinVersions(versions) +
                       ", none of which OloEditor supports";
            return false;
        }

        // An UnsupportedProtocolVersionError is itself a modern marker: only a
        // modern server emits it, so this is a version mismatch to resolve, NOT a
        // reason to fall back (the spec is explicit that a recognized modern error
        // identifies a modern server).
        if (response->contains("error") && (*response)["error"].is_object())
        {
            const Json& error = (*response)["error"];
            // Defensive read: a child's reply is untrusted, and value() throws on a
            // present-but-non-numeric "code".
            const bool unsupportedVersion = error.contains("code") && error["code"].is_number_integer() &&
                                            error["code"].get<int>() == kUnsupportedProtocolVersionCode;
            if (unsupportedVersion)
            {
                const Json supported = error.contains("data") && error["data"].is_object()
                                           ? error["data"].value("supported", Json::array())
                                           : Json::array();
                if (OffersModernVersion(supported))
                {
                    m_Era = McpProtocolEra::Modern;
                    m_ProtocolVersion = kModernProtocolVersion;
                    return true;
                }
                // Same rule as the DiscoverResult branch above, deliberately: only a
                // list that is present AND names revisions we cannot speak is a hard
                // failure. An absent or empty `supported` tells us nothing, and
                // hard-failing on it would strand every tool from a child whose only
                // sin is a thin error payload — so fall back and let the handshake
                // answer the question.
                if (OffersLegacyVersion(supported) || !supported.is_array() || supported.empty())
                    return true;
                outError = "MCP client '" + m_Config.Alias +
                           "': the server rejected every protocol version OloEditor speaks (it supports " +
                           JoinVersions(supported) + ")";
                return false;
            }
        }

        // Anything else — most commonly -32601 from a legacy server that has never
        // heard of `server/discover`. Legacy it is.
        return true;
    }

    Json McpClientConnection::RequestMeta() const
    {
        if (m_Era != McpProtocolEra::Modern)
            return Json::object();
        // clientCapabilities is REQUIRED and per-request: the server must not infer
        // it from an earlier call. We consume tools only — no sampling, roots or
        // elicitation — so an empty object is the honest declaration.
        return Json{ { kMetaProtocolVersion, m_ProtocolVersion },
                     { kMetaClientInfo, ClientInfo() },
                     { kMetaClientCapabilities, Json::object() } };
    }

    void McpClientConnection::BuildBridgedTools(const Json& toolsArray,
                                                const std::shared_ptr<McpClientConnection>& self)
    {
        const std::string prefix = McpServer::ClientToolPrefix(m_Config.Alias);
        for (const Json& entry : toolsArray)
        {
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string())
            {
                OLO_CORE_WARN("[MCP client '{}'] skipping a tools/list entry with no string name.",
                              m_Config.Alias);
                continue;
            }
            const std::string childName = entry["name"].get<std::string>();

            ToolDef def;
            def.Name = prefix + childName;
            def.Title = entry.value("title", std::string{});
            // The provenance note travels in the description so a calling agent
            // knows this tool leaves the editor process (latency + trust).
            def.Description = "(bridged from external MCP server '" + m_Config.Alias + "') " +
                              entry.value("description", std::string{});
            if (entry.contains("inputSchema") && entry["inputSchema"].is_object())
                def.InputSchema = entry["inputSchema"]; // local validation before forwarding
            if (entry.contains("outputSchema") && entry["outputSchema"].is_object())
                def.OutputSchema = entry["outputSchema"];
            if (entry.contains("icons"))
                def.Icons = entry["icons"]; // ReplaceClientTools validates-or-drops
            // Strong capture on purpose: an executing bridged call keeps this
            // connection (and its transport) alive across a disconnect/re-merge.
            def.Handler = [self, childName](McpServer&, const Json& arguments)
            { return self->InvokeBridged(childName, arguments); };
            m_BridgedTools.push_back(std::move(def));
        }
        m_BridgedToolCount = m_BridgedTools.size();
    }

    std::vector<ToolDef> McpClientConnection::TakeBridgedTools()
    {
        return std::move(m_BridgedTools);
    }

    ToolResult McpClientConnection::InvokeBridged(const std::string& childToolName, const Json& arguments)
    {
        const std::optional<Json> response = SendRequest(
            "tools/call", Json{ { "name", childToolName }, { "arguments", arguments } }, m_Config.CallTimeout);
        if (!response)
            return ToolResult::Error("External server '" + m_Config.Alias +
                                     "' did not respond (disconnected or timed out).");
        if (response->contains("error"))
        {
            std::string message = "external server returned an error";
            if ((*response)["error"].is_object())
                message = (*response)["error"].value("message", message);
            return ToolResult::Error("External server '" + m_Config.Alias + "': " + message);
        }

        const Json result = response->value("result", Json::object());

        // Multi Round-Trip Requests (spec 2026-07-28): a modern server may answer
        // with resultType "input_required" instead of a finished call, expecting us
        // to satisfy `inputRequests` (elicitation / sampling / roots) and retry. We
        // implement none of those, and an absent resultType means "complete" for
        // backward compatibility — so the ONLY thing that must not happen is
        // forwarding an interim result to the agent as though it were the answer.
        // Fail loudly instead; a silent half-result is the failure mode this bridge
        // must not have. The field is read defensively (contains + is_string rather
        // than value()) because a child's reply is untrusted network data and
        // nlohmann's value() throws type_error.302 on a present-but-wrong type.
        if (result.is_object() && result.contains("resultType"))
        {
            const Json& resultType = result["resultType"];
            if (!resultType.is_string() || resultType.get<std::string>() != "complete")
            {
                return ToolResult::Error(
                    "External server '" + m_Config.Alias + "' did not return a completed result (resultType " +
                    resultType.dump() +
                    "); OloEditor's outbound MCP client does not implement Multi Round-Trip Requests, so the "
                    "call was not completed.");
            }
        }

        ToolResult out;
        out.IsError = result.is_object() && result.value("isError", false);
        if (result.is_object() && result.contains("content") && result["content"].is_array())
            out.Content = result["content"];
        else
            out.Content = Json::array({ Json{ { "type", "text" }, { "text", result.dump(2) } } });
        if (result.is_object() && result.contains("structuredContent") && result["structuredContent"].is_object())
            out.StructuredContent = result["structuredContent"];
        return out;
    }

    std::optional<Json> McpClientConnection::SendRequest(const std::string& method, const Json& params,
                                                         std::chrono::milliseconds timeout)
    {
        if (!m_Alive.load(std::memory_order_acquire))
            return std::nullopt;

        // Exact-value id keying, the server's own cancellation-registry precedent.
        const Json id = m_NextId.fetch_add(1, std::memory_order_relaxed);
        const std::string key = id.dump();
        auto pending = std::make_shared<PendingCall>();
        {
            std::lock_guard lock(m_PendingMutex);
            m_Pending.emplace(key, pending);
        }

        // Modern era: every request is self-describing. Stamp `_meta` here — the
        // one place every client-initiated request funnels through — rather than
        // at each call site, so a future request can never ship without it. A
        // caller that already supplied `_meta` (the era probe) keeps its own.
        Json outParams = params;
        if (m_Era == McpProtocolEra::Modern)
        {
            if (!outParams.is_object())
                outParams = Json::object();
            if (!outParams.contains("_meta"))
                outParams["_meta"] = RequestMeta();
        }

        const Json request{ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", std::move(outParams) } };
        bool written = false;
        {
            // Serialize concurrent bridged handlers' writes. Never held while
            // waiting; compact dump() never contains a raw newline, so one line
            // is one message (the NDJSON property McpEventStream.h relies on).
            std::lock_guard lock(m_WriteMutex);
            written = m_Transport && m_Transport->WriteLine(request.dump());
        }
        if (!written)
        {
            std::lock_guard lock(m_PendingMutex);
            m_Pending.erase(key);
            return std::nullopt;
        }

        std::unique_lock lock(pending->M);
        pending->Cv.wait_for(lock, timeout, [&pending]
                             { return pending->Done || pending->Aborted; });
        const bool done = pending->Done;
        Json response = std::move(pending->Response);
        lock.unlock();
        {
            std::lock_guard g(m_PendingMutex);
            m_Pending.erase(key);
        }
        if (!done)
            return std::nullopt;
        return response;
    }

    void McpClientConnection::SendNotification(const std::string& method, const Json& params)
    {
        const Json notification{ { "jsonrpc", "2.0" }, { "method", method }, { "params", params } };
        std::lock_guard lock(m_WriteMutex);
        if (m_Transport)
            (void)m_Transport->WriteLine(notification.dump());
    }

    void McpClientConnection::OnLine(const std::string& line)
    {
        const Json message = Json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (!message.is_object())
        {
            OLO_CORE_WARN("[MCP client '{}'] dropping an unparseable line from the child.", m_Config.Alias);
            return;
        }

        const bool hasId = message.contains("id") && !message["id"].is_null();
        if (hasId && (message.contains("result") || message.contains("error")))
        {
            // A response: complete the matching waiter.
            std::shared_ptr<PendingCall> pending;
            {
                std::lock_guard lock(m_PendingMutex);
                if (const auto it = m_Pending.find(message["id"].dump()); it != m_Pending.end())
                    pending = it->second;
            }
            if (pending)
            {
                {
                    std::lock_guard lock(pending->M);
                    pending->Response = message;
                    pending->Done = true;
                }
                pending->Cv.notify_all();
            }
            return;
        }
        if (hasId && message.contains("method"))
        {
            // A request FROM the child (sampling, roots, ...) — not supported.
            // Answer cleanly so a well-behaved child never hangs waiting on us.
            const Json reply{ { "jsonrpc", "2.0" },
                              { "id", message["id"] },
                              { "error", Json{ { "code", -32601 },
                                               { "message",
                                                 "OloEditor's outbound MCP client does not accept requests" } } } };
            std::lock_guard lock(m_WriteMutex);
            if (m_Transport)
                (void)m_Transport->WriteLine(reply.dump());
            return;
        }

        // A notification. v1 deliberately does not live-re-merge on
        // tools/list_changed (reconnect picks the new list up); log so the
        // operator can see why the surface looks stale.
        if (message.value("method", std::string{}) == "notifications/tools/list_changed")
            OLO_CORE_INFO("[MCP client '{}'] child announced a tool-list change; disconnect + reconnect to "
                          "pick it up.",
                          m_Config.Alias);
    }

    void McpClientConnection::OnTransportClosed()
    {
        m_Alive.store(false, std::memory_order_release);

        // Fail every waiter promptly — a blocked httplib worker must never wait
        // out its full timeout against a child that is already gone.
        std::unordered_map<std::string, std::shared_ptr<PendingCall>> pending;
        {
            std::lock_guard lock(m_PendingMutex);
            pending.swap(m_Pending);
        }
        for (auto& [key, call] : pending)
        {
            {
                std::lock_guard lock(call->M);
                call->Aborted = true;
            }
            call->Cv.notify_all();
        }

        // Deliberate teardown is not a death; only an unexpected end of stream
        // unpublishes the alias's tools via the owner's callback.
        if (!m_ShuttingDown.load(std::memory_order_acquire) && m_OnDeath)
        {
            OLO_CORE_WARN("[MCP client '{}'] child process ended; unpublishing its bridged tools.",
                          m_Config.Alias);
            m_OnDeath(m_Config.Alias);
        }
    }

    void McpClientConnection::Shutdown()
    {
        if (m_ShuttingDown.exchange(true, std::memory_order_acq_rel))
            return;
        m_Alive.store(false, std::memory_order_release);

        // Drain waiters first so no handler thread is parked while the
        // transport tears the child down (mirrors Stop()'s consent-abort-
        // before-join ordering).
        std::unordered_map<std::string, std::shared_ptr<PendingCall>> pending;
        {
            std::lock_guard lock(m_PendingMutex);
            pending.swap(m_Pending);
        }
        for (auto& [key, call] : pending)
        {
            {
                std::lock_guard lock(call->M);
                call->Aborted = true;
            }
            call->Cv.notify_all();
        }

        // No connection lock may be held here: Close() joins the reader thread,
        // and the reader's callbacks take the pending/write mutexes.
        if (m_Transport)
            m_Transport->Close();
    }

    // ---- Windows stdio transport ----------------------------------------------

#ifdef _WIN32
    namespace
    {
        class WindowsPipeTransport final : public IMcpClientTransport
        {
          public:
            static std::unique_ptr<WindowsPipeTransport> Spawn(const std::string& command,
                                                               std::function<void(std::string)> onLine,
                                                               std::function<void()> onClosed,
                                                               std::string& outError)
            {
                SECURITY_ATTRIBUTES inheritable{};
                inheritable.nLength = sizeof(SECURITY_ATTRIBUTES);
                inheritable.bInheritHandle = TRUE;

                HANDLE stdinRead = nullptr;
                HANDLE stdinWrite = nullptr;
                HANDLE stdoutRead = nullptr;
                HANDLE stdoutWrite = nullptr;
                HANDLE nulHandle = nullptr;
                const auto closeAll = [&]
                {
                    for (HANDLE h : { stdinRead, stdinWrite, stdoutRead, stdoutWrite, nulHandle })
                    {
                        if (h != nullptr)
                            ::CloseHandle(h);
                    }
                };

                if (!::CreatePipe(&stdinRead, &stdinWrite, &inheritable, 0) ||
                    !::CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 0))
                {
                    closeAll();
                    outError = "CreatePipe failed";
                    return nullptr;
                }
                // Our ends must NOT leak into the child, or the child holding a
                // duplicate of stdoutWrite would keep our reader from ever
                // seeing EOF (and stdinWrite would defeat the polite-EOF close).
                ::SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
                ::SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

                // stderr -> NUL: a chatty child logging to stderr must never
                // corrupt the NDJSON stdout stream, and the editor is a GUI app
                // with no console to inherit.
                nulHandle = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                                          &inheritable, OPEN_EXISTING, 0, nullptr);

                // UTF-8 command line -> mutable UTF-16 buffer (CreateProcessW
                // may modify it in place).
                const int wideLength =
                    ::MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
                std::wstring wideCommand(static_cast<std::size_t>(wideLength > 0 ? wideLength : 1), L'\0');
                if (wideLength > 0)
                    ::MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wideCommand.data(), wideLength);

                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdInput = stdinRead;
                startup.hStdOutput = stdoutWrite;
                startup.hStdError = nulHandle != nullptr ? nulHandle : INVALID_HANDLE_VALUE;

                PROCESS_INFORMATION process{};
                if (!::CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
                {
                    const DWORD error = ::GetLastError();
                    closeAll();
                    outError = "CreateProcess failed (error " + std::to_string(error) + ") for: " + command;
                    return nullptr;
                }

                // Child-side ends are the child's problem now.
                ::CloseHandle(stdinRead);
                ::CloseHandle(stdoutWrite);
                if (nulHandle != nullptr)
                    ::CloseHandle(nulHandle);

                auto transport = std::unique_ptr<WindowsPipeTransport>(new WindowsPipeTransport());
                transport->m_StdinWrite = stdinWrite;
                transport->m_StdoutRead = stdoutRead;
                transport->m_Process = process.hProcess;
                transport->m_ProcessThread = process.hThread;
                transport->m_OnLine = std::move(onLine);
                transport->m_OnClosed = std::move(onClosed);
                transport->m_Reader = std::thread([transportPtr = transport.get()]
                                                  { transportPtr->ReaderLoop(); });
                return transport;
            }

            ~WindowsPipeTransport() override
            {
                Close();
            }

            [[nodiscard]] bool WriteLine(const std::string& payload) override
            {
                std::lock_guard lock(m_StdinMutex);
                if (m_StdinWrite == nullptr)
                    return false;
                std::string data = payload;
                data.push_back('\n');
                const char* cursor = data.data();
                DWORD remaining = static_cast<DWORD>(data.size());
                while (remaining > 0)
                {
                    DWORD written = 0;
                    if (!::WriteFile(m_StdinWrite, cursor, remaining, &written, nullptr) || written == 0)
                        return false;
                    cursor += written;
                    remaining -= written;
                }
                return true;
            }

            void Close() override
            {
                if (m_Closed.exchange(true, std::memory_order_acq_rel))
                {
                    // Second caller (e.g. destructor after an explicit Close)
                    // must still not race the join.
                    if (m_Reader.joinable())
                        m_Reader.join();
                    return;
                }

                // Polite first: closing the child's stdin is the conventional
                // "we're done" signal for stdio MCP servers.
                {
                    std::lock_guard lock(m_StdinMutex);
                    if (m_StdinWrite != nullptr)
                    {
                        ::CloseHandle(m_StdinWrite);
                        m_StdinWrite = nullptr;
                    }
                }
                if (m_Process != nullptr &&
                    ::WaitForSingleObject(m_Process, kChildExitGraceMs) == WAIT_TIMEOUT)
                {
                    ::TerminateProcess(m_Process, 1);
                    ::WaitForSingleObject(m_Process, kChildExitGraceMs);
                }
                // The child (and every write handle to its stdout pipe) is gone,
                // so the reader's blocking ReadFile returns EOF and the thread
                // exits — the only reliable way to unblock a pipe read.
                if (m_Reader.joinable())
                    m_Reader.join();

                if (m_StdoutRead != nullptr)
                {
                    ::CloseHandle(m_StdoutRead);
                    m_StdoutRead = nullptr;
                }
                if (m_ProcessThread != nullptr)
                {
                    ::CloseHandle(m_ProcessThread);
                    m_ProcessThread = nullptr;
                }
                if (m_Process != nullptr)
                {
                    ::CloseHandle(m_Process);
                    m_Process = nullptr;
                }
            }

          private:
            WindowsPipeTransport() = default;

            void ReaderLoop()
            {
                std::string buffer;
                char chunk[4096];
                for (;;)
                {
                    DWORD read = 0;
                    if (!::ReadFile(m_StdoutRead, chunk, sizeof(chunk), &read, nullptr) || read == 0)
                        break;
                    buffer.append(chunk, read);
                    std::size_t newline = 0;
                    while ((newline = buffer.find('\n')) != std::string::npos)
                    {
                        std::string line = buffer.substr(0, newline);
                        buffer.erase(0, newline + 1);
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();
                        if (!line.empty() && m_OnLine)
                            m_OnLine(std::move(line));
                    }
                }
                if (m_OnClosed)
                    m_OnClosed();
            }

            static constexpr DWORD kChildExitGraceMs = 2000;

            std::mutex m_StdinMutex; // guards m_StdinWrite (WriteLine vs Close)
            HANDLE m_StdinWrite = nullptr;
            HANDLE m_StdoutRead = nullptr;
            HANDLE m_Process = nullptr;
            HANDLE m_ProcessThread = nullptr;
            std::thread m_Reader;
            std::atomic<bool> m_Closed{ false };
            std::function<void(std::string)> m_OnLine;
            std::function<void()> m_OnClosed;
        };
    } // namespace
#endif // _WIN32

    McpClientTransportFactory MakeStdioTransportFactory()
    {
#ifdef _WIN32
        return [](const std::string& command, std::function<void(std::string)> onLine,
                  std::function<void()> onClosed, std::string& outError) -> std::unique_ptr<IMcpClientTransport>
        { return WindowsPipeTransport::Spawn(command, std::move(onLine), std::move(onClosed), outError); };
#else
        return [](const std::string& /*command*/, std::function<void(std::string)> /*onLine*/,
                  std::function<void()> /*onClosed*/, std::string& outError) -> std::unique_ptr<IMcpClientTransport>
        {
            outError = "outbound stdio MCP clients are not supported on this platform yet";
            return nullptr;
        };
#endif
    }

    // ---- McpServer's connection ownership (defined here, not in McpServer.cpp,
    // so the dispatch core TU stays transport-agnostic) --------------------------

    std::string McpServer::ConnectStdioClient(const McpClientConfig& config)
    {
        return ConnectClientWithTransport(config, MakeStdioTransportFactory());
    }

    std::string McpServer::ConnectClientWithTransport(const McpClientConfig& config,
                                                      const McpClientTransportFactory& factory)
    {
        if (!IsValidClientAlias(config.Alias))
            return "invalid alias '" + config.Alias + "' (1-32 chars of [a-z0-9-], starting alphanumeric)";
        if (config.Command.empty())
            return "command must not be empty";
        {
            std::lock_guard lock(m_ClientsMutex);
            const bool taken = std::any_of(m_Clients.begin(), m_Clients.end(),
                                           [&config](const std::shared_ptr<McpClientConnection>& client)
                                           { return client->Alias() == config.Alias; });
            if (taken)
                return "alias '" + config.Alias + "' is already in use (disconnect it first)";
        }

        std::string error;
        auto connection = McpClientConnection::Connect(
            config, factory,
            // Reader-thread death callback: unpublish the alias's tools. The
            // connection stays listed (Connected=false) for the panel until
            // DisconnectClient reaps it. `this` outlives every connection —
            // Stop()/~McpServer joins them before the server dies.
            [this](const std::string& alias)
            { ReplaceClientTools(alias, {}); }, error);
        if (!connection)
            return error.empty() ? "connection failed" : error;

        const sizet merged = ReplaceClientTools(config.Alias, connection->TakeBridgedTools());
        {
            std::lock_guard lock(m_ClientsMutex);
            m_Clients.push_back(connection);
        }
        OLO_CORE_INFO("[MCP] Outbound client '{}' connected ({} protocol {}): {} tool(s) bridged from `{}`.",
                      config.Alias, connection->Era() == McpProtocolEra::Modern ? "stateless" : "handshake",
                      connection->ProtocolVersion(), merged, config.Command);
        return {};
    }

    bool McpServer::DisconnectClient(const std::string& alias)
    {
        std::shared_ptr<McpClientConnection> found;
        {
            std::lock_guard lock(m_ClientsMutex);
            const auto it = std::find_if(m_Clients.begin(), m_Clients.end(),
                                         [&alias](const std::shared_ptr<McpClientConnection>& client)
                                         { return client->Alias() == alias; });
            if (it != m_Clients.end())
            {
                found = *it;
                m_Clients.erase(it);
            }
        }
        if (!found)
            return false;
        found->Shutdown();
        ReplaceClientTools(alias, {});
        OLO_CORE_INFO("[MCP] Outbound client '{}' disconnected.", alias);
        return true;
    }

    std::vector<McpClientStatus> McpServer::ClientStatuses() const
    {
        std::vector<McpClientStatus> statuses;
        std::lock_guard lock(m_ClientsMutex);
        statuses.reserve(m_Clients.size());
        for (const std::shared_ptr<McpClientConnection>& client : m_Clients)
        {
            McpClientStatus status;
            status.Alias = client->Alias();
            status.Command = client->Command();
            status.Connected = client->IsConnected();
            status.ToolCount = client->BridgedToolCount();
            status.Era = client->Era();
            status.ProtocolVersion = client->ProtocolVersion();
            statuses.push_back(std::move(status));
        }
        return statuses;
    }

    void McpServer::ShutdownClients()
    {
        std::vector<std::shared_ptr<McpClientConnection>> clients;
        {
            std::lock_guard lock(m_ClientsMutex);
            clients.swap(m_Clients);
        }
        for (const std::shared_ptr<McpClientConnection>& client : clients)
        {
            client->Shutdown();
            ReplaceClientTools(client->Alias(), {});
        }
    }
} // namespace OloEngine::MCP
