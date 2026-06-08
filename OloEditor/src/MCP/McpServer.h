#pragma once

// Read-only MCP (Model Context Protocol) diagnostics server hosted inside OloEditor.
//
// Strategy is "expose, don't embed" (issue #285): OloEditor is a long-running
// process, so it hosts a localhost-only, read-only MCP endpoint over Streamable
// HTTP. A game developer points their own agent (Claude Code / Desktop) at the
// running editor and gets grounded help debugging *their* game from the
// diagnostics the engine already collects.
//
// Security posture (non-negotiable — we expose the user's project):
//   * Binds 127.0.0.1 only, never a routable interface.
//   * Off by default; started explicitly from the editor's MCP panel.
//   * Every request must carry "Authorization: Bearer <token>"; the editor
//     displays the token for the user to paste into their agent config.
//   * The Origin header is validated (DNS-rebinding defence).
//   * Read-only: the dispatch layer exposes only query tools; nothing mutates.
//
// Threading: cpp-httplib handles each request on its own worker thread. The
// already-FMutex-guarded diagnostics (Profiler / MemoryTracker / ShaderDebugger /
// Log) may be read directly from a handler thread ("lock-safe"). The EnTT
// registry is NOT thread-safe, so any Scene / entity read must be marshaled onto
// the main (game) thread at a frame boundary via MarshalRead() ("main-marshaled").

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

// Forward-declare cpp-httplib's types so this header (included by EditorLayer)
// never pulls in <winsock2.h>. The full types are only needed in McpServer.cpp.
namespace httplib
{
    class Server;
    struct Request;
    struct Response;
} // namespace httplib

namespace OloEngine::MCP
{
    using Json = nlohmann::json;

    // Default localhost port the MCP server binds to. Configurable in the panel.
    inline constexpr u16 DefaultPort = 7345;

    // Result of a tool invocation. `Content` is the MCP content array
    // (e.g. [{ "type": "text", "text": "..." }]); `IsError` maps to the
    // tools/call `isError` flag (a tool-level error, not a JSON-RPC protocol error).
    struct ToolResult
    {
        Json Content = Json::array();
        bool IsError = false;

        [[nodiscard]] static ToolResult Text(const std::string& text);
        [[nodiscard]] static ToolResult Error(const std::string& message);
    };

    class McpServer;

    // A tool handler runs on a cpp-httplib worker thread. Lock-safe tools read
    // guarded diagnostics directly; main-marshaled tools wrap their registry/Scene
    // reads in server.MarshalRead(...).
    using ToolHandler = std::function<ToolResult(McpServer& server, const Json& arguments)>;

    // A registered MCP tool. `MainMarshaled` is informational (documents that the
    // handler reads main-thread-only state); it does not change dispatch.
    struct ToolDef
    {
        std::string Name;
        std::string Description;
        Json InputSchema;
        ToolHandler Handler;
        bool MainMarshaled = false;
    };

    // Editor state the main-marshaled tools read. EditorLayer fills these in; the
    // std::function bodies are ONLY safe to call on the main (game) thread, i.e.
    // from inside a MarshalRead() job.
    struct EditorMcpContext
    {
        std::function<Ref<Scene>()> GetActiveScene;
        std::function<bool()> IsPlaying;
        // Capture the editor viewport framebuffer as PNG bytes, downscaled so the
        // long edge is at most maxWidth (<=0 = native). ONLY safe on the main
        // thread (does GL readback), so call it from inside a MarshalRead job.
        // Returns empty bytes on failure.
        std::function<std::vector<u8>(int maxWidth)> CaptureViewportPng;
    };

    // An MCP resource: a passive, addressable blob (vs. an active tool). The reader
    // returns the resource's text; it may marshal/throw exactly like a tool handler.
    using ResourceReader = std::function<std::string(McpServer& server)>;

    struct ResourceDef
    {
        std::string Uri;
        std::string Name;
        std::string Description;
        std::string MimeType;
        ResourceReader Reader;
    };

    // An MCP prompt: a canned workflow shipped for non-expert users. prompts/get
    // expands it into a single user message (Text) that walks the agent through the
    // right tool sequence. Our prompts take no arguments.
    struct PromptDef
    {
        std::string Name;
        std::string Title;
        std::string Description;
        std::string Text;
    };

    class McpServer
    {
      public:
        explicit McpServer(EditorMcpContext context);
        ~McpServer();

        McpServer(const McpServer&) = delete;
        McpServer& operator=(const McpServer&) = delete;
        McpServer(McpServer&&) = delete;
        McpServer& operator=(McpServer&&) = delete;

        // Bind 127.0.0.1:port and start serving on a background thread. Generates a
        // fresh auth token each call. Returns false if already running or the port
        // could not be bound (e.g. already in use); on failure the server stays off.
        bool Start(u16 port);
        void Stop();

        [[nodiscard]] bool IsRunning() const
        {
            return m_Running.load(std::memory_order_acquire);
        }
        [[nodiscard]] u16 GetPort() const
        {
            return m_Port;
        }
        [[nodiscard]] const std::string& GetToken() const
        {
            return m_Token;
        }

        // Optional redaction: when on, absolute filesystem paths are scrubbed from
        // text output before it leaves the process (off by default).
        void SetRedactPaths(bool enabled)
        {
            m_RedactPaths.store(enabled, std::memory_order_relaxed);
        }
        [[nodiscard]] bool RedactPaths() const
        {
            return m_RedactPaths.load(std::memory_order_relaxed);
        }
        [[nodiscard]] const EditorMcpContext& Context() const
        {
            return m_Context;
        }
        [[nodiscard]] const std::vector<ToolDef>& Tools() const
        {
            return m_Tools;
        }
        [[nodiscard]] const std::vector<ResourceDef>& Resources() const
        {
            return m_Resources;
        }
        [[nodiscard]] const std::vector<PromptDef>& Prompts() const
        {
            return m_Prompts;
        }

