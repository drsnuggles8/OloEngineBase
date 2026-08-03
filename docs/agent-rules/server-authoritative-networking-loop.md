# Assembling a server-authoritative multiplayer loop

Written while doing issue #636, which found ~20 well-tested replication / prediction
/ interest / lag-compensation classes and a working GameNetworkingSockets transport
that had **never been assembled into a running loop**. Not one of them was broken.
The subsystem simply did nothing.

Read this before touching `OloEngine/src/OloEngine/Networking/`, and especially
before adding "one more networking class".

---

## 1. How a fully-tested subsystem produces nothing

`NetworkManager::TickSnapshots()` had **zero call sites**. `SetActiveScene()` was
never called in production, so the tick would have early-returned anyway.
`NetworkServer::PollMessages()` / `NetworkClient::PollMessages()` had no production
call site either, so *no message was ever received*. And the header carried this:

```cpp
//   - The network thread (NetworkThread) calls SteamNetworkingSockets::RunCallbacks(),
//     dispatches queued tasks, and invokes TickSnapshots().
```

`NetworkThread::ThreadFunc` did the first two and had never done the third. That
comment is most of the answer to "how did this survive so long": every reader who
went looking for the driver found a sentence saying it existed.

**The rule.** A subsystem's health is not the number of green tests over its parts.
It is whether its *entry point* has a call site. When you inherit or audit a
subsystem, grep for callers of the drive function — `Tick`, `Update`, `Poll`,
`Flush` — before you read anything else. Zero callers means the subsystem is
decorative no matter how good the tests are.

Corollary: unit tests over primitives cannot detect this, by construction. They call
the primitive directly, which is exactly the call site production is missing. The
test that would have caught it is the one that drives the *assembled* loop — which
is why the Functional-axis test here stands up a real server and **two** real
clients rather than asserting on one class.

And: **fix the lying comment in the same change.** A stale "X drives Y" comment is
worse than no comment, because it terminates the next person's search.

---

## 2. Replication runs on the game thread. All of it.

The tempting design is "networking is I/O, so put it on the network thread". It is
wrong here, and the reason is not performance:

* capturing a snapshot reads every replicated component,
* applying one writes them,
* spawn/despawn create and destroy entities (a *structural* registry mutation),
* RPC handlers run arbitrary gameplay code.

All four touch the EnTT registry that the game thread is already iterating. Running
them on the network thread is a data race, not a latency win. (See also
`script-structural-command-safe-point.md` — the same hazard, reached from scripting.)

So the split is by **data**, not by subject matter:

| Thread | Owns |
| --- | --- |
| Network thread (`NetworkThread`) | `RunCallbacks()`, named-thread tasks. Transport state only. |
| Game thread | `NetworkManager::Tick(dt)`: message polling, the replication tick, entity lifecycle, RPC handlers, interpolation. |

`s_Mutex` guards only what genuinely crosses that line (`s_Server`, `s_Client`,
`s_ActiveScene`, `s_Initialized`), and `Tick()` copies those pointers out and
**releases the lock before any scene work** — otherwise a long capture blocks a GNS
callback for its whole duration.

### The consequence people miss: connection events

GNS delivers connection-status changes on the network thread. "A client connected"
usually means "spawn its pawn", which is ECS work. So the transport must not invoke
a spawn callback directly — it only **records** the transition
(`NetworkServer::DrainClientEvents`), and the replication tick drains it on the game
thread. Any future "on connect, do X to the world" feature goes through that queue.

Related detail: GNS can re-deliver the `Connected` state. Record the transition only
on the *first* one, or you spawn a second pawn for the same client.

---

## 3. Five bugs that all look like "multiplayer feels wrong"

Each of these was live in the stack, each is invisible to the primitives' own tests,
and each fails *plausibly* rather than loudly.

### 3.1 The client's input tick must be the client's own counter

