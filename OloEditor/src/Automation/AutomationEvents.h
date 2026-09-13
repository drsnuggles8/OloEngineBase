#pragma once

// The publish side of the automation event bus (issue #1131).
//
// The bus itself is the engine's diagnostics ring (Debug/DiagnosticsEventLog.h):
// one fixed window, monotonic ids, a cursor per consumer, a reported gap, and a
// condition variable to block on. This header is the editor-side vocabulary over
// it — one function per event the issue named, so a call site says WHAT happened
// and this file decides how it is spelled, what its payload carries, and what it
// deliberately leaves out.
//
// Consumers, all riding the same ring and the same cursor:
//   * olo_events_wait  — the long-poll subscription (blocks for the next match);
//   * olo_events_tail  — the incremental poll;
//   * olo://events/recent + resources/subscribe — the era-proof push carrier;
//   * the GET /mcp SSE stream — the legacy push carrier;
//   * `oloctl events follow` — the CLI consumer, a loop over olo_events_wait.
//
// THE AUTHORITY RULE, stated once. A subscription is a read, at the authority of
// any read-only command: it needs the session's bearer token and nothing more, it
// is not consent-gated, and it is offered to read-only frontends. That is only
// sound because of what an event may CARRY — see DiagnosticEventDataKeys in the
// engine header: identities (a command name, a scene name, a project-relative
// path, an outcome), never content (arguments, results, YAML, bytes, absolute
// paths). Every publisher here builds its payload through that closed key set,
// paths are made project-relative before they are recorded (ProjectRelativePath),
// and every MCP carrier applies the session's path redaction on the way out — the
// SSE stream did not before #1131, which was a real leak of what a redacted tool
// result hides. A subscriber therefore cannot see through a read gate, because
// nothing behind a read gate is ever put on the bus.
//
// BACKPRESSURE, stated once. There is no per-subscriber queue anywhere. A slow
// or absent subscriber costs the editor nothing; when it resumes past the window
// it is told how many records it lost (`dropped`). The push stream's only buffer
// is the kernel socket buffer, and a client that stops reading fails its write
// and is disconnected.

#include "Automation/AutomationCommand.h"
#include "Automation/AutomationResult.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace OloEngine::Automation::Events
{
    // A path as an event may carry it: relative to `root` when it lies inside,
    // otherwise the file name alone. Generic (forward-slash) form, so the same
    // asset spells the same on every platform and never reveals a drive letter or
    // a home directory. Empty in, empty out.
    [[nodiscard]] std::string ProjectRelativePath(const std::filesystem::path& path, const std::filesystem::path& root);

    // The authored scene was written to disk. `changed` is false when the bytes on
    // disk were already identical (the save was a no-op that only re-marked the
    // history clean). Returns the event id, or 0 when suppressed.
    u64 PublishSceneSaved(std::string_view sceneName, const std::filesystem::path& path,
                          const std::filesystem::path& projectRoot, bool changed);

    // The scene's unsaved-changes state flipped. Recorded on BOTH edges (dirty and
    // clean again) under one category, with Data.dirty telling them apart, so a
    // subscriber filtering on `scene_dirty` sees every transition.
    u64 PublishSceneDirty(std::string_view sceneName, bool dirty);

    // An asset was registered. `source` names the path it came in by: "filewatch"
    // for the content-directory watcher's auto-import, "automation" for
    // olo_asset_import.
    u64 PublishAssetImported(u64 handle, std::string_view assetType, const std::filesystem::path& path,
                             const std::filesystem::path& projectRoot, std::string_view source);

    // A compile ended. `kind` is "build" (a CMake target through olo_build_run),
    // "script" (the C# assembly), or "shader" (a shader library reload); `target`
    // names what was compiled. Counts and seconds are 0 when the path does not
    // measure them. Forwards to DiagnosticsEventLog::RecordCompileFinished, which
    // the two engine-side compile paths call directly.
    u64 PublishCompileFinished(std::string_view kind, std::string_view target, bool ok, u32 errors, u32 warnings,
                               f64 seconds);

    // Whether running `command` records a CommandCompleted event. THE RULE: every
    // command that is not annotated read-only does; a read-only one never does.
    // Two reasons, both load-bearing. A read is not "something that happened" to
    // the project, and a session that polls a dozen reads a second would evict
    // the events worth waiting for from a 512-record ring. And the subscription
    // command itself (olo_events_wait) is read-only: if its completion were an
    // event, two agents waiting on each other's activity would wake each other
    // forever. A command with no annotations at all is treated as not read-only,
    // which is the conservative side (an event too many, never one too few).
    [[nodiscard]] bool EmitsCompletionEvent(const AutomationCommand& command);

    // A mutating automation command finished (see EmitsCompletionEvent). Carries
    // the command's name, toolset, authority class, outcome and duration — and
    // NOT its arguments or result, which are exactly the content the authority
    // rule keeps off the bus.
    u64 PublishCommandCompleted(const AutomationCommand& command, const AutomationResult& result, f64 durationMs);
} // namespace OloEngine::Automation::Events
