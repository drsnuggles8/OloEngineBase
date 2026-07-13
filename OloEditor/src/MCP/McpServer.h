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
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Forward-declare cpp-httplib's types so this header (included by EditorLayer)
// never pulls in <winsock2.h>. The full types are only needed in McpServer.cpp.
namespace httplib
{
    class Server;
    struct Request;
    struct Response;
} // namespace httplib

namespace OloEngine
{
    // The editor's undo/redo stack (OloEditor/src/UndoRedo/EditorCommand.h). Only
    // forward-declared here: EditorMcpContext exposes a pointer to it so consented
    // write tools (issue #306 item C) can route their mutation through a single
    // undoable command, without this header pulling in the editor command classes.
    class CommandHistory;
} // namespace OloEngine

namespace OloEngine::MCP
{
    using Json = nlohmann::json;

    // Default localhost port the MCP server binds to. Configurable in the panel.
    inline constexpr u16 DefaultPort = 7345;

    // JSON-RPC error code for a request abandoned via `notifications/cancelled`
    // (issue #357 item B). MCP does not reserve a code; -32800 is the
    // LSP-established convention for "request cancelled" in the
    // implementation-defined range. Per spec the server SHOULD NOT respond to a
    // cancelled request at all — the SSE transport suppresses the response
    // frame; this code is what the plain-JSON path (which must return SOME
    // body) carries, and clients ignore any response to a cancelled request.
    inline constexpr int kRequestCancelledCode = -32800;

    // How a project-mutating (ToolDef::ProjectWrite) tool call is authorized at
    // dispatch (issue #306 item C). Supersedes the old binary "Allow writes" gate,
    // which maps onto the two extremes (Disabled / AllowSession); the middle mode
    // adds the per-action consent DIALOG. OFF by default and never persisted, so
    // every editor launch starts read-only and the human opts in for the session.
    enum class WriteConsentMode : u8
    {
        Disabled = 0,     // writes refused outright (default) — the read-only posture.
        Prompt = 1,       // each write pops a consent dialog the human approves/denies.
        AllowSession = 2, // writes auto-approved for the rest of the session (no prompt).
    };

    // The resolution of one consent prompt. Pending until the human (or a mode
    // change / server shutdown) resolves it. Approve/Deny/ApproveAll are the three
    // buttons the panel offers; Timeout is set internally when no answer arrives.
    enum class ConsentDecision : u8
    {
        Pending = 0,
        Approve = 1,
        Deny = 2,
        ApproveAll = 3, // approve THIS one and flip the session to AllowSession.
        Timeout = 4,
        Cancel = 5, // a notifications/cancelled reached the call while it was parked on the modal.
    };

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
        // True for a tool that MUTATES the user's project (scene / ECS components /
        // assets) — as opposed to the read-only diagnostics and the ephemeral
        // editor-only camera/viewport/render-override tools. A project-write tool is
        // gated behind the session "Allow writes" toggle: HandleToolsCall rejects it
        // with a clean JSON-RPC error when writes are disabled (the default). Issue
        // #306 item C; should be paired with `readOnlyHint:false` annotations.
        bool ProjectWrite = false;
        // True for a tool registered from a project Lua script (McpScriptTools,
        // issue #357 / ADR 0005) rather than compiled-in. Script tools are
        // replaced wholesale by LoadScriptTools — now safely even while the
        // server is RUNNING, via the copy-on-write tool snapshot (issue #607
        // live-reload item); see ReplaceScriptTools / UnregisterScriptTools.
        bool ScriptOwned = false;
        // Optional MCP `icons` array (SEP-973, spec 2025-11-25): display icons a
        // client may show next to the tool. Each element is an object with a
        // required string `src` (an http(s) or data: URI) plus optional
        // `mimeType` and `sizes` (an array of "48x48"-style strings). Null /
        // empty => omitted from tools/list entirely (never emit an empty
        // `icons` key). Validated by McpServer::IsValidIcons at registration.
        Json Icons;
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