`SendInput` stamped commands with `s_TickCounter` — the **server's** replication
tick. On a client that counter never advances, so every input carried tick 0.
`ServerInputHandler` rejects `tick <= lastProcessed`, so the first input was applied
and every subsequent one was dropped.

That is the nastiest possible failure shape: it works once. Give the client its own
monotonically increasing input counter (`ClientReplicationDriver::m_InputTick`),
starting at 1.

### 3.2 Recording an input is not predicting it

`SendInput` called `ClientPrediction::RecordInput` (which only buffers) and sent the
command. Nothing applied it locally. The doc comment said "apply locally
(prediction)". Without the local apply you have a replay buffer for a correction
that never had anything to correct — the pawn simply waits a round trip before
moving.

### 3.3 Reconciliation must rewind before it replays

`ClientPrediction::Reconcile` replays unacknowledged inputs and its comment says
"the server snapshot has already been applied to the scene". It had not been:
`SnapshotInterpolator` deliberately skips the entities the client predicts, so the
predicted pawn never received authoritative state at all. Replaying on top of the
client's own drifting prediction is not reconciliation; the error compounds forever
and every symptom looks like "prediction is a bit off".

The order has to be: **snap the owned entities to the newest authoritative state,
then replay the unacknowledged inputs on top.**

### 3.4 Interpolate by ownership, not by authority

The interpolator skipped every entity whose authority was not `Server`. But *every
player pawn* is client-authoritative — so it skipped all of them, including the
other players'. Result: remote players frozen in place while the world moves.

The correct predicate is "does **this** client predict it": non-server authority
**and** `OwnerClientID == myLocalClientID`. Which means the client has to know its
own id — the server assigns it in a `Connect` message, and that assignment is part
of the identity contract, not an afterthought.

### 3.5 A delta baseline must be an *acknowledged* baseline

Snapshots go out unreliable. If the server deltas against "the last thing I sent",
one dropped packet makes every subsequent delta describe changes relative to a state
the client is not in — permanently, silently, and only off localhost.

The fix is small: the client acks the newest tick it applied (`SnapshotAck`), the
server keeps the un-acked snapshots per connection and deltas against the last
**acked** one. Bounded (32 entries here); past that the client just gets a full
resend, which is the right degradation.

Its mirror on the client: **reassemble deltas into a full state before handing them
to the interpolator.** Pushing raw deltas gives the interpolator two brackets with
different entity sets, so an entity missing from the newer one either holds a stale
value or snaps, depending on which side of the bracket it fell — motion that reads
exactly like packet loss but is a client-side reassembly bug.

---

## 3.6 A transport that dispatches inside its own lock deadlocks every handler that replies

`NetworkServer::PollMessages` collects messages under `m_Mutex` and dispatches
**outside** it, with a comment explaining that a handler may re-enter the server to
send. `NetworkClient::PollMessages` did the opposite — it held `m_Mutex` across the
whole dispatch loop.

Nothing noticed, because until this change **no client handler ever replied**. The
moment one did (the snapshot acknowledgement), the client self-deadlocked on the
first snapshot it received: `Send()` takes the same non-recursive `m_Mutex` that
`PollMessages` was still holding.

The symptom is worth recognising, because it does not look like a deadlock at
first: the process sits at near-zero CPU, the log stops at a plausible-looking
place ("Assigned client id 1"), and a test that should fail an assertion in one
second instead runs forever. **Near-zero CPU over a long wall-clock is a blocked
process, not a slow one** — the same diagnostic that identifies a wedged
`mspdbsrv` (see `build-trees-and-windows-asan.md` §1a).

Rule: **dispatch outside the lock, always.** A message handler is arbitrary
user code; assuming it will not call back into the object that invoked it is
assuming something you do not control. If two sibling classes disagree about
this, the one that dispatches inside the lock is the bug — even if it has
"always worked", which only means nobody has replied from a handler yet.

### The same shape again: an iterator that holds the lock

