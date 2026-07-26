# No MCP endpoint in cooked/runtime builds — deferred behind seven preconditions, and if ever built it is a separate default-deny registry, not the editor's surface lifted into the engine

Issue [#673](https://github.com/drsnuggles8/OloEngineBase/issues/673)'s Tier 3 is
**runtime/cooked-build-capable diagnostic tools** — Unreal Engine 6.0's
`AIRuntimeCallable`, tools that keep working in a packaged build so QA can
triage a bug on a test device from their own machine. The issue itself scoped
this as *"worth a design doc before building, not now"* and *"Don't build
opportunistically."* This ADR is that design doc: it records the threat model,
the conditions that would have to hold before the idea is worth revisiting, and
the decision to **not build it**.

Nothing in this ADR is implemented. It exists so the roadmap bullet stops being
an open invitation and becomes a recorded decision with a re-open test.

## What is being proposed

A read-only MCP endpoint hosted by `OloRuntime` (and, in the most ambitious
version, `OloServer`) instead of `OloEditor`, so an agent can call
`olo_perf_snapshot` / `olo_render_why_not_visible` / `olo_log_tail` against a
shipped binary running on a QA device.

## Threat model

The editor's server is safe because of five properties stated at the top of
[`McpServer.h`](../../OloEditor/src/MCP/McpServer.h). Moving the endpoint into a
cooked build breaks or hollows out **four of the five** — and the fifth turns
out to protect the wrong thing.

1. **"Binds 127.0.0.1 only, never a routable interface."** Loopback is exactly
   what makes the feature pointless in the runtime case: the whole premise is
   *remote* triage of a device you are not sitting at. So the feature creates
   direct pressure to bind a routable interface — i.e. to delete the single
   strongest control we have. A design that keeps loopback has to add an
   operator-driven tunnel (`adb reverse`, SSH), at which point the "remote"
   convenience it was built for is mostly gone.

2. **"Off by default; started explicitly from the editor's MCP panel."** A
   cooked build has no MCP panel and, usually, no operator sitting in front of
   it. Whatever replaces the panel — a console command, a launch flag, an
   in-game menu — becomes the thing an attacker targets, and it has to be
   reachable enough for QA to use and unreachable enough that a player can't
   flip it. Those two requirements are in direct tension.

3. **"Every request must carry a bearer token the editor displays for the user
   to paste."** In the editor the token is generated per run and shown to a
   present human. A cooked build has neither. A token baked into the binary is a
   shared secret shipped to every copy — recoverable with `strings` — and a
   token generated per run needs an out-of-band channel to reach the QA
   operator, which is a whole subsystem nobody has costed.

4. **"The Origin header is validated (DNS-rebinding defence)."** This one still
   works, but it only ever defended against a *browser* being used as a
   confused deputy. It does nothing against a direct socket, which is the actual
   threat once the port is reachable.

5. **"Read-only with respect to the user's PROJECT."** This is the property
   people reach for to argue the feature is safe, and in the runtime context it
   is protecting the wrong asset. Read-only bounds what an attacker can *break*.
   It says nothing about what they can *learn* — and in a shipped game the
   diagnostics ARE the payload:

   - `olo_scene_list_entities` / `olo_scene_get_entity` on a live multiplayer
     client is a wallhack oracle: exact positions of everything the client has
     been told about. "Read-only" is a safety property, never a
     competitive-integrity one.
   - `olo_log_tail`, asset paths, and shader sources are an IP and
     reverse-engineering channel; absolute paths are also a PII channel
     (`RedactPaths` is opt-in and off by default).
   - `olo_screenshot` against an unreleased build is a spoiler pipe.

Two risks are entirely new in the cooked case and have no editor analogue:

6. **The binary is distributed, so every copy is an attack sample.** An attacker
   can reverse the handshake, the token derivation, and the dispatch code
   offline, at leisure, before ever touching a live instance. Nothing about the
   editor deployment model has this property.

7. **The request path can stall the game.** `MarshalRead` deliberately parks an
   HTTP worker until the game thread reaches a frame boundary; capture tools
   allocate megabytes and do GL readbacks. In the editor a hitch is an
   annoyance. In a shipped multiplayer client, an unauthenticated pre-token
   parse plus a marshal-heavy tool is a remote hitch/DoS primitive — and the
   pre-auth surface (cpp-httplib framing + `nlohmann::json` over hostile bytes)
   runs before any of our own checks do.

`OloServer` is the sharpest version of all of the above: a diagnostics endpoint
on a dedicated game server is a competitive-integrity hole with a network
attacker permanently in front of it.

## Decision

**Defer. Do not build a runtime MCP endpoint, and do not add partial scaffolding
"just in case".** The editor server stays the only MCP surface.

Concretely, until every precondition below is met:

- No MCP source moves from `OloEditor/src/MCP/` into `OloEngine/src/`.
- No `ToolDef` flag is added to mark a tool runtime-callable.
- `OloRuntime` and `OloServer` link no HTTP server.

## What would have to be true to revisit

All seven, not a subset. Any one of them missing reproduces a hole above.

1. **Compile-time exclusion, not a runtime bool.** Gated behind an opt-in
   definition whose absence removes the whole translation unit from the binary,
   so a shipping build cannot be talked into enabling it at runtime and a
   reverser finds no dormant server to wake.
2. **A separate, default-deny tool registry.** The editor's ~66 tools are
   curated for a *trusted local operator inspecting their own project*. That
   assumption is invisible in each tool and fatal if it travels. A runtime tier
   is a much smaller registry with explicit per-tool opt-in (UE's actual shape),
   built from scratch — never the editor list minus a blocklist, because a
   blocklist fails open for every tool added later.
3. **Per-run token delivered out of band** — written where only device access
   reaches it (platform log, app-private storage), never baked into the binary.
4. **Loopback binding, always**, with exposure requiring a deliberate operator
   tunnel. No configuration path binds a routable interface.
5. **A hard budget on the request path**: no unbounded `MarshalRead`, no capture
   or readback tools, rate limiting — so a hostile caller cannot hitch the
   frame. Sized against a frame budget, not "generous".
6. **A build configuration that is internal-only.** Today's `Distribution` ships
   to end users; an internal-QA configuration distinct from it does not exist
   and would have to be introduced first. Without it, "QA only" is a convention,
   not a guarantee.
7. **Never on a multiplayer client connected to a shared live server, and never
   on `OloServer` in production** — enforced in code, not in documentation.

If a future session can only satisfy some of these, the answer is still no.

## Alternatives that solve most of the actual need without a listening socket

The QA-triage need is real; a listening port is just the worst way to serve it.
Prefer, in order:

1. **Offline artifacts.** Frame captures, save-games, and the diagnostics event
   log already serialize. A "dump diagnostics bundle to disk on request" path
   gets QA the same data with no network surface and no timing, and it replays
   in the editor where the full tool surface is already safe to use.
2. **Log/telemetry upload.** Covers the majority of triage ("what did it say
   before it broke") with an outbound, authenticated, one-way channel.
3. **Dial-out instead of listen.** If live interaction is genuinely required,
   the runtime should *connect out* to a rendezvous the developer controls
   rather than accept connections — no listening port on the shipped binary, no
   pre-auth parse surface exposed to the network, and the operator's machine
   holds the trust anchor. Tier 1 already built the client half of this
   (`McpClient.{h,cpp}`, stdio transport), so the shape is familiar. This is the
   only *interactive* variant worth designing if the preconditions above are
   ever met.

## Consequences

- OloEngine has no remote-triage story for packaged builds. Accepted: the
  alternatives above cover the common cases, and the gap is documented rather
  than silently open.
- Issue #673's Tier 3 bullet closes as **decided**, not as done. Re-opening
  requires meeting the seven preconditions, which is a deliberately high bar.
- The editor MCP server's security posture is unchanged, and this ADR is now
  the reference for why it is scoped to the editor — see also
  [ADR 0005](0005-mcp-script-tools-lua-sandbox.md), which reasons about the
  script-tool trust boundary inside that same posture.
