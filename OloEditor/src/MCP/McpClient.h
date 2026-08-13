#pragma once

// Outbound MCP client, stdio transport (issue #673 Tier 1, bullet 1): the
// editor's MCP server can now also CONSUME tools — spawn a local MCP server
// process (e.g. a filesystem or web-fetch server), speak newline-delimited
// JSON-RPC over its stdin/stdout, and fold its tools into the native registry
// as `ext.<alias>.<tool>` entries via McpServer::ReplaceClientTools (which
// forces the untrusted-authority posture; see that method's contract).
//
// Layering:
//   * IMcpClientTransport — the byte seam. The Windows implementation
//     (MakeStdioTransportFactory, McpClient.cpp) owns CreateProcessW + pipes +
//     a blocking reader thread; tests substitute an in-memory fake that
//     replies synchronously inside WriteLine.
//   * McpClientConnection — the protocol + concurrency layer: era negotiation
//     (see NegotiateEra — `server/discover` probe, falling back to the
//     initialize / initialized handshake), tools/list, request-id
//     multiplexing (bridged
//     tool handlers run CONCURRENTLY on httplib worker threads, so responses
//     are matched to waiters by exact id, the same id.dump() keying the
//     server's cancellation registry uses), timeouts, and teardown that never
//     strands a blocked handler (Shutdown fails every pending request BEFORE
//     the server joins its worker pool — the consent-abort pattern).
//
// Ownership: McpServer owns the connections (ConnectStdioClient /
// DisconnectClient / ClientStatuses, defined in McpClient.cpp) and shuts them
// down inside Stop(). Each bridged ToolDef::Handler strongly captures its
// shared_ptr<McpClientConnection>, so a live re-merge or disconnect can never
// destroy a connection under an executing call (the ScriptToolsRuntime
// lifetime pattern).