`NetworkServer::ForEachConnection` runs the caller's lambda while holding
`m_Mutex`. `NetworkServer::GetClientPingMs` takes `m_Mutex`. So *every* call of
the form

```cpp
server->ForEachConnection([&](auto handle, const auto& conn) {
    ping = server->GetClientPingMs(handle);   // deadlock
});
```

hangs on the first connection. That pattern was already in three places before
this change — `NetworkManager::GetClientPingMs`, `OloServerApp`'s `players`
console command, and the editor's **Network Debug panel**, where it meant
opening the panel against a live server froze the editor. None of them had a
caller, so none of them had ever run.

Two lessons, one general and one specific:

* **A callback-taking iterator that holds a lock is a trap in its API shape**,
  not in its uses. Prefer returning a snapshot (`GetConnectionSnapshot()`) so
  the caller's code runs outside the lock by construction; keep the
  callback form only for genuinely trivial, non-re-entrant work, and say so at
  the declaration.
* When you add the first real caller to a dormant API, **assume its other call
  sites have never executed either.** Grep them and read them — they are
  unexercised code that has been accumulating exactly this kind of defect.

## 4. Input commands must be displacements, not direction + speed

Reconciliation replays buffered commands in a loop. That loop has **no timeline** —
it is not a re-simulation at the original frame times. A command meaning "move at
speed s" replays with whatever `dt` the replay assumes and lands somewhere the
server never went, so prediction can never converge.

Bake the step on the client: one command = one exact, order-independent, replayable
displacement (`NetworkMovementInput`). The server then *validates* it — clamping the
step length is what makes the input a request rather than an instruction. An
authoritative server that faithfully applies whatever delta a client sends is
authoritative in structure only.

---

## 5. Make the loop objects, not more statics on the facade

`NetworkManager` is a static singleton: one server, one client. That makes a
two-client test **impossible to write in one process** — and "no test could observe
two clients" is a large part of how the stack stayed dead.

`ServerReplicationDriver` and `ClientReplicationDriver` therefore take their `Scene`
and transport as parameters. The facade holds one of each; a test stands up one
server driver and two client drivers side by side and asserts that client B sees
client A move. Testability was the design constraint that produced the shape, not a
happy accident.

If you add a networking subsystem, ask first: *what would a two-client test of this
look like?* If the answer is "impossible, it's a singleton", change the shape now.

---

## 6. Scoping, identity and lifecycle details worth writing down

* **The interest manager's candidate set is not the replicated set.**
  `NetworkInterestManager::GetRelevantEntities` answers "may this client see it" over
  every entity with an ID and a transform — networked or not. Filter to replicated
  entities before using the result, or the driver will happily "spawn" a purely local
  prop on every client.

* **Sort the relevant set.** The per-client baseline is compared as bytes; an
  unstable entity order makes every tick look changed and turns every delta into a
  full snapshot.

* **A despawn may destroy only what the client's own spawn path created.** Track it
  (`m_LocallySpawned`). Otherwise an entity that came from the client's *scene file*
  gets deleted when it leaves relevance, and walking around the level dismantles it
  one prop at a time.

* **Spawn must carry the entity's initial state**, not just its identity — otherwise
  the client renders at least one frame of default-constructed placeholder. Reusing
  the snapshot record as the spawn payload's tail means one hardened parser for the
  attacker-controlled part, and no chance of the two formats drifting.

* **Snapshot apply must not materialise missing components; spawn must.** That
  asymmetry is deliberate: it is how a client keeps its local-only components out of
  the replicated set while a freshly created entity still gets everything. Hence
  `InterpolationEntry::Ensure` and the `ensureComponents` flag on
  `EntitySnapshot::ApplyEntityRecord`.

* **Put the server tick in the message frame, not the snapshot buffer.** Framing
  snapshot messages as `[serverTick: u32][snapshot bytes]` lets the client time-base
  interpolation on the server's clock and reject out-of-order arrivals, without
  touching the snapshot format `EntitySnapshot::Parse` already reads.

