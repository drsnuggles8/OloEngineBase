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
//   * Read-only with respect to the user's PROJECT: no tool writes scenes,
//     assets, or files. Tier-0 inspection tools (issue #316) may adjust
//     editor-only viewport state — the editor camera pose and the viewport
//     capture size — which is never persisted.
//
// Threading: cpp-httplib handles each request on its own worker thread. The
// already-FMutex-guarded diagnostics (Profiler / MemoryTracker / ShaderDebugger /
// Log) may be read directly from a handler thread ("lock-safe"). The EnTT
// registry is NOT thread-safe, so any Scene / entity read must be marshaled onto
// the main (game) thread at a frame boundary via MarshalRead() ("main-marshaled").

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
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
    //
    // `StructuredContent` is the optional typed result (MCP `structuredContent`,
    // spec 2025-06-18): a JSON object an agent can parse directly instead of
    // scraping the `content` text. It is null (omitted) for inherently-textual
    // tools; a tool that sets it should also declare a matching `ToolDef::OutputSchema`.
    // Per the spec, when `structuredContent` is present `content` must still carry a
    // backward-compatible serialized mirror — `Structured()` builds both at once.
    struct ToolResult
    {
        Json Content = Json::array();
        bool IsError = false;
        Json StructuredContent; // null => omitted; must be a JSON object when set.

        [[nodiscard]] static ToolResult Text(const std::string& text);
        [[nodiscard]] static ToolResult Error(const std::string& message);
        // Typed success result: sets `StructuredContent` to `data` (must be a JSON
        // object) and mirrors it into `content` as pretty-printed text for clients
        // that don't read structured output. `IsError` stays false.
        [[nodiscard]] static ToolResult Structured(const Json& data);
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
        // Optional human-friendly display name (MCP Tool.title, spec 2025-06-18).
        // Clients prefer it over `Name` for display (precedence: title >
        // annotations.title > name). Emitted as the top-level `title` only when
        // non-empty.
        std::string Title;
        std::string Description;
        Json InputSchema;
        // Optional JSON Schema for the tool's structured result (MCP `outputSchema`,
        // spec 2025-06-18). Emitted under `outputSchema` in tools/list only when it is
        // a non-empty object; pairs with a handler that returns ToolResult::Structured.
        // Default-null tools stay text-only and omit the field.
        Json OutputSchema;
        // Optional MCP ToolAnnotations object — behavioural hints the client may
        // use to e.g. auto-approve a read-only tool: `readOnlyHint`,
        // `destructiveHint`, `idempotentHint`, `openWorldHint`. Defaults to null;
        // emitted under `annotations` only when it is a non-empty object.
        Json Annotations;
        // Lightweight grouping category (e.g. "render", "physics", "shader") so the
        // 39-tool surface can be browsed/filtered instead of paged through flat. Used
        // by `tools/search` (filter + catalogue) and surfaced under each tool's `_meta`
        // in `tools/list`. Empty => uncategorized (omitted from the metadata). Purely
        // descriptive — it does not affect dispatch or tool resolution.
        std::string Toolset;
        ToolHandler Handler;
        bool MainMarshaled = false;
    };

    // Snapshot of the editor camera's full pose, returned by GetCameraPose and
    // accepted by RestoreCameraPose. Angles are radians (EditorCamera's units);
    // FOV is vertical degrees. Distance 0 means a collapsed orbit (free pose).
    struct McpCameraPose
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 FocalPoint{ 0.0f };
        glm::vec3 Forward{ 0.0f, 0.0f, -1.0f };
        f32 Distance = 0.0f;
        f32 YawRadians = 0.0f;
        f32 PitchRadians = 0.0f;
        f32 FovDegrees = 45.0f;
        f32 NearClip = 0.1f;
        f32 FarClip = 1000.0f;
        u32 ViewportWidth = 0;
        u32 ViewportHeight = 0;
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

        // ---- Tier-0 camera / viewport control (issue #316) -----------------
        // All main-thread-only, like the readers above. These mutate editor-only
        // inspection state (never the project): the EditorCamera pose and the
        // viewport's capture-size override.

        std::function<McpCameraPose()> GetCameraPose;
        // Pose the camera at an explicit eye position looking along yaw/pitch
        // (radians); fovDegrees <= 0 keeps the current FOV. Collapses the orbit
        // (EditorCamera::SetPose) so the pose renders exactly as requested.
        std::function<void(const glm::vec3& eye, f32 yawRadians, f32 pitchRadians, f32 fovDegrees)> SetCameraPose;
        // Orbit-frame the camera around `target` (EditorCamera::Focus).
        std::function<void(const glm::vec3& target, f32 yawRadians, f32 pitchRadians, f32 distance)> OrbitCamera;
        // Restore a previously captured pose (focal point / distance / angles /
        // FOV) — the save/restore half of multi-angle screenshot capture.
        std::function<void(const McpCameraPose& pose)> RestoreCameraPose;
        // Orbit-frame the camera on the entity with this UUID so it fills the
        // view. Returns false when the entity doesn't exist in the active scene.
        std::function<bool(u64 entityUuid)> FrameEntity;
        // Override the viewport's logical size (deterministic capture
        // resolution); width/height 0 clears the override and returns the
        // viewport to the ImGui panel size.
        std::function<void(u32 width, u32 height)> SetViewportSizeOverride;
        // Monotonic editor frame counter + whether the framebuffer is currently
        // NOT capture-ready: the last frame skipped scene rendering (frame-budget
        // throttle), or a viewport resize happened within the last few frames
        // (resized render-graph framebuffers render black for a couple of frames).
        // Together these let a tool wait until a camera change has actually been
        // rendered before capturing.
        std::function<u64()> GetFrameIndex;
        std::function<bool()> IsCaptureUnready;
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
        // Number of live server-push SSE streams currently connected on GET /mcp
        // (issue #306 item B). Surfaced in the panel so the user can see an agent is
        // watching; lock-free, safe to read from any thread.
        [[nodiscard]] int ActiveStreamCount() const
        {
            return m_ActiveStreams.load(std::memory_order_relaxed);
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
        // user copy-pasting. Removed on Stop. Resolution order:
        //   1. The OLO_MCP_DISCOVERY_FILE env var, verbatim, when set & non-empty —
        //      the launching tool (e.g. the run-oloengine skill) picks the exact path
        //      it reads back, so parallel worktrees never collide regardless of port.
        //   2. Otherwise the OS temp dir. The default port keeps the legacy
        //      `oloengine-mcp.json` name (back-compat for the panel / manual attach);
        //      any other port namespaces the file as `oloengine-mcp-<port>.json` so
        //      two editors on distinct ports don't clobber each other's host/token.
        // Returns an empty string only if the temp dir can't be resolved and no
        // override is set.
        [[nodiscard]] static std::string DiscoveryFilePath(u16 port = DefaultPort);

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

        // Validate a tool name against the MCP registration contract: length 1..128
        // and every character in [A-Za-z0-9_.-]. A malformed name is a programmer
        // error (tools are registered at startup), which RegisterTool surfaces
        // loudly via OLO_CORE_VERIFY. Exposed as a pure helper so it can be unit
        // tested without tripping that assert. Pure.
        [[nodiscard]] static bool IsValidToolName(std::string_view name);

      private:
        // Top-level HTTP handler (cpp-httplib worker thread): auth + origin check,
        // JSON-RPC parse, dispatch. Defined in McpServer.cpp.
        void HandlePost(const httplib::Request& req, httplib::Response& res);

        // GET /mcp handler: opens a persistent text/event-stream (SSE) and pushes
        // newly-recorded diagnostics events to the agent as MCP JSON-RPC
        // notifications (issue #306 item B). Same origin + bearer + session gates as
        // HandlePost; then it streams on the worker thread until the client
        // disconnects or the server stops. Defined in McpServer.cpp.
        void HandleGetStream(const httplib::Request& req, httplib::Response& res);

        // Dispatch one JSON-RPC message. Returns the response object, or a null Json
        // for notifications (no response is sent).
        [[nodiscard]] Json DispatchRpc(const Json& request);

        [[nodiscard]] Json HandleInitialize(const Json& id, const Json& params);
        [[nodiscard]] Json HandleToolsList(const Json& id) const;
        // Custom (non-standard) discovery method `tools/search`: filter the tool
        // surface by a free-text `query` (whitespace-separated terms, all must match
        // name/title/description/toolset, case-insensitive) and/or a `toolset`
        // category, and return the matching tools plus a catalogue of every toolset
        // with its tool count. Additive: `tools/list` is unchanged, so a standard MCP
        // client that never calls this keeps working.
        [[nodiscard]] Json HandleToolsSearch(const Json& id, const Json& params) const;
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

        // Count of live GET /mcp SSE push streams (issue #306 item B). Bumped while a
        // stream's content provider runs; surfaced via ActiveStreamCount().
        std::atomic<int> m_ActiveStreams{ 0 };

        mutable std::mutex m_SessionMutex;
        std::unordered_set<std::string> m_Sessions;
    };
} // namespace OloEngine::MCP
