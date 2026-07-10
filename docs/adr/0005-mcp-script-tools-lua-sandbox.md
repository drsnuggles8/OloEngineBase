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
tool and get it loaded at the next server start — but such an agent can edit
the game's own scripts/code anyway; MCP adds no new authority. Memory-quota
enforcement via a custom `lua_Alloc` is a noted follow-up, not in v1 (the
instruction-count watchdog bounds runaway allocation loops in practice).

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

### 2. Capability = pure compute + a read-only tool-composition bridge

A script tool's handler receives its (schema-validated) arguments as a Lua
table and returns a table/string/number that is converted back to JSON. Its
only window into the engine is:

- `olo.call_tool(name, args)` — invoke another **registered, non-ProjectWrite**
  tool through the same dispatch path a network caller uses (schema
  enforcement included), returning the tool's structured result (or
  `nil, errorMessage`). `ToolDef::ProjectWrite` tools are rejected at the
  bridge, so a script tool is **read-only by construction** — the
  `readOnlyHint:true` annotation v1 stamps on every script tool is a
  guarantee, not a claim.
- `olo.log(message)` — write to the engine log.

No engine usertypes (`Entity`, `TransformComponent`, …) are bound into the MCP
state in v1. Everything a script tool can observe, it observes through tools
that already exist and already honor the read-only/consent rules — the sandbox
adds **zero new engine attack surface**, and the interesting authoring niche
("aggregate three diagnostics into one project-specific digest") is exactly
composition.

### 3. Registration: load at server start, under a reserved namespace

- Scripts are loaded from `<project assets>/McpTools/*.lua` when the MCP
  server **starts** (before `Start()` binds the socket), via
  `MCP::LoadScriptTools(server, dir)` — the same "register before serving"
  contract the native tools follow, so the tool vector is never mutated while
  dispatch threads read it. Reloading edited scripts = restart the server from
  the panel (a one-click stop/start; the token rotates, which is already the
  panel's restart semantic): each load has **replace semantics** — script-owned
  tools (`ToolDef::ScriptOwned`) are unregistered first
  (`McpServer::UnregisterScriptTools`, legal only while stopped and enforced
  loudly), then the directory is rescanned — so edits AND deletions take
  effect. **Live re-registration is deferred**: it would require locking
  `m_Tools` against concurrent dispatch and advertising `listChanged:true` +
  emitting `notifications/tools/list_changed`; v1 keeps `listChanged:false`
  honest by construction.
- Each script calls `RegisterMcpTool{ name=, title=, description=, schema=,
  handler=, toolset? }` (a global the loader injects). Names must match
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

### 5. Consent & writes

v1 script tools are strictly read-only tier: `ProjectWrite = false`,
`readOnlyHint:true`, and the bridge refuses ProjectWrite tools, so the
session's write-consent mode (Disabled / Prompt / Allow-all) never applies to
them — there is nothing they can write. **Write-shaped script tools are an
explicit follow-up**: they would declare `writes = true` at registration,
carry `ProjectWrite = true` (inheriting the #512 per-action consent dialog on
the *script tool itself*), and only then would the bridge allow consented
writes through. Not designing that fully here keeps v1's security argument
one sentence long: *script tools cannot mutate anything*.

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
- **Load scripts live with `listChanged` notifications.** Deferred (see
  Decision 3): requires making the tool registry concurrent and is pure
  ergonomics — the restart loop is one click and rotates a token the panel
  already re-displays.

## Consequences

- The read-only-by-default posture survives verbatim: every v1 script tool is
  provably unable to mutate the project, and the consent machinery is
  untouched.
- `tools/list` stays truthful (`listChanged:false`) because the tool set is
  immutable per server run.
- A buggy or malicious *call* can waste at most one watchdog budget on one
  worker thread; the editor, the other workers, and the gameplay Lua state are
  unaffected.
- The bridge means script tools inherit every future native diagnostic tool
  for free, but also means they can do nothing a native read-only tool cannot
  — by design. Demand for more (engine read-bindings, write-tier script tools
  with consent, C# tools, live reload) is recorded as follow-ups on the
  tracker rather than smuggled into v1.
- Tests can exercise the whole path headlessly: `MCP::LoadScriptTools`
  pointed at a temp dir of test scripts, then JSON-RPC dispatch through the
  in-process server — no editor, no GL
  (see `OloEngine/tests/MCP/McpScriptToolsTest.cpp`).
