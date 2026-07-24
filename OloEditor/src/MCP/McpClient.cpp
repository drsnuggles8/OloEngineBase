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

        // MCP handshake: initialize -> notifications/initialized -> tools/list.
        // 2025-06-18 is the newest revision whose features we rely on here
        // (outputSchema/structuredContent pass-through); the child may echo an
        // older one — transport framing is identical, so we accept any reply.
        const Json initParams{ { "protocolVersion", "2025-06-18" },
                               { "capabilities", Json::object() },
                               { "clientInfo", Json{ { "name", "OloEditor" }, { "version", "0.0.1" } } } };
        const std::optional<Json> initResponse =
            connection->SendRequest("initialize", initParams, config.HandshakeTimeout);
        if (!initResponse || !initResponse->contains("result"))
        {
            outError = "MCP initialize failed (no/invalid response from '" + config.Command + "')";
            connection->Shutdown();
            return nullptr;
        }
        connection->SendNotification("notifications/initialized", Json::object());

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

        const Json request{ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } };
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
        OLO_CORE_INFO("[MCP] Outbound client '{}' connected: {} tool(s) bridged from `{}`.", config.Alias,
                      merged, config.Command);
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
