#pragma once

// The seam an automation command runs against (issue #1123).
//
// Before the extraction every handler took `McpServer&` and could therefore
// reach the whole transport — sessions, consent modals, SSE streams, JSON-RPC
// framing — even though the census showed 96 of 97 handlers used exactly four
// things: the editor context, main-thread marshalling, progress, cancellation.
// `IAutomationHost` is those four. A command written against it can be invoked
// from an MCP tools/call, from a CLI, or from a test that constructs no server
// at all, which is the property #1125 (oloctl) and the headless epics need.
//
// `McpServer` implements this interface; so does any other caller that wants to
// run commands. The interface is deliberately small — adding to it is how the
// transport leaks back in, so a new virtual needs a reason a command genuinely
// cannot express any other way.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace OloEngine::MCP
{
    // The editor capability surface a main-marshaled command reads: a bundle of
    // std::functions EditorLayer fills in (GetActiveScene, CaptureViewportPng,
    // camera control, the consented-write entry points, ...). Despite the name it
    // describes the EDITOR, not a transport — MCP is simply where it was born.
    // Forward-declared here, and still defined in MCP/McpServer.h: this name is
    // the whole of what the automation layer still borrows from that header, and
    // rehoming it is follow-up work, not part of a pure extraction.
    struct EditorMcpContext;
} // namespace OloEngine::MCP

namespace OloEngine::Automation
{
    // A blob a command produced that is too large to inline in its result — a
    // screenshot, a render-target readback, a golden-compare capture. The host
    // stores it and hands back the reference the caller returns instead.
    struct AutomationArtifact
    {
        std::string Uri;
        std::string Name;
        std::string Description;
        std::string MimeType;
        std::vector<u8> Bytes;
    };

    class IAutomationHost
    {
      public:
        IAutomationHost() = default;
        virtual ~IAutomationHost() = default;

        IAutomationHost(const IAutomationHost&) = delete;
        IAutomationHost& operator=(const IAutomationHost&) = delete;
        IAutomationHost(IAutomationHost&&) = delete;
        IAutomationHost& operator=(IAutomationHost&&) = delete;

        // Editor state the main-marshaled commands read. The std::function bodies
        // are ONLY safe to call on the main (game) thread, i.e. from inside a
        // MarshalRead() job. A host with no editor leaves the hooks null, and a
        // command must check before calling one.
        [[nodiscard]] virtual const MCP::EditorMcpContext& Context() const = 0;

        // Marshal a read onto the main (game) thread at the next frame boundary,
        // blocking the calling (handler) thread on the result. The job runs before
        // the scene is stepped that frame, so it observes a consistent snapshot.
        //
        // If the game thread does not service the job within `timeout` (editor
        // stalled / shutting down), this throws std::runtime_error and the caller
        // surfaces it as a command error.
        //
        // MUST NOT be called from the game thread (it would deadlock). Commands only
        // run on handler threads, so this holds.
        //
        // Non-virtual on purpose: the timeout default then lives in exactly one
        // place instead of being re-declared by every override, which is the classic
        // way a default argument on a virtual silently disagrees with itself.
        [[nodiscard]] nlohmann::json MarshalRead(const std::function<nlohmann::json()>& readJob,
                                                 std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
        {
            return MarshalReadOnMainThread(readJob, timeout);
        }

        // Report progress for the CURRENTLY EXECUTING call. A no-op unless the
        // transport asked for progress and provided somewhere to send it.
        // `progress` must increase monotonically per call; `total` < 0 omits the
        // field; an empty `message` omits it. Call from the handler thread — a
        // MarshalRead job runs on the game thread, where the call scope is not
        // visible. Non-virtual for the same reason as MarshalRead.
        void EmitProgress(f64 progress, f64 total = -1.0, const std::string& message = {}) const
        {
            EmitProgressUpdate(progress, total, message);
        }

        // True when the CURRENTLY EXECUTING call has been cancelled. Long-running
        // handlers poll this between frames/steps and abort cleanly; the dispatch
        // layer then discards their result. False outside any call scope, and
        // false on a host with no cancellation channel.
        [[nodiscard]] virtual bool IsCurrentCallCancelled() const = 0;

        // Store `artifact`'s bytes under its Uri so a caller can fetch them later,
        // and return true. Returns false on a host with NO artifact store — the
        // command must then say resource-link delivery is unavailable rather than
        // hand back a link to nothing. Under the MCP server this publishes an
        // ephemeral resource, charged against the ephemeral byte budget.
        [[nodiscard]] virtual bool PublishArtifact(AutomationArtifact artifact) = 0;

      protected:
        virtual nlohmann::json MarshalReadOnMainThread(const std::function<nlohmann::json()>& readJob,
                                                       std::chrono::milliseconds timeout) = 0;
        virtual void EmitProgressUpdate(f64 progress, f64 total, const std::string& message) const = 0;
    };
} // namespace OloEngine::Automation
