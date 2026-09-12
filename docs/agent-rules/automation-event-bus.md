# Put identities on the event bus, never content, and subscribe with a cursor, never a queue

Rules for the automation event bus (issue #1131), which is the engine's diagnostics ring
(`Debug/DiagnosticsEventLog.h`) plus a long-poll command over it. Both rules exist because the
alternatives each failed a design constraint the issue stated, and one of them had already
failed silently in the code that shipped before.

## 1. An event carries the identity of what happened, not what a read would return

**Rule.** A record on the bus names the thing (a command name, a scene name, a project-relative
path, an entity id, an outcome) and never carries the content a read command would return for
it: no arguments, no results, no serialized scene, no file bytes, no absolute path. The keys a
category may carry are a closed table (`DiagnosticEventDataKeys`), `Record()` refuses any other
key, and string values are cut at 256 characters. Publish through `Automation/AutomationEvents.h`,
which builds every payload from that table and makes paths project-relative first.

**Why.** The issue's constraint was that a subscription is an authority-bearing capability
because "events can leak state a read tool would have gated". The census found nothing that
gates a read today beyond the bearer token, so the rule was made structural instead: keep
gated content off the bus and there is nothing for a subscriber to see through. That is what
lets `olo_events_wait` be a plain read-only command and lets `oloctl`, which refuses every write,
follow the stream.

**What had already leaked.** `tools/call` and `resources/read` scrub absolute paths when the
session's redaction toggle is on. The `GET /mcp` push stream did not: it serialized each record
straight from the ring, so the same `asset_reload` record arrived redacted through
`olo_events_tail` and unredacted through the stream. Every carrier now applies the redaction
(`ServiceEventStream` takes it as a parameter). When you add a carrier, route it through the
same function.

## 2. A subscriber holds a cursor; the ring is the only window

**Rule.** There is no per-subscriber buffer. A consumer holds the last event id it saw and asks
for what came after it (`olo_events_tail`, `olo_events_wait`, `Last-Event-ID` on the stream,
`oloctl events follow`). The ring keeps the newest 512 records and evicts the rest whether or not
anyone read them. A consumer whose cursor is older than the oldest retained record is told how
many it lost (`dropped`), and must treat that as a hole in its history, not continue as if it
were complete.

**Why.** The issue's second constraint was that "a subscriber that stops reading must not grow
the editor's memory without bound". A queue per subscriber answers it with a cap and a drop
policy per queue; a shared cursor answers it with no memory at all and the same drop policy,
counted. The gap count is the part that was missing: before #1131 a cursor that fell behind got a
silently shortened history from `QueryWithCursor`, and the stream's "the oldest may already have
been evicted" was a sentence in the guide rather than a number in the response.

## 3. Only a mutating command publishes `command_completed`

**Rule.** `AutomationRegistry::RunHandler` publishes a completion event for every command that is
not annotated `readOnlyHint: true`. A read-only command never publishes one, and a command with
no annotations counts as mutating.

**Why.** Two reasons, and the second is the one to remember. A session that polls a dozen reads a
second would evict everything worth waiting for from a 512-record ring. And the subscription
command is itself read-only: if its completion were an event, two agents each waiting for "the
next event" would wake each other forever. If you add a read-only command with no annotations,
it will publish; give it `ReadOnlyAnnotations()`.

## 4. Emission sites, so a new event lands where the others do

| Event | Published from | Path it covers |
|---|---|---|
| `scene_save` | `Automation::SaveSceneDocument` | menu, Ctrl+S, `olo_scene_save`, `olo_scene_save_as` |
| `scene_dirty` | `CommandHistory::OnDirtyChanged`, wired once in `EditorLayer` | every edit, undo, redo, save and transaction on the scene history |
| `asset_import` | `EditorLayer::OnAssetImported` and the `olo_asset_import` handler | the file watcher does not fire the `AssetImportedEvent` for a command import, so both publish |
| `compile_finished` | `ScriptEngine::ReloadAssembly`, `ShaderLibrary::ReloadShaders`, `Handle_BuildRun` | `kind` says which: `script`, `shader`, `build` |
| `command_completed` | `AutomationRegistry::RunHandler` | every frontend, since every one runs a handler through it |

A new site must record under an existing category or add one to the enum, the token maps, the
`kDiagnosticEventCategoryCount` constant, the closed key table, and the two exhaustiveness tests
(`McpEventsTailTest`, `McpEventStreamTest`). The compiler catches the switch statements; the
tests catch the tables.
