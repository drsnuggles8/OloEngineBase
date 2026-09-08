# Progressive structural collapse (issue #786)

A set of `StructuralNodeComponent` pieces hold each other up; when one leaves, whatever loses its
path to an anchor comes down after it. Read this before touching `StructuralGraph`, adding a piece
to a structure, or wondering why a wall did or did not fall. The design decision (derived adjacency,
not authored links) is [ADR 0021](../adr/0021-structural-connectivity-is-derived-from-collider-adjacency.md);
the debris this feeds into is [destructible-debris.md](destructible-debris.md).

---

## Anchors are explicit. A structure with none stands until it is touched, then vanishes

A piece is stable iff a piece with `m_Anchor = true` reaches it along support edges. Nothing is
anchored by being low, by being static, or by resting on the ground plane — the floor is not a graph
node, and guessing would be a silent wrong answer.

The consequence is the trap: **an unanchored structure is not detected until something breaks.** The
solver only runs when a piece leaves, so an unanchored wall stands normally through the whole scene
until the first hit, and then the entire wall comes down at once. That is not a bug in the solver;
it is what "no path to an anchor" means. `StructuralGraph::SolveUnsupported` logs
`"a structure of N pieces (lowest id X) has no StructuralNodeComponent::m_Anchor left"` once per
structure when it happens. If a wall vanishes wholesale on the first hit, read the log before
reading the solver.

## Same-course neighbours support each other, and that is deliberate

Edge direction comes from height: the lower piece supports the higher one. Pieces whose centres are
within `kLevelToleranceFraction` (0.25) of their combined vertical half-extents are on the **same
course** and support each other *both ways*.

Without that mutual edge, a lintel drops the instant nothing is directly beneath its middle, and no
arch, doorway or overhang can stand. With it, load travels sideways along a row to reach an anchor —
which is also why a single surviving leg keeps a whole bonded row up.

## Sideways load is charged for, or a wall is all-or-nothing

The flood is a **0-1 BFS**, not plain reachability: a vertical edge costs 0, a same-course edge costs
1, and a piece is stable only while its cheapest path from an anchor stays inside its own
`m_MaxLateralSpan` (default 3). A piece past its budget is unstable **and** relays nothing onward.

Do not "simplify" this back to reachability. With free lateral transfer, a wall that still has one
base block standing is fully supported through the bonded courses — so it is intact until it is
entirely unsupported and then it vanishes in one go, which is the behaviour #786 exists to replace.
The span budget is what makes a hole widen one course-step at a time.

If a structure refuses to collapse when you expect it to, the usual cause is a same-course path,
within budget, that you did not notice reaching an anchor several pieces away. Read
`m_MaxLateralSpan` before reading the solver.

## Adjacency is a shared FACE, and the query radius has to allow for the margin on the diagonal

Two pieces are connected when their world AABBs come within `m_ContactMargin` (default 5 cm) on all
three axes **and** genuinely overlap on at least two of them. Authored blocks rarely sit exactly
flush and a 1 mm gap would sever the graph, so the margin is not optional — but the two-axis rule is
what stops a corner touch counting as support. Without it, two unit cubes meeting only along an edge
are load-bearing, and a diagonal staircase of corner-touching blocks holds up a wall. The third axis
is the contact normal, which is why it is two and not three.

The margin enters the overlap test **per axis**, so the widest centre distance that can still overlap
is `|he_i + he_j + margin*(1,1,1)|` — the margin contributes `margin * sqrt(3)` on the diagonal. The
spatial-hash query radius must include that factor, or corner-touching pairs are silently dropped and
a diagonally-braced structure quietly loses edges. `StructuralGraph::Rebuild` does; a future rewrite
must too.

## A static piece cannot be made dynamic unless it was built expecting to be

Jolt allocates `MotionProperties` at **body creation time only**. A body created `Static` has none,
and `SetMotionType(Dynamic)` on it asserts. `BodyCreationSettings::mAllowDynamicOrKinematic` is what
asks for the allocation up front, and `JoltBody::CreateBodySettings` sets it for entities that carry
a `StructuralNodeComponent` — scoped to those, so no other static body in the scene pays for it.

That check runs **when the body is built**, which is a different moment from when the components are
authored. Add a `Rigidbody3DComponent` at runtime before the `StructuralNodeComponent` and the body
is built without the flag. Authored scenes are safe (every component exists before `OnRuntimeStart`
creates bodies); a prefab spawn or a test assembling a piece by hand is not. So the release path does
not rely on ordering: it checks `JoltBody::CanBecomeDynamic()` and **rebuilds the body** when the
answer is no. Do not replace that with a documented ordering rule.

## A detached piece goes to MOVING, not DEBRIS

`ReleaseToPhysics` re-derives the object layer with `SetCollisionLayer(GetCollisionLayer())` after
switching the motion type — the call looks like a no-op and is not: `GetObjectLayerForCollider`
consults the body type, which has just changed, so a piece created on `NON_MOVING` moves to `MOVING`.
Skip it and the collapse falls through itself and through the ground, because `NON_MOVING` pairs do
not collide.