* **Lag compensation rewinds by half-RTT *plus the client's render delay*.** The
  client renders ~100 ms behind the newest snapshot it holds; rewinding by RTT alone
  overshoots by exactly that and under-registers hits on moving targets. The server's
  assumed render delay must match `SnapshotInterpolator::SetRenderDelay` on the
  client — they are one contract in two places.

* **Keep the lag-comp history unscoped.** A rewind has to restore entities that no
  single client currently sees.

---

## 7. RPC: the registry is the authority boundary

The wire carries a name **hash**, never the name, and never the target. Both ends
resolve the id in their own `RpcRegistry`, which is what makes the descriptor
trustworthy: a client can forge any payload it likes, but it cannot change the
server's copy of the rule that says "this RPC is multicast, so a client may not
originate it".

Check authority at **both** ends — the sender refuses to put an illegal call on the
wire, and the receiver re-checks the descriptor against the direction the call
arrived from. The second check is the one that matters; the first is only politeness.

### "Received on the server" is not the same as "sent by a client"

Both authority checks — the direction check and the entity-ownership check —
originally keyed off `receivedOnServer` alone. That is wrong in the same way
twice, because the server also executes RPCs *it* originated:

* the ownership rule validates a **client** sender, so applying it to a
  server-originated call made the server unable to act on any client-owned pawn:
  its own check refused it;
* the direction rule refuses a client pushing a Client/Multicast RPC, so applying
  it to the server's own local execution of a Multicast meant the broadcast
  reached every client **except the host**.

The discriminator is `senderClientID == 0`, meaning "the server itself
originated this". Keying both checks off it keeps the security property intact —
a client that forges a Multicast payload still arrives with its own non-zero id
and is still refused — while letting the server do the things it is by
definition allowed to do.

The second half of this only surfaced when the server-local Multicast path was
routed through the shared `ExecuteLocally` instead of calling the handler
directly. That consolidation was right (one place decides whether a call may
run), but it moved a code path under a guard that had never seen it. **When you
funnel a previously-special-cased path into a shared checkpoint, re-read the
checkpoint's preconditions against the newcomer** — the test that caught this
asserted a count of recipients (3: two clients + the server), which a "did the
multicast arrive" check would have passed with 2.

Two more that bite:

* **Entity-bound server RPCs need the same ownership rule as input commands**
  (`RequiresOwnership`), with an opt-out for calls a non-owner is legitimately
  allowed to make ("open that door").

* **A script-registered handler must not outlive its VM.** A `std::function`
  capturing a `sol::protected_function` survives `delete lua_state` quite happily and
  then crashes on the next incoming call. Mark such descriptors `ScriptOwned` and
  drop them from `LuaScriptEngine::Shutdown` and `ScriptEngine::ShutdownMono` /
  `ReloadAssembly`. The same applies to any registry that can hold a script closure.

Registry lookups return a **copy**, not a pointer: scripts register at runtime, so a
borrowed pointer can dangle the moment another registration reallocates the backing
vector — and the dispatcher holds the descriptor across a re-entrant handler call.

---

## 8. The host contract

Three hosts drive the loop and each must do the same two things:

1. `NetworkManager::SetActiveScene(scene)` when its runtime scene starts, and
   `SetActiveScene(nullptr)` **before** that scene is released. The drivers hold a raw
   `Scene*`; a tick between the swap and the re-registration dereferences a dead scene.
2. `NetworkManager::Tick(ts)` once per frame, **after** the simulation step — so the
   snapshot describes the state the step produced, and so inputs that arrived this
   frame are applied before it.

Call sites: `OloServerApp` (dedicated server), `OloRuntimeApp` (shipped game),
`EditorLayer::StartActiveRuntimeScene` / `OnSceneStop` (editor Play).

Note the editor asymmetry: **only the Play scene is ever registered, never
`m_EditorScene`.** Replicating the authored scene would let a live connection write
into the document the user is editing.
