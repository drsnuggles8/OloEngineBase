# MCP protocol eras — why the stateless core is deferred, and what shipped instead

Spec `2026-07-28` splits MCP into two **eras**, and the split lands on the transport, not on a
version string:

- **Legacy** (`2025-11-25` and earlier) — a session established by an `initialize` handshake.
- **Modern** (`2026-07-28`+) — a *stateless core*: no handshake, every request self-describing.

This document is the evidence behind issue #777's decision to **not** advertise `2026-07-28` yet,
the two pieces that did ship, and the traps waiting for whoever does the migration. Read it before
touching `OloEditor/src/MCP/McpServer.cpp`'s version list, `HandleInitialize`, `HandleGetStream`, or
`McpClient.cpp`'s connect sequence.

Sources are normative: the spec's [`schema.ts`](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.ts),
[Versioning and Compatibility](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning),
and [Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http).
The blog announcement is a summary and materially under-describes the work — do not scope from it.

---

## 1. What advertising `2026-07-28` actually costs

The issue listed four requirements (`server/discover`, `_meta` identity, routing headers,
`resultType`). The normative spec requires all of those **and** the following, every one of which
touches the transport this server's clients already depend on:

| Requirement | Where it lands here |
|---|---|
| `server/discover` — servers **MUST** implement | new RPC in `DispatchRpc` |
| `_meta` per request: `io.modelcontextprotocol/protocolVersion` **and** `/clientCapabilities` **required**, `/clientInfo` SHOULD | every handler's params |
| `Mcp-Method` + `Mcp-Name` headers **required**, and the server **MUST** validate header↔body agreement | `HandlePost` |
| `-32020 HeaderMismatch`, `-32021 MissingRequiredClientCapability`, `-32022 UnsupportedProtocolVersion` — each with a mandated `data` shape and a mandated HTTP status | error construction |
| Method-not-found becomes **HTTP 404** (was 200 + JSON-RPC error) | `HandlePost` status selection |
| **The GET SSE stream is removed** — `GET`/`DELETE` must answer `405` | `HandleGetStream` |
| `subscriptions/listen` replaces it, and replaces `resources/subscribe` | new long-lived RPC |
| Cancellation over HTTP becomes *closing the response stream*; `notifications/cancelled` is stdio-only | the whole `m_InFlightCalls` path |
| `Mcp-Session-Id` ignored, never minted or echoed; `Last-Event-ID` ignored, streams not resumable | `ProcessRequestBody`, `HandleGetStream` |
| `logging/setLevel` folded into `_meta`'s `io.modelcontextprotocol/logLevel` | logging path |
| MRTR: `resultType: "input_required"`, `inputRequests`, `inputResponses`, `requestState` | results |
| `resultType` on **every** result | `MakeResult` |

`resources/read`'s not-found code also moves (`-32002` → `-32602`), and clients must support
`x-mcp-header` parameter mirroring.

**This is a second transport shape living beside the first**, not a field-addition. Both shapes must
be served from one httplib endpoint for the whole ≥12-month offramp, because our actual clients all
speak `2025-*` today.

## 2. Why it is a no-go *now*

1. **Zero benefit.** The stateless core exists so any request can land on any instance behind a
   round-robin load balancer. This is a single **localhost** diagnostics server with one client, no
   gateway, and no metering. Every listed benefit is structurally inapplicable.
2. **The blast radius is the instrument itself.** This server is how other engine work gets verified
   live. The failure mode of a botched dual-era transport is a server that passes a happy-path smoke
   test and misbehaves on the paths that matter — exactly the "green but wrong" archetype.
3. **Nothing is breaking.** Every removal has a ≥12-month offramp from 2026-07-28, and our version
   negotiation already degrades gracefully (an unknown `protocolVersion` falls back to our latest).
4. **No client asks for it.** A modern client talking to us gets a clean legacy handshake; the spec's
   own compatibility matrix rates *dual-era client × legacy server* as **Works**.

The spec explicitly blesses staying legacy: a dual-era **client** falls back after inspecting a
`400`, and only a *legacy-only client × modern-only server* pairing fails outright — the direction we
are not in.

## 3. The trap: `server/discover` cannot be added on its own

The obvious "cheap first slice" is to implement `server/discover` — it is additive, it is a new
method, and it breaks no existing client.

**It is actively harmful without the rest.** A modern client uses a successful `DiscoverResult` as
proof the peer is a modern server, and then sends modern requests. A `server/discover` answered by a
server that still requires `initialize`, has no `_meta` handling, mints session ids and serves a GET
stream converts a *working* legacy fallback into a *broken* modern conversation. On stdio the probe
**is** the era detector; on HTTP the era detector is a modern request's `400` body — and answering
discover while rejecting modern requests puts a client between the two.

This is why the issue's "do it as one coherent change, not piecemeal" is a correctness constraint,
not a style preference. `server/discover` ships when `_meta`, `resultType`, the routing headers, the
three error codes and the GET-stream removal ship — or it does not ship.

## 4. What did ship (PR for #777)

### 4a. The outbound client is dual-era