    // Outcome of a consented script reload (issue #306 item C), returned by
    // EditorMcpContext::ReloadScriptAssembly. `Available` is false when C# scripting
    // is not compiled into this build or the engine has no core assembly loaded — the
    // tool then reports that honestly instead of pretending it reloaded. `Ok` is true
    // only when the reload actually ran; `ScriptClassCount` is how many entity-script
    // classes are registered afterwards (a non-zero signal the app assembly loaded).
    struct McpScriptReloadResult
    {
        bool Available = false;
        bool Ok = false;
        std::string Language = "csharp";
        u32 ScriptClassCount = 0;
        std::string Message;
    };

    // Outcome of a consented scene switch (issue #316 Part 5), returned by
    // EditorMcpContext::OpenSceneFromMcp. `Available` is false in a host that owns
    // no editor (the headless attach / dispatch tests) — the tool then reports that
    // honestly. `Ok` is true only when the scene actually loaded; on failure
    // `Message` explains why (bad extension, not found, deserialize failed). `Path`
    // is the resolved scene path, `SceneName` / `EntityCount` describe the scene now
    // active. The load bypasses the editor's auto-save recovery modal (an agent
    // can't click it) and loads the requested file directly.
    struct McpSceneOpenResult
    {
        bool Available = false;
        bool Ok = false;
        std::string Path;
        std::string SceneName;
        u32 EntityCount = 0;
        std::string Message;
    };

    // Outcome of a consented play-mode toggle (issue #316 Part 5), returned by
    // EditorMcpContext::SetScenePlayState. `Available` is false in a host with no
    // editor. `Ok` is true when the editor is in the requested mode afterwards;
    // `Changed` is true only when this call actually transitioned (entering Play can
    // fail — e.g. no primary camera — leaving the editor in Edit with Ok:false).
    // `Playing` is the resulting play state; `SceneName` is the active scene.
    struct McpScenePlayResult
    {
        bool Available = false;
        bool Ok = false;
        bool Playing = false;
        bool Changed = false;
        std::string SceneName;
        std::string Message;
    };

    // ---- Synthetic input injection (olo_input_inject, issue #607) --------------
    // The editor is normally NOT the foreground window when an agent drives it, so
    // OS-level injection (SendInput / SetCursorPos) is both useless and dangerous —
    // it would land in whatever window IS focused. Instead the editor feeds these
    // events into its OWN input stream (the ImGui GLFW backend callbacks, which fan
    // out to both ImGuiIO and the engine's chained Event dispatch). The tool layer
    // builds a frame-quantized plan; the editor drains one frame of it per tick.

    // One primitive synthetic event.
    struct McpInputEvent
    {
        enum class Kind
        {
            MousePos,    // move the cursor to (X, Y)
            MouseButton, // press/release mouse button `Code`
            Key,         // press/release GLFW key `Code`
            Char         // type unicode codepoint `Code`
        };

        Kind Type = Kind::MousePos;
        // MousePos only: OS window CLIENT coordinates, logical pixels, origin
        // top-left — exactly what GLFW's cursor-pos callback receives.
        f32 X = 0.0f;
        f32 Y = 0.0f;
        // MouseButton: GLFW mouse-button index. Key: GLFW key code (== OloEngine
        // KeyCode). Char: unicode codepoint.
        i32 Code = 0;
        // MouseButton / Key only: true = press, false = release.
        bool Down = false;
    };

    // A frame-quantized injection plan. `Frames[i]` is applied at the start of the
    // i-th editor frame after the plan is accepted — one frame per element, never
    // several in one tick. This is load-bearing, not conservatism: ImGui only sees
    // a click if the button is DOWN for at least one full frame (a same-frame
    // press+release never fires IsItemClicked), and the editor's viewport picking is
    // two frames behind the cursor (async PBO readback), so a click must trail the
    // move by several frames to hit the entity the agent aimed at.
    struct McpInputPlan
    {
        std::vector<std::vector<McpInputEvent>> Frames;
    };

    // Outcome of accepting a plan (returned by EditorMcpContext::InjectInput).
    // `Available` is false in a host with no editor window — the tool then reports
    // that honestly instead of pretending it injected.
    struct McpInputInjectResult
    {
        bool Available = false;
        bool Ok = false;
        u64 BaseFrame = 0;  // editor frame index when the plan was accepted
        u32 FrameCount = 0; // frames the plan spans (drains over BaseFrame+1 .. +FrameCount)
        std::string Message;
    };