#include "MCP/McpServer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::MCP
{
    // The byte-stream seam between the protocol layer and the child process.
    //
    // Threading contract:
    //   * WriteLine receives one complete JSON-RPC message WITHOUT the trailing
    //     newline (the transport appends it). Calls are serialized by the
    //     connection's write mutex — implementations need no write lock.
    //   * The transport delivers each incoming line via the onLine callback handed
    //     to the factory, from its own reader thread (or synchronously inside
    //     WriteLine for an in-memory test fake), and onClosed exactly once when
    //     the stream ends for any reason.
    //   * Close() is idempotent and must guarantee NO further onLine/onClosed
    //     callback is in flight or forthcoming once it returns (the Windows
    //     implementation joins its reader thread; it also ends the child:
    //     stdin-EOF first, TerminateProcess after a short grace).
    class IMcpClientTransport
    {
      public:
        virtual ~IMcpClientTransport() = default;
        [[nodiscard]] virtual bool WriteLine(const std::string& payload) = 0;
        virtual void Close() = 0;
    };

    // Spawns the Windows stdio transport: CreateProcessW(command) with piped
    // stdin/stdout (stderr -> NUL so a chatty child can never corrupt the
    // NDJSON stream), plus a reader thread that splits stdout on '\n'.
    // Returns nullptr with outError set on spawn failure. On non-Windows
    // builds it always fails (outbound clients are Windows-only for now).
    [[nodiscard]] McpClientTransportFactory MakeStdioTransportFactory();

    // One outbound connection: a spawned child MCP server + its bridged tools.
    class McpClientConnection
    {
      public:
        // Spawn (via `factory`), negotiate an era (see NegotiateEra), run the
        // matching opening sequence, list the child's tools, and build the
        // bridged ToolDefs. Returns nullptr with `outError` set on any failure
        // (spawn, timeout, malformed handshake). `onDeath(alias)` is invoked at
        // most once, from the transport's reader thread, when the child dies or
        // the stream breaks OUTSIDE a deliberate Shutdown — the owner uses it to
        // unpublish the alias's tools.
        [[nodiscard]] static std::shared_ptr<McpClientConnection> Connect(
            const McpClientConfig& config, const McpClientTransportFactory& factory,
            std::function<void(const std::string& alias)> onDeath, std::string& outError);

        ~McpClientConnection();
        McpClientConnection(const McpClientConnection&) = delete;
        McpClientConnection& operator=(const McpClientConnection&) = delete;

        // The bridged ToolDefs built from the child's tools/list (names already
        // prefixed `ext.<alias>.`; handlers strongly capture this connection).
        // Hand them to McpServer::ReplaceClientTools — which re-validates and
        // forces the authority posture; this class does not try to.
        [[nodiscard]] std::vector<ToolDef> TakeBridgedTools();

        // Fail every pending request, end the child, join the reader. Idempotent;
        // suppresses the onDeath callback (deliberate teardown is not a death).
        // Never blocks on the game thread or the server's locks.
        void Shutdown();

        [[nodiscard]] const std::string& Alias() const
        {
            return m_Config.Alias;
        }
        [[nodiscard]] const std::string& Command() const
        {
            return m_Config.Command;
        }
        [[nodiscard]] bool IsConnected() const
        {
            return m_Alive.load(std::memory_order_acquire);
        }
        [[nodiscard]] sizet BridgedToolCount() const
        {
            return m_BridgedToolCount;
        }

        // The era this connection negotiated at Connect time, and the exact
        // revision string it stamps on requests (issue #777). Fixed for the
        // lifetime of the connection — the spec makes era a property of the
        // server, not of an individual request — so these are plain reads.
        [[nodiscard]] McpProtocolEra Era() const
        {
            return m_Era;
        }
        [[nodiscard]] const std::string& ProtocolVersion() const
        {
            return m_ProtocolVersion;
        }

      private:
        explicit McpClientConnection(McpClientConfig config,
                                     std::function<void(const std::string&)> onDeath);

        // Decide whether the child speaks the 2026-07-28 stateless core or the
        // legacy `initialize` handshake, and settle on a mutually supported
        // revision. Implements the spec's stdio backward-compatibility rule:
        // probe with `server/discover`, treat a recognized MODERN reply (a
        // DiscoverResult, or an UnsupportedProtocolVersionError naming the
        // versions it does support) as a modern server, and fall back to the
        // legacy handshake on anything else — including a timeout. Sets m_Era
        // and m_ProtocolVersion; returns false with `outError` set only when the
        // child is modern but shares no revision with us (an actionable
        // mismatch, not a fallback).
        [[nodiscard]] bool NegotiateEra(std::string& outError);

        // The per-request `_meta` block every MODERN request must carry: the
        // protocol version (required), our identity (SHOULD), and our capability
        // set for THIS request (required — the server may not infer it from a
        // previous one). Empty in the legacy era, where the handshake carried
        // all three once.
        [[nodiscard]] Json RequestMeta() const;

        // Send one request and block the CALLING thread (an httplib worker or
        // the connecting thread) until the child's response arrives, `timeout`
        // expires, or the connection shuts down. Returns the full JSON-RPC
        // response object, or nullopt on timeout/write-failure/shutdown. In the
        // modern era `params._meta` is stamped on by RequestMeta() unless the
        // caller already supplied one.
        [[nodiscard]] std::optional<Json> SendRequest(const std::string& method, const Json& params,
                                                      std::chrono::milliseconds timeout);
        void SendNotification(const std::string& method, const Json& params);

        // Transport callbacks (reader thread / inline from a fake).
        void OnLine(const std::string& line);
        void OnTransportClosed();

        [[nodiscard]] ToolResult InvokeBridged(const std::string& childToolName, const Json& arguments);
        void BuildBridgedTools(const Json& toolsArray, const std::shared_ptr<McpClientConnection>& self);

        struct PendingCall
        {
            std::mutex M;
            std::condition_variable Cv;
            bool Done = false;    // response arrived
            bool Aborted = false; // shutdown / stream death
            Json Response;
        };

        McpClientConfig m_Config;
        std::function<void(const std::string&)> m_OnDeath;
        std::unique_ptr<IMcpClientTransport> m_Transport;

        // Serializes WriteLine calls (concurrent bridged handlers). NEVER held
        // while waiting; never held together with m_PendingMutex.
        std::mutex m_WriteMutex;

        std::mutex m_PendingMutex;
        std::unordered_map<std::string, std::shared_ptr<PendingCall>> m_Pending; // key: id.dump()
        std::atomic<u64> m_NextId{ 1 };

        std::atomic<bool> m_Alive{ false };
        std::atomic<bool> m_ShuttingDown{ false };

        // Written once by NegotiateEra during Connect, before the connection is
        // published to any other thread; read-only thereafter.
        McpProtocolEra m_Era = McpProtocolEra::Legacy;
        std::string m_ProtocolVersion;
        // True when the era probe got a RECOGNIZED modern reply but we still chose
        // the legacy handshake (the child advertised only handshake-era revisions,
        // or its reply named no versions at all). Purely diagnostic: if `initialize`
        // then fails, the plain "initialize failed" message would point at the wrong
        // problem, so Connect appends the era context it alone knows.
        bool m_FellBackFromModernReply = false;

        std::vector<ToolDef> m_BridgedTools;
        sizet m_BridgedToolCount = 0;
    };
} // namespace OloEngine::MCP