`McpClient.cpp` used to hardcode `initialize` with `protocolVersion: "2025-06-18"`. It now runs the
spec's stdio backward-compatibility rule (`McpClientConnection::NegotiateEra`):

1. Probe `server/discover`, carrying a modern `_meta`.
2. A `DiscoverResult`, **or** a `-32022 UnsupportedProtocolVersionError`, identifies a **modern**
   server — a recognized modern error is a modern marker, so it triggers a version retry, never a
   fallback.
3. **Anything else — including no reply at all — is legacy**, and the `initialize` /
   `notifications/initialized` handshake runs unchanged.
4. In the modern era every request carries `_meta` (version + `clientInfo` + an empty
   `clientCapabilities`, which is *required* and per-request: a server must not infer it from an
   earlier call). Stamped in `SendRequest`, the single funnel, so a future request cannot ship
   without it.

Three details worth keeping:

- **The probe timeout is separate and short** (`McpClientConfig::DiscoverProbeTimeout`, 3 s, clamped
  to `HandshakeTimeout`). A well-behaved legacy child answers `-32601` instantly; this budget is only
  spent on one that swallows unknown methods, and without it *every* legacy connect would pay the
  full handshake timeout before falling back.
- **A late probe response is harmless.** After the timeout the pending entry is erased, so `OnLine`
  finds no waiter and drops it — it cannot be mistaken for a later request's answer.
- **`resultType != "complete"` is now refused, loudly.** A modern server may answer a bridged
  `tools/call` with `input_required` (MRTR) expecting elicitation/sampling and a retry. We implement
  none of that, and an *absent* `resultType` means `"complete"` for backward compatibility — so the
  only unacceptable outcome is forwarding an interim result to the agent as if it were the answer.
  It returns a tool error instead.

### 4b. The `logging` offramp has a carrier, and both run side by side

`logging` is deprecated (SEP-2577). We push the entire diagnostics event stream as
`notifications/message` over the GET SSE stream, which made this the one place OloEngine was
structurally tied to a removed feature.

**The replacement is a subscribable resource, and the choice is forced.** Under `2026-07-28`:

- the GET stream is gone;
- `notifications/message` survives but only on the response stream of the request it relates to;
- `SubscriptionFilter` — everything `subscriptions/listen` can deliver — is a **closed set** of four
  types (`toolsListChanged`, `promptsListChanged`, `resourcesListChanged`, `resourceSubscriptions`),
  and the server **MUST NOT** send a type the client did not request.

So a custom notification has nowhere to live in the modern era, and the extensions framework is for
negotiated capability bundles, not a one-off push. `notifications/resources/updated` on a subscribed
URI is the only carrier on that list that fits — and it also exists *today*. That is the property
that made it the answer: **the same notification, resource and payload work in both eras; only the
subscription plumbing moves** (`resources/subscribe` → `subscriptions/listen`).

Shipped: `olo://events/recent` (200 newest events + `lastId`, entries byte-identical to
`olo_events_tail`), `resources/subscribe` / `resources/unsubscribe`, and
`capabilities.resources.subscribe: true`. `logging` and the `notifications/message` push are
**unchanged** — dropping them early would break every client that speaks a `2025-*` revision, which
today is all of them.

Two design points to preserve:

- **Only a resource with a `ResourceDef::ChangeToken` is subscribable.** Without a cheap monotonic
  change token there is no honest way to know the content moved, so a subscription would promise
  updates that never arrive. `resources/subscribe` rejects those with `-32602` *naming the URIs that
  do work*. This is the same honesty rule as `olo_render_why_not_visible` reporting HZB occlusion as
  not-observable instead of guessing.
- **The first poll cycle after a subscribe seeds the token instead of firing.** Otherwise a client
  gets a spurious "updated" for state it has never seen change. Unsubscribing drops the entry, so a
  resubscribe re-seeds rather than replaying.

The known simplification: the subscription set is **server-global**, because the GET stream carries
no session identity. With two streams open, both see updates once either subscribes. It errs toward
over-delivery (a non-subscriber ignores the notification), and it dissolves in the modern transport
where `subscriptions/listen` makes the stream *itself* the subscription — so building a bespoke
per-session registry now would be work thrown away by the migration.

## 5. When to revisit

Not on a date — on a trigger. Any one of these makes the migration real work rather than speculative:

- A client we actually use (Claude Code, an SDK we bridge to) starts sending `2026-07-28` requests —
  visible as `_meta`-carrying POSTs, or as a `400`/`405` we return to a modern probe.
- A bridged external server goes modern-only. The client half already handles this; the *server* half
  is what would need the rework.
- The MCP SDKs' Tier-1 servers drop the legacy handshake, which starts the clock on the offramp
  actually ending.

When it happens, build it as one additive route: `2026-07-28` added to
`kSupportedProtocolVersions`, `server/discover` + `_meta` + routing headers + `resultType` +
`subscriptions/listen` served **alongside** the existing handshake, legacy path untouched and still
tested, and the new version advertised only once the whole set works. The load-bearing test is not
the new path — it is the legacy one still passing beside it.