    // Viewport / window geometry the tool layer needs to turn viewport-relative
    // coordinates into the window-client coordinates the injected events carry.
    // All lengths are LOGICAL pixels except where noted.
    struct McpInputViewportInfo
    {
        bool Available = false;
        // Viewport panel content-area origin, in ImGui screen coordinates.
        f32 PanelX = 0.0f;
        f32 PanelY = 0.0f;
        // The OS window's client-area origin, in the SAME ImGui screen coordinates.
        // With multi-viewport enabled ImGui screen coords are desktop coords, so this
        // is the window position; without it, (0, 0).
        f32 WindowX = 0.0f;
        f32 WindowY = 0.0f;
        // The viewport's logical render size (EditorLayer::m_ViewportSize) and the DPI
        // factor between it and the captured framebuffer. The framebuffer olo_screenshot
        // reads back is LogicalWidth*DpiScale x LogicalHeight*DpiScale physical pixels.
        f32 LogicalWidth = 0.0f;
        f32 LogicalHeight = 0.0f;
        f32 DpiScale = 1.0f;
        // OS window client-area size, logical pixels.
        u32 WindowWidth = 0;
        u32 WindowHeight = 0;
    };

    // Post-injection observation: what the editor looks like once the plan drained.
    // Lets olo_input_inject report the state change a click caused (the whole point
    // of the tool) without a second round-trip.
    struct McpInputStateSnapshot
    {
        bool Available = false;
        bool Pending = false;     // plan frames still queued (the caller gave up waiting)
        u64 SelectedEntityId = 0; // 0 = nothing selected
        std::string SelectedEntityName;
        u64 HoveredEntityId = 0;
        std::string HoveredEntityName;
        bool ViewportHovered = false;
        // Cursor position ImGui currently believes in, in window-client logical pixels.
        f32 MouseX = 0.0f;
        f32 MouseY = 0.0f;
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

        // ---- Consented, undoable project writes (issue #306 item C) ------------
        // The editor's undo/redo stack, so a write tool can apply its mutation as a
        // single undoable command (a Ctrl-Z). Main-thread-only, like the readers
        // above — call it from inside a MarshalRead job, alongside GetActiveScene.
        // Null in a build that does not back the server with an editor (the dispatch
        // tests), and gated at dispatch by the "Allow writes" session toggle.
        std::function<CommandHistory*()> GetCommandHistory;

        // Reload the C# script assembly — the editor's Script ▸ Reload assembly
        // (Ctrl+R) path, ScriptEngine::ReloadAssembly(), so an agent can iterate on
        // C# scripts over MCP without restarting the editor. Like GetCommandHistory
        // this is a consented project-write tool: gated at dispatch by "Allow writes"
        // (reloading runs the user's freshly-built assembly code). Main-thread-only
        // (touches the Mono domain), so call it from inside a MarshalRead job. Null in
        // a headless host that owns no script engine — the tool then reports "not
        // available".
        std::function<McpScriptReloadResult()> ReloadScriptAssembly;

        // Open / switch the active scene — the consented-write scene switch
        // (issue #316 Part 5). Loads the scene at `path` (resolved against the
        // project asset directory when relative) directly, the same install path
        // as the editor's Open Scene menu but WITHOUT the auto-save recovery modal
        // (a remote agent can't click it) and without the file dialog. Stops Play
        // mode first if running. Main-thread-only (touches the EnTT registry /
        // renderer settings), so the server calls it from a MarshalRead job. Like
        // GetCommandHistory this is a consented project-write tool. Null in a
        // headless host that owns no editor scene — the tool then reports "not
        // available".
        std::function<McpSceneOpenResult(const std::string& path)> OpenSceneFromMcp;

        // Toggle Play mode — the consented-write, fully-reversible play/stop switch
        // (issue #316 Part 5). `play` true enters Play (OnScenePlay: copies the
        // scene + starts the runtime), false stops it (OnSceneStop: restores the
        // authored scene). Idempotent — a redundant call is a no-op reported as
        // Changed:false. Main-thread-only (mutates scene state / runs runtime
        // start-stop), so the server calls it from a MarshalRead job. A consented
        // project-write tool (entering Play executes the user's game scripts). Null
        // in a headless host with no editor.
        std::function<McpScenePlayResult(bool play)> SetScenePlayState;