It deliberately does **not** call `SetToDebrisLayer()`. Rubble that piles up and can knock the player
over is the point of a collapse; `ObjectLayers::DEBRIS` collides with neither the character nor other
debris. The chunks the piece eventually shatters into are what go to `DEBRIS`, through the #459 path.

## The collapse runs on the existing `Destructible` node, in phases

There is no separate scheduler node. A break, the loss of support it causes, and the debris the
collapse produces are three steps of one tick's story, and the collapse needs the same post-fence pin
the debris path already has (switching a body's motion type must not race the physics step).
`DestructibleSystem::OnUpdate` is:

0. advance the collapse state machine — condemned pieces tick down, detach, fall, and finally set
   `m_PendingBreak`;
1. collect breaks (now including the pieces phase 0 just handed over);
1b. re-solve the islands that lost a piece and put newly unsupported pieces on the clock;
2–4. the #459 debris ageing, destruction and budgeted spawn, unchanged.

Phases 0 and 1b are field writes only. Everything structural still happens in phases 3 and 4.

**A piece is only removed from the graph when it actually detaches, not when it is condemned.** That
is what makes the collapse cascade one ring at a time instead of resolving in a single tick: while a
piece is `Detaching` it is still standing and still holding up whatever rests on it.

## What is island-scoped, and what is not

The **solve** is: a break re-floods only the connected components that lost a piece, in O(island).
`LastSolveVisitedNodes` reports exactly how many nodes it looked at, and
`StructuralCollapseTest.ASolveVisitsOnlyTheIslandThatLostAPiece` pins it against five untouched
columns. Scratch is stamped rather than cleared, and the in-scope set comes from a per-island node
list, so neither scoping nor resetting is O(scene).

The **build** is not, and the header says so: it is a full O(N log N) pass. What triggers it is an
**order-independent signature**, computed each tick, over every structural piece's UUID mixed with
the authored fields the build caches — `m_Anchor`, `m_ContactMargin`, `m_MaxLateralSpan`. Two
consequences worth stating outright:

- **A same-count swap rebuilds.** Destroy one piece and create another in the same tick and a count
  is unchanged while the graph is thoroughly wrong; the signature is not fooled. Pinned by
  `AddingAndRemovingAPieceInOneTickStillInvalidatesTheGraph`.
- **Retuning a piece rebuilds.** Toggling `m_Anchor` from the inspector, or a Lua script setting
  `anchor` / `maxLateralSpan` mid-Play, changes an input the graph has cached. Lua, the editor, MCP
  and C# can all reach those fields and none of them know the graph exists, so the signature has to
  notice — do not replace it with invalidation calls at each mutation site, because the next site
  will forget.
- **Detaching does not rebuild.** `m_State` is deliberately outside the signature; a piece leaving
  the structure is tracked in place by `MarkRemoved`, and folding state in would force a full
  rebuild on every tick of a collapse.

Rebuilding is idempotent because aliveness is derived from each piece's own `m_State`, not from the
previous graph, so a rebuild mid-collapse reconstructs the standing structure rather than
resurrecting what already fell. Do not "optimise" that by carrying the old `Alive` array across a
rebuild.

One input is **not** covered: a standing piece's transform. Nothing moves a standing structural
piece at runtime today, and folding transforms in would rebuild every tick while debris falls.

## Cross-binding: what `StructuralNodeComponent` needed

All-trivial, so scene YAML is fully generated (no `SceneSerializer` block, no `kComponentsCustomSerialize`
entry). Hand-written, because nothing generates them:

- `SaveGameComponentSerializer::Serialize` + `REGISTER_SAVE_COMPONENT` — it persists the collapse
  state, so a half-collapsed structure reloads half-collapsed and a falling piece is not restored as
  a load-bearing member. The state byte is range-checked on load; an out-of-range value restores as
  `Stable` rather than as an undefined fourth enumerator.
- The Lua usertype (`LuaScriptGlue_Core.cpp` + `REGISTER_COMPONENT`), with `state` read-only.
- The editor inspector and Add Component entry (`SceneHierarchyPanel.cpp`). `DestructibleComponent`
  had neither — #459 shipped without them — and both were added at the same time.

## Where the contract is pinned

`OloEngine/tests/Functional/Gameplay/StructuralCollapseTest.cpp`: a column collapsing above a removed
support while a second column does not move; a piece with two supports surviving the loss of one; an
unanchored cycle collapsing entirely; the island-scoping metric; the hop-staggered ripple; a detached
piece falling as a rigidbody before it shatters; a large collapse staying inside the global
live-debris budget; and a plain `DestructibleComponent` being untouched by the solver.

Every test addresses pieces by **UUID**, never by a stored `Entity`. A collapsing piece is destroyed,
and `Entity::operator bool` only checks for a null handle — `HasComponent` on a destroyed entity is
an EnTT assert, not a `false`.
