# Script-defined MCP tools run user-authored Lua in a dedicated, capability-stripped `sol::state` with a tool-composition bridge — not engine bindings, not C#, not eval-over-MCP

Issue [#357](https://github.com/drsnuggles8/OloEngineBase/issues/357)'s last blocked
item is **script-defined MCP tools**: let a game developer add project-specific
diagnostics tools to the editor's MCP server without recompiling the engine —
OloEngine's answer to UE's Blueprint-authored MCP tools. It was blocked on a
sandboxing design, because it sits exactly on the server's security boundary:
the MCP server is a **localhost, bearer-token, read-only-by-default diagnostics
endpoint** (see the security posture in
[`McpServer.h`](../../OloEditor/src/MCP/McpServer.h)), and "scripts register
network-reachable endpoints" sounds like a hole in that posture unless the
design says precisely what a script tool can and cannot do.

## Threat model

What actually changes when script tools exist:

1. **The scripts themselves are user-authored local files** loaded from the
   opened project (`<project assets>/McpTools/*.lua`). They are the same trust
   tier as the game's own entity Lua scripts, which already execute arbitrary
   code inside the editor process whenever Play mode runs. The MCP server
   **never accepts tool *definitions* over the network** — there is no
   "register this Lua source" RPC, and `tools/call` arguments are data, never
   code. So we are *not* defending against hostile tool authors; we are
   containing three real risks:
2. **The network caller abusing a script tool's capabilities.** An agent
   holding the bearer token can invoke any registered tool with any
   schema-passing arguments. Whatever ambient authority a script tool has, the
   agent has. The sandbox therefore bounds a script tool's authority to
   *read-only diagnostics*, keeping the server's read-only-by-default promise
   even if the token leaks.
3. **Accidental damage.** A buggy script (infinite loop, huge string
   concatenation, error mid-tool) must not hang a dispatch worker forever,
   crash the editor, or corrupt another in-flight tool call.
4. **Confused-deputy file/process access.** A script tool that could reach
   `io` / `os` / `package` would let a remote *caller* read arbitrary files or
   spawn processes through an innocently-written script ("pass me a path and
   I'll dump it"). Strip the capability rather than audit every script.

Out of scope (documented, not defended): an agent that already has filesystem
write access to the project (e.g. a local coding agent) can author a `.lua`
tool and get it loaded on the next scan — but such an agent can edit the game's
own scripts/code anyway; MCP adds no new authority. That remains true now that
scripts reload LIVE and can declare a write tier: a script tool it authors is
still gated by the human's session write-consent mode before it can mutate
anything (see §5).

> **Status (issue #607).** The four items this ADR originally deferred —
> write-tier script tools, live reload, the memory quota, and tool icons — have
> all landed. Each section below now describes the shipped behaviour and calls
> out what changed from v1.

## Decision

### 1. A dedicated, capability-stripped `sol::state` owned by the MCP layer

Script tools run in their **own** Lua state
(`OloEditor/src/MCP/McpScriptTools.{h,cpp}`), never in the gameplay state that
`LuaScriptEngine` owns:

- **Libraries:** `base`, `math`, `string`, `table` only — the same restricted
  set the gameplay state opens — and then the file/code-loading escape hatches
  are removed from `base`: `dofile`, `loadfile`, `load`, and `require` are
  nil'd out. No `io`, no `os`, no `debug`, no `package`. `print` is rebound to
  the engine log (tagged `[McpScriptTool]`) so scripts can trace without
  gaining I/O.
- **Why a separate state:** thread isolation (the gameplay state is touched by
  the game thread; MCP handlers run on httplib workers), lifecycle isolation
  (reloading tool scripts must not disturb entity scripts and vice versa), and
  no shared globals between network-reachable code and gameplay code.
- **Threading:** one `std::mutex` serializes every entry into the MCP Lua
  state. Handlers already run on worker threads; the game thread never touches
  this state, so the mutex can never participate in a lock-order inversion
  with the game-thread task queue.
- **Memory (issue #607):** the state is created with a custom `lua_Alloc`
  (`LuaQuotaAlloc`) that tracks live bytes and **refuses** any allocation that
  would cross a per-state budget (`kDefaultScriptToolsMemoryBudget`, 64 MiB;
  a parameter of `LoadScriptTools`). A refused allocation is an ordinary Lua
  memory error inside a protected call, so an allocation bomb
  (`while true do t[#t+1] = string.rep('x', 1e6) end`) fails **its own call**
  with `isError:true` and leaves the editor standing — the watchdog bounds
  *time*, this bounds *space*. The quota is armed **after** the libraries and
  the bridge are built, so a small budget can never starve the interpreter's
  own boot; it is headroom for the scripts, not a total.

### 2. Capability = pure compute + a tool-composition bridge, tiered by declaration

A script tool's handler receives its (schema-validated) arguments as a Lua
table and returns a table/string/number that is converted back to JSON. Its
only window into the engine is:

- `olo.call_tool(name, args)` — invoke another **registered** tool through the
  same argument validation a network caller passes, returning the tool's
  structured result (or `nil, errorMessage`). Whether a **ProjectWrite** target
  is reachable is governed by the tier rule in §5.
- `olo.log(message)` — write to the engine log.

No engine usertypes (`Entity`, `TransformComponent`, …) are bound into the MCP
state in v1. Everything a script tool can observe, it observes through tools
that already exist and already honor the read-only/consent rules — the sandbox
adds **zero new engine attack surface**, and the interesting authoring niche
("aggregate three diagnostics into one project-specific digest") is exactly
composition.

### 3. Registration: load at server start, under a reserved namespace

- Scripts are loaded from `<project assets>/McpTools/*.lua` via
  `MCP::LoadScriptTools(server, dir)` — at editor init, and (issue #607) at any
  time afterwards, **including while the server is serving**. Each load has
  **replace semantics**: the scan collects the new `ToolDef`s and publishes them
  in ONE swap (`McpServer::ReplaceScriptTools`), so edits, additions AND
  deletions all take effect and no half-rescanned surface is ever observable.
- **Live reload, and how the lock-free read path survives it.** The registry is
  a `std::atomic<std::shared_ptr<const std::vector<ToolDef>>>`: writers
  (`RegisterTool` / `ReplaceScriptTools`, serialized by a write mutex) build a
  fresh vector and store it; readers take a `shared_ptr` **snapshot** — there is
  **no lock on the dispatch hot path**. `HandleToolsCall` pins its snapshot for
  the whole call, which is what makes a mid-call swap safe twice over: the
  `ToolDef*` cannot dangle, and the superseded tools keep the previous
  `sol::state` alive (their handlers own a `shared_ptr` to it) until the last
  call using it returns. Nothing may hand out a *reference* into the snapshot's
  vector — hence `Tools()` was replaced by `ToolsSnapshot()`.
  Triggers: the `olo_script_tools_reload` tool (so an agent can iterate on its
  own tools) and the MCP panel's "Reload script tools" button. On success the
  server bumps a generation counter and emits
  `notifications/tools/list_changed` — every live `GET /mcp` SSE stream polls
  the counter and pushes the notification (an SSE `DataSink` may only be written
  from its own worker thread, so a cross-thread push would be a data race).
  `initialize` therefore now advertises `capabilities.tools.listChanged: true`.
  *Not* done: watching the directory with filewatch — an editor-thread
  auto-rescan on every keystroke-save would republish the surface (and rotate
  the Lua state) at times no agent asked for; the explicit trigger is one call.
- Each script calls `RegisterMcpTool{ name=, title=, description=, schema=,
  handler=, toolset?, writes?, icons? }` (a global the loader injects). Names must match
  `script_[a-z0-9_]+` — a reserved prefix distinct from the native `olo_*`
  namespace, so a script can never shadow or spoof a built-in tool (the
  existing P5a name validation still applies on top). Default toolset:
  `"script"`, so `tools/list` / `tools/search` callers can see at a glance
  which tools are user-authored. Duplicate names and malformed
  schemas/handlers are rejected with a logged error — **registration failures
  never assert** (scripts are runtime data, unlike the compile-time native
  registrations, whose malformed names remain a loud `OLO_CORE_VERIFY`).
- The declared `schema` table is converted to JSON Schema and stored as
  `ToolDef::InputSchema`, so the server-side argument enforcement (#423) and
  the schema-first `tools/list` contract apply to script tools unchanged.

### 4. Failure containment

- Handlers run as `sol::protected_function`; any Lua error becomes a clean
  `isError:true` tool result carrying the Lua message — never an exception out
  of dispatch, never a crash.
- A **watchdog debug hook** (`lua_sethook`, count mask) trips inside
  long-running Lua execution: it errors the script out when the per-call
  wall-clock budget (default 10 s) is exceeded **or** the call has been
  cancelled via MCP `notifications/cancelled` — so a runaway loop cannot pin a
  dispatch worker, and script tools get cooperative cancellation for free.
  (The hook only fires while Lua code runs; a script blocked inside a native
  `olo.call_tool` is bounded by that tool's own MarshalRead timeout instead.)
- Script results that cannot be converted to JSON (functions, userdata,
  cycles) are reported as tool errors, not silently mangled.

### 5. Consent & writes — the tier rule (issue #607)

A script tool declares its tier at registration. `writes = true` sets
`ProjectWrite = true` and `readOnlyHint:false`; anything else is the read-only
default (`ProjectWrite = false`, `readOnlyHint:true`). **No new consent code
exists**: `HandleToolsCall`'s gate is driven purely off `ToolDef::ProjectWrite`,
so a write-tier script tool is refused outright when the session mode is
Disabled (the default), raises the per-action modal in Prompt, and runs straight
through in Allow-all — exactly like a native write tool, with the modal naming
the *script tool* and its arguments.

The rule the bridge enforces:

> **A tool's write authority is its OWN declared tier, evaluated at the moment
> of the call. It is never inherited, never ambient, and never laundered.**

Concretely, `olo.call_tool` admits a `ProjectWrite` target only when **both**
hold:

1. the script tool **currently executing** declared `writes = true`
   (`ScriptToolsRuntime::CallMayWrite`, installed by that handler's
   `WatchdogScope` and *restored* when a nested call unwinds — so a nested
   read-only tool runs read-only even when a write-tier tool called it, and the
   caller gets its own authority back afterwards); **and**
2. the session **still** permits writes at that instant (`AllowWrites()`), which
   is re-checked at each inner call rather than trusted from the dispatch-time
   gate — so flipping the panel back to Disabled stops a long-running macro's
   remaining writes.

Condition (1) is also what closes the escalation path, and it does so
*structurally* rather than by an extra check: **a write-tier script tool is
itself `ProjectWrite`**, so a read-only script tool trying to reach a mutation
*through* one is refused by the very same test that refuses the direct call.
There is therefore no sequence of `olo.call_tool` hops that reaches a mutation
from a read-only entry point — the `readOnlyHint:true` a read-only script tool
advertises remains a **guarantee**, not a claim, which is what v1's one-sentence
security argument was really protecting.

What the human is consenting to, honestly stated: **the macro, not each step
inside it.** Approving `script_perf_lite_mode` approves the three renderer
writes it is documented to make; the modal cannot enumerate them, because the
handler is Lua and the calls it makes are data-dependent. This is the same
bargain as approving any native write tool whose effect is described by its
name + arguments, and it is why the tier must be *declared* (visible in
`tools/list` as `readOnlyHint:false`) rather than inferred.

Rejected alternative: **re-prompt for every inner ProjectWrite call.** It would
make the consent literal, but it turns a three-write macro into three modals
(the exact ergonomics macros exist to remove), it leaks the script's internals
into the dialog, and it buys no safety the tier rule doesn't already provide —
the caller could equally have made those three native calls itself, each with
its own modal. The tier is the consent boundary.

## Considered options

- **Dedicated capability-stripped state + composition bridge (chosen).** See
  above. Smallest new attack surface; the security boundary remains the
  existing tool dispatch; useful immediately for aggregation/digest tools.
- **Bind the gameplay usertypes (`LuaScriptGlue::RegisterAllTypes`) into the
  tool state.** Maximum expressive power — and an immediate violation of the
  read-only posture: the gameplay bindings exist to *mutate* entities
  (`SetPosition`, physics impulses, …). Auditing that surface into a read-only
  subset is a large, error-prone allowlist nobody asked for yet. Rejected for
  v1; revisit only with concrete demand, and then as an explicit read-only
  binding set, not a pass-through.
- **Share the gameplay `sol::state`.** No second interpreter, and scripts
  could reach entity tables directly — but it is touched by the game thread
  (data race with handler threads), its lifetime is coupled to
  scene/play-mode transitions, and a network-reachable tool would share
  globals with game code. Rejected.
- **Accept tool definitions over MCP (an agent registers a tool by sending
  Lua source).** The UE-style dynamic authoring loop — and a textbook
  eval-over-the-network hole: the bearer token would become "execute arbitrary
  code in the editor". Rejected outright; tool *definitions* stay local files
  under the user's version control.
- **C# script tools (Mono).** The other half of the original #357 wish.
  Rejected for v1 and deferred as its own issue: modern Mono/.NET has no
  supported in-process sandbox (AppDomain sandboxing is gone), so a C# tool
  is unrestricted engine code — the only honest designs are "trust it
  completely" (defeats the read-only posture) or out-of-process isolation
  (a much bigger architecture). Lua's capability model is exactly the cheap,
  strippable sandbox this needs; C# authors can already write native-tier
  tools by recompiling.
- **Load scripts live with `listChanged` notifications.** Originally deferred;
  **shipped in #607** (see Decision 3). The registry became a copy-on-write
  atomic snapshot rather than a locked vector, so the concurrency cost landed
  entirely on the *write* side and the dispatch read path stayed lock-free.
- **A `RegisterMcpTool`-time consent declaration instead of a tier flag** (e.g.
  a per-tool "always ask" string). Rejected: it would put a *second*, script-only
  consent mechanism next to the session mode the panel already owns. The tier
  flag reuses the existing gate verbatim — the whole write-tier feature adds zero
  lines of consent code.

## Consequences

- The read-only posture survives with a sharper edge: a script tool is
  read-only **unless it says otherwise in its own registration**, and a
  read-only one is *provably* unable to reach a mutation by any composition
  path (§5). A write-tier one is a normal `ProjectWrite` tool as far as consent
  is concerned — the human's session mode governs it, and Ctrl-Z reverts what it
  routed through the undo stack.
- `tools/list` is truthful in the other direction now: `listChanged:true`,
  because a reload genuinely republishes the surface and pushes
  `notifications/tools/list_changed`.
- A buggy or malicious *call* can waste at most one watchdog budget and one
  memory quota on one worker thread; the editor, the other workers, and the
  gameplay Lua state are unaffected. An allocation bomb now fails its own call
  instead of OOM-ing the process.
- The bridge means script tools inherit every future native tool for free —
  including, for a write-tier tool, the native *write* tools; a script macro can
  do nothing a consenting caller could not have done itself with the same native
  calls. Remaining follow-ups (engine read-bindings, C# tools) stay on the
  tracker.
- Tests exercise the whole path headlessly: `MCP::LoadScriptTools` pointed at a
  temp dir of test scripts, then JSON-RPC dispatch through the in-process server
  — no editor, no GL, no socket (`OloEngine/tests/MCP/McpScriptToolsTest.cpp`
  for tiers / quota / live reload / script icons,
  `OloEngine/tests/MCP/McpProtocolIconsTest.cpp` for the icons + `listChanged` +
  snapshot-swap protocol surface).

### 6. Tool icons (SEP-973)

`ToolDef::Icons` (and `serverInfo.icons` via `McpServer::SetServerIcons`) carry
the optional MCP 2025-11-25 `icons` array: objects with a required string `src`
plus optional `mimeType` and a `sizes` array. `McpServer::IsValidIcons` is the
single shape check — a loud `OLO_CORE_VERIFY` for native registrations
(programmer error) and a logged rejection for `RegisterMcpTool{ icons = ... }`
(scripts are runtime data). The key is emitted **only when non-empty**: an empty
`icons` array would advertise icons a client then cannot render.