        // ---- Synthetic input injection (olo_input_inject, issue #607) ----------
        // Report the viewport / window geometry the tool needs to resolve
        // viewport-relative coordinates, accept a frame-quantized input plan (queued;
        // drained one frame per editor tick), and read back what the editor looks like
        // afterwards. All three are main-thread-only (ImGui / GLFW / EnTT), so the
        // server calls them from a MarshalRead job. Injection is a consented PROJECT
        // WRITE — a synthetic click can move a gizmo and a synthetic key can delete an
        // entity — so olo_input_inject is ProjectWrite and gated by "Allow writes".
        // Null in a headless host with no editor window; the tool then reports that
        // input injection is unavailable rather than crashing.
        std::function<McpInputViewportInfo()> GetInputViewportInfo;
        std::function<McpInputInjectResult(const McpInputPlan& plan)> InjectInput;
        std::function<McpInputStateSnapshot()> GetInputState;
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

        // Session-level write consent (issue #306 item C). OFF by default (Disabled)
        // and NOT persisted, so every editor launch starts read-only and the user
        // opts in for the session via the MCP panel. Distinct from the enabled +
        // bearer-token gate: even an authenticated agent cannot mutate the project
        // until the human raises this. Read-only / ephemeral-editor-state tools
        // (camera / viewport / render overrides) are never ProjectWrite, so they are
        // unaffected by any mode.
        //
        // In Prompt mode a ProjectWrite dispatch calls RequestConsent, which BLOCKS
        // the handler (worker) thread until the human resolves the modal the panel
        // renders on the main thread — see ResolveConsent / PendingConsents. Setting
        // the mode drains any in-flight prompts: AllowSession approves them, Disabled
        // denies them. Defined in the .cpp (touches the consent queue).
        void SetWriteConsentMode(WriteConsentMode mode);
        [[nodiscard]] WriteConsentMode GetWriteConsentMode() const
        {
            return m_ConsentMode.load();
        }

        // Back-compat binary gate over the mode: true maps to AllowSession (writes go
        // straight through, no prompt), false to Disabled. `AllowWrites()` is "any
        // mode that permits a write" (Prompt or AllowSession). Kept so the existing
        // dispatch tests and any external caller need not learn the tri-state.
        void SetAllowWrites(bool enabled)
        {
            SetWriteConsentMode(enabled ? WriteConsentMode::AllowSession : WriteConsentMode::Disabled);
        }
        [[nodiscard]] bool AllowWrites() const
        {
            return m_ConsentMode.load() != WriteConsentMode::Disabled;
        }

        // A consent prompt awaiting a human decision, as a snapshot for the panel to
        // render. The panel polls PendingConsents() each frame; when non-empty it
        // shows the modal and calls ResolveConsent() on a button press. Main-thread
        // (UI) side of the RequestConsent handshake.
        struct PendingConsent
        {
            u64 Id = 0;
            std::string ToolName;  // e.g. "olo_entity_set_field"
            std::string ToolTitle; // display title (falls back to ToolName)
            std::string Summary;   // human-readable arguments (one "key = value" per line)
        };

        // Snapshot of every prompt currently awaiting a decision, oldest first.
        // Cheap + lock-guarded; safe to call every frame from the main thread.
        [[nodiscard]] std::vector<PendingConsent> PendingConsents() const;

        // Resolve one pending prompt by id (main thread, from the panel). Approve /
        // Deny apply to that prompt only; ApproveAll additionally flips the session
        // to AllowSession and approves every currently-pending prompt. Wakes the
        // blocked handler thread(s). A no-op if the id is already gone.
        void ResolveConsent(u64 id, ConsentDecision decision);

        // How long RequestConsent waits for a human before giving up (Timeout ->
        // dispatch returns a clean error). Default 120s; exposed for tests.
        void SetConsentTimeout(std::chrono::milliseconds timeout)
        {
            m_ConsentTimeoutMs.store(timeout.count());
        }
        [[nodiscard]] const EditorMcpContext& Context() const
        {
            return m_Context;
        }