        // Discovery file written while the server runs: a small JSON blob with the
        // host/port/token/url so an agent (or a test harness) can attach without the
        // user copy-pasting. Lives in the OS temp dir; removed on Stop.
        [[nodiscard]] static std::string DiscoveryFilePath();

        void RegisterTool(ToolDef tool);
        void RegisterResource(ResourceDef resource);
        void RegisterPrompt(PromptDef prompt);

        // Marshal a read onto the main (game) thread at the next frame boundary,
        // blocking the calling (handler) thread on the result. The job runs before
        // the scene is stepped that frame, so it observes a consistent snapshot.
        //
        // Contract / snapshot freshness: the job is serviced by the game thread's
        // per-frame task drain (Application::Run), which ticks every frame in both
        // Edit and Play modes. If the game thread does not service the job within
        // `timeout` (editor stalled / shutting down), this throws std::runtime_error
        // and the caller surfaces it as a tool error.
        //
        // MUST NOT be called from the game thread (it would deadlock). Tools only
        // run on handler threads, so this holds.
        [[nodiscard]] Json MarshalRead(const std::function<Json()>& readJob,
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

        // ---- Dispatch / framing seam (httplib-free; transport-agnostic) --------
        //
        // The HTTP transport (HandlePost) is a thin shell around these: it adds the
        // origin / auth / session-validation checks, then defers all JSON-RPC
        // parsing and routing here. They are public so the protocol can be unit
        // tested without binding a socket or constructing httplib types (issue
        // #306 item D); they are also the reuse point for any future MCP transport.

        // Route one parsed JSON-RPC message and return the response object, or a
        // null Json for a notification (a message with no "id" — no response is
        // sent). Tools / resources / prompts must already be registered.
        [[nodiscard]] Json HandleMessage(const Json& message);

        // Outcome of running a raw request body through the JSON-RPC framing layer.
        struct FramedResponse
        {
            // Response payload to send back. A null Json means "send no body" (a
            // notification, or an all-notification batch); see Status.
            Json Body;
            // Suggested HTTP status: 200 when Body carries a payload, 202 when
            // there is nothing to return.
            int Status = 200;
            // Non-empty only after a successful single `initialize`: the freshly
            // minted (and registered) session id the transport should echo back in
            // the Mcp-Session-Id header. This is the one stateful side effect of
            // the framing path.
            std::string SessionId;
        };

        // Pure framing: parse `body`, route a single message or a batch through
        // HandleMessage, and collect the response(s). Mirrors the JSON-RPC 2.0
        // batch rules (notifications drop out; an empty batch is itself an invalid
        // request). No httplib — HandlePost wraps this with the transport concerns.
        [[nodiscard]] FramedResponse ProcessRequestBody(const std::string& body);

        // Validate an "Authorization: Bearer <token>" header value against
        // `expectedToken` with a length-independent, content-constant-time compare.
        // An empty `expectedToken` (server not running) rejects everything. Pure.
        [[nodiscard]] static bool CheckBearerAuth(std::string_view authorizationHeader,
                                                  std::string_view expectedToken);

        // DNS-rebinding defence: true if `origin` denotes a loopback host, or is
        // absent / "null" (non-browser agents send no Origin). Pure.
        [[nodiscard]] static bool IsOriginAllowed(std::string_view origin);

      private:
        // Top-level HTTP handler (cpp-httplib worker thread): auth + origin check,
        // JSON-RPC parse, dispatch. Defined in McpServer.cpp.
        void HandlePost(const httplib::Request& req, httplib::Response& res);

        // Dispatch one JSON-RPC message. Returns the response object, or a null Json
        // for notifications (no response is sent).
        [[nodiscard]] Json DispatchRpc(const Json& request);

        [[nodiscard]] Json HandleInitialize(const Json& id, const Json& params);
        [[nodiscard]] Json HandleToolsList(const Json& id) const;
        [[nodiscard]] Json HandleToolsCall(const Json& id, const Json& params);
        [[nodiscard]] Json HandleResourcesList(const Json& id) const;
        [[nodiscard]] Json HandleResourcesRead(const Json& id, const Json& params);
        [[nodiscard]] Json HandlePromptsList(const Json& id) const;
        [[nodiscard]] Json HandlePromptsGet(const Json& id, const Json& params) const;

        [[nodiscard]] const ToolDef* FindTool(const std::string& name) const;
        [[nodiscard]] const ResourceDef* FindResource(const std::string& uri) const;
        [[nodiscard]] const PromptDef* FindPrompt(const std::string& name) const;
        [[nodiscard]] bool CheckAuth(const httplib::Request& req) const;

        EditorMcpContext m_Context;
        std::vector<ToolDef> m_Tools;
        std::vector<ResourceDef> m_Resources;
        std::vector<PromptDef> m_Prompts;

        Scope<httplib::Server> m_Http;
        std::thread m_ListenThread;
        std::atomic<bool> m_Running{ false };
        u16 m_Port = DefaultPort;
        std::string m_Token;

        std::atomic<bool> m_RedactPaths{ false };

        mutable std::mutex m_SessionMutex;
        std::unordered_set<std::string> m_Sessions;
    };
} // namespace OloEngine::MCP