        // ---- the tool registry: a copy-on-write, atomically swapped snapshot ----
        //
        // Dispatch worker threads read the tool vector LOCK-FREE on the hot path,
        // and script-tool live reload (issue #607) must be able to REPLACE that
        // vector while the server is serving. Both hold only because the vector is
        // immutable once published: every writer (RegisterTool / ReplaceScriptTools,
        // serialized by m_ToolsWriteMutex) builds a fresh vector and atomically
        // stores it; every reader takes a `shared_ptr` snapshot and holds it for as
        // long as it needs the ToolDefs (a whole tools/call, in HandleToolsCall) —
        // which also keeps a *replaced* script tool's Lua runtime alive until the
        // call using it finishes. There is NO lock on the read path.
        //
        // Never hand out a reference into the snapshot's vector: a concurrent swap
        // would leave it dangling the moment the temporary shared_ptr dies. Take a
        // ToolSnapshot and keep it in scope instead.
        using ToolList = std::vector<ToolDef>;
        using ToolSnapshot = std::shared_ptr<const ToolList>;

        [[nodiscard]] ToolSnapshot ToolsSnapshot() const
        {
            return m_Tools.load(std::memory_order_acquire);
        }
        [[nodiscard]] sizet ToolCount() const
        {
            return ToolsSnapshot()->size();
        }
        // Monotonic counter bumped on every tool-list swap (ReplaceScriptTools).
        // The GET /mcp SSE stream polls it and emits a
        // `notifications/tools/list_changed` when it moves, so a connected agent
        // re-lists after a live script rescan.
        [[nodiscard]] u64 ToolsGeneration() const
        {
            return m_ToolsGeneration.load(std::memory_order_acquire);
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

        // Swap every ScriptOwned tool for `scriptTools` in one atomic publish
        // (issue #357 / ADR 0005 + the #607 live-reload item): the new vector is
        // (every non-ScriptOwned tool, in order) + (scriptTools). Safe while the
        // server is RUNNING — readers hold a snapshot, so an in-flight call keeps
        // executing against the tools (and the Lua state) it started with. Bumps
        // ToolsGeneration() and broadcasts `notifications/tools/list_changed` to
        // every registered notification listener and every live SSE stream.
        void ReplaceScriptTools(std::vector<ToolDef> scriptTools);

        // Drop every ScriptOwned tool. Thin wrapper over ReplaceScriptTools({}).
        void UnregisterScriptTools();

        // Optional `icons` for the server itself (SEP-973): emitted under
        // `initialize`'s `serverInfo.icons` only when non-empty. Set before Start()
        // (guarded, but it is startup configuration, not a runtime knob).
        void SetServerIcons(Json icons);

        // Validate an MCP `icons` value (SEP-973 shape): a non-empty array whose
        // every element is an object with a non-empty string `src`, an optional
        // string `mimeType`, and an optional `sizes` array of strings. A null /
        // absent value is valid (it means "no icons") — check `icons.is_null()`
        // separately when you need "present". Pure; exposed for tests and for the
        // Lua registration path.
        [[nodiscard]] static bool IsValidIcons(const Json& icons);

        // ---- Progress + cancellation (issue #357 item B) -----------------------
        //
        // Both utilities are wired through a per-call, per-thread scope that
        // HandleToolsCall installs around each handler invocation, so the 50+
        // existing ToolHandler signatures stay unchanged: a handler that cares
        // simply calls these on its `server` argument; one that doesn't pays
        // nothing.

        // Emit a `notifications/progress` for the CURRENTLY EXECUTING tool call.
        // A no-op unless the call carried `params._meta.progressToken` AND the
        // transport provided a notification sink (the SSE-upgraded POST path, or
        // a ProcessRequestBody caller that passed one). `progress` must increase
        // monotonically per call; `total` < 0 omits the field; `message` empty
        // omits the field. Call from the handler (worker) thread — a MarshalRead
        // job runs on the game thread, where the call scope is not visible.
        void EmitProgress(f64 progress, f64 total = -1.0, const std::string& message = {}) const;

        // True when the CURRENTLY EXECUTING tool call has been cancelled via a
        // `notifications/cancelled` (matched by exact request-id value). Long-
        // running handlers poll this between frames/steps and abort cleanly; the
        // dispatch layer then discards their result per spec. False outside any
        // call scope.
        [[nodiscard]] bool IsCurrentCallCancelled() const;

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

        // Where a dispatched tool call's `notifications/progress` are delivered,
        // called synchronously on the dispatching thread as the tool emits them.
        using NotificationSink = std::function<void(const Json& notification)>;

        // Subscribe to SERVER-INITIATED notifications (today: only
        // `notifications/tools/list_changed`, fired by ReplaceScriptTools). The
        // sink is invoked on whichever thread performed the swap, so it must be
        // cheap and must not re-enter the server. Returns a token for removal.
        //
        // The HTTP transport does NOT use this (a live SSE stream is written from
        // its own worker thread and cannot be poked from another one safely); it
        // polls ToolsGeneration() instead. The listener list exists for embedders
        // and for the headless dispatch tests, which have no socket.
        u64 AddNotificationListener(NotificationSink sink);
        void RemoveNotificationListener(u64 token);

        // ProcessRequestBody with a notification side-channel (issue #357 item
        // B): progress notifications emitted by the dispatched call(s) via
        // EmitProgress flow into `sink` as they occur. The HTTP layer passes a
        // sink that writes SSE frames onto the (upgraded) POST response; tests
        // pass a collector. A null sink behaves exactly like the plain overload
        // (progress is dropped — the server "MAY" simply not send it).
        [[nodiscard]] FramedResponse ProcessRequestBody(const std::string& body, const NotificationSink& sink);

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

        // Validate a tools/call `arguments` object against a tool's declared
        // `inputSchema` BEFORE the handler runs, so a malformed call fails with a
        // clean kInvalidParams naming the offending field instead of reaching the
        // handler's ad-hoc checks (or worse, silently not being checked at all).
        // Supports exactly the JSON-Schema vocabulary the schema-builder DSL
        // (McpSchemaBuilder.h) can emit: object / integer / number / boolean /
        // string / array (incl. a multi-type `type` array), `properties`, `items`,
        // `required`, `enum`, `minimum` / `maximum` / `exclusiveMinimum`,
        // `minItems` / `maxItems`, and `additionalProperties:false`. It is NOT a
        // general JSON-Schema implementation.
        //
        // Returns a human-readable error (e.g. "missing required property
        // 'entity'", "'count' must be <= 200", "unexpected property 'foo'") or
        // std::nullopt when `args` satisfies `schema`. A non-object / empty schema
        // is treated as permissive (returns nullopt — nothing to enforce). Pure.
        [[nodiscard]] static std::optional<std::string> ValidateArguments(const Json& schema, const Json& args);

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

        // Streamable-HTTP upgrade for a progress-opted call (issue #357 item B):
        // HandlePost routes here (after the auth/origin/session gates) when the
        // body is a single tools/call carrying params._meta.progressToken and the
        // client accepts text/event-stream. The response becomes an SSE stream:
        // each notifications/progress the tool emits is written as a frame as it
        // occurs, then the final JSON-RPC response frame — unless the call was
        // cancelled, in which case the response is suppressed per spec and the
        // stream just closes. Defined in McpServer.cpp.
        void HandleStreamingPost(const std::string& body, httplib::Response& res);

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

        // Prompt-mode consent handshake (issue #306 item C). Called from
        // HandleToolsCall on a worker thread for a ProjectWrite tool when the mode is
        // Prompt: enqueue a prompt, block until the panel resolves it (or the timeout
        // / a mode change / shutdown intervenes), and return the decision. MUST run on
        // a handler thread — it blocks on the main thread rendering the modal, so
        // calling it from the game thread would deadlock.
        //
        // `cancelFlag` is the in-flight call's cooperative cancel flag (issue #610): the
        // wait predicate consults it so a `notifications/cancelled` arriving while the
        // call is parked on the modal returns promptly with ConsentDecision::Cancel
        // instead of running to the human's decision or the consent timeout. The
        // cancellation path sets the atomic and wakes m_ConsentCv (see DispatchRpc);
        // the store and this predicate's read are serialized through m_ConsentMutex so
        // there is no lost wakeup. Never null in the dispatch path; may be null in a
        // test that exercises the handshake directly.
        [[nodiscard]] ConsentDecision RequestConsent(const ToolDef& tool, const Json& arguments,
                                                     const std::shared_ptr<std::atomic<bool>>& cancelFlag);

        // Locate a tool inside a snapshot the CALLER holds alive for as long as it
        // uses the returned pointer (see the ToolsSnapshot contract above).
        [[nodiscard]] static const ToolDef* FindTool(const ToolList& tools, const std::string& name);
        [[nodiscard]] const ResourceDef* FindResource(const std::string& uri) const;
        [[nodiscard]] const PromptDef* FindPrompt(const std::string& name) const;
        [[nodiscard]] bool CheckAuth(const httplib::Request& req) const;

        // Bump ToolsGeneration() and fan the tools/list_changed notification out to
        // the registered listeners. Called by ReplaceScriptTools.
        void NotifyToolsListChanged();

        EditorMcpContext m_Context;
        // Copy-on-write, atomically published (see ToolsSnapshot). Writers serialize
        // on m_ToolsWriteMutex; readers are lock-free.
        std::atomic<ToolSnapshot> m_Tools{ std::make_shared<const ToolList>() };
        std::mutex m_ToolsWriteMutex;
        std::atomic<u64> m_ToolsGeneration{ 0 };
        std::vector<ResourceDef> m_Resources;
        std::vector<PromptDef> m_Prompts;

        // Server-initiated notification listeners (AddNotificationListener).
        mutable std::mutex m_ListenerMutex;
        std::vector<std::pair<u64, NotificationSink>> m_Listeners;
        u64 m_NextListenerToken = 1;

        // serverInfo.icons (SEP-973); empty => omitted from initialize.
        mutable std::mutex m_ServerIconsMutex;
        Json m_ServerIcons;

        Scope<httplib::Server> m_Http;
        std::thread m_ListenThread;
        std::atomic<bool> m_Running{ false };
        u16 m_Port = DefaultPort;
        std::string m_Token;

        std::atomic<bool> m_RedactPaths{ false };

        // Session write consent mode (issue #306 item C); see SetWriteConsentMode.
        // Default Disabled (read-only), never persisted — every launch starts safe.
        std::atomic<WriteConsentMode> m_ConsentMode{ WriteConsentMode::Disabled };

        // Prompt-mode consent handshake state. RequestConsent (worker thread) pushes
        // an entry and waits on m_ConsentCv; the panel (main thread) mutates the
        // entry's Decision via ResolveConsent and notifies. m_ConsentAborting is
        // raised by Stop()/the destructor to release any blocked worker as a Deny —
        // distinct from m_Running so the dispatch tests (which never Start the server)
        // can still exercise the handshake.
        mutable std::mutex m_ConsentMutex;
        std::condition_variable m_ConsentCv;
        struct ConsentEntry
        {
            u64 Id = 0;
            std::string ToolName;
            std::string ToolTitle;
            std::string Summary;
            ConsentDecision Decision = ConsentDecision::Pending;
        };
        std::vector<ConsentEntry> m_ConsentQueue;      // guarded by m_ConsentMutex
        u64 m_NextConsentId = 1;                       // guarded by m_ConsentMutex
        bool m_ConsentAborting = false;                // guarded by m_ConsentMutex
        std::atomic<i64> m_ConsentTimeoutMs{ 120000 }; // human-response deadline

        // Count of live GET /mcp SSE push streams (issue #306 item B). Bumped while a
        // stream's content provider runs; surfaced via ActiveStreamCount().
        std::atomic<int> m_ActiveStreams{ 0 };

        mutable std::mutex m_SessionMutex;
        std::unordered_set<std::string> m_Sessions;

        // In-flight tools/call registry for `notifications/cancelled` (issue #357
        // item B): canonical request-id (Json::dump — exact-value matching, so
        // "5" and 5 stay distinct) -> the cooperative cancel flag the running
        // call's scope polls. Entries live exactly as long as their dispatch.
        mutable std::mutex m_InFlightMutex;
        std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> m_InFlightCalls;
    };
} // namespace OloEngine::MCP
