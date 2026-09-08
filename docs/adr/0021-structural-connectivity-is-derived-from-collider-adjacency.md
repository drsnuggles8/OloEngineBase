# Structural connectivity is derived from collider adjacency, not authored per piece

Issue #786 asks for *"a connectivity graph between adjacent destructible bodies (which pieces
support which)"* and leaves open how that graph comes to exist. The choice is load-bearing in a way
the rest of the feature is not: everything downstream — the solver, the propagation, the collapse
timing — works the same whichever answer you pick, but the answer decides what a level designer
does every time they build a wall, and it cannot be changed later without invalidating every
authored scene.

[ADR 0013](0013-destructible-debris-asset-swap-not-runtime-fracture.md) is the precedent: same
feature family, same shape of decision, recorded before the code was written.

---

## The two designs

**A. Authored connectivity.** `StructuralNodeComponent` carries a `std::vector<UUID> m_SupportedBy`
(or a symmetric neighbour list). The designer states, per piece, which pieces hold it up. The
runtime reads the list and never computes anything.

**B. Derived connectivity.** The component carries only *policy* — is this piece an anchor, how much
slack counts as contact, how far load may travel sideways, how fast does it come down. The graph
itself is computed from the pieces' collider bounds: two pieces that share a contact face are
adjacent, and the one whose centre is lower supports the one above.

## Decision

**Design B.** Adjacency is derived from collider bounds. Nothing about the graph's topology is
authored.

## Why

1. **The graph is already in the scene, implicitly.** A designer builds a wall by placing blocks
   next to each other. That placement *is* the connectivity statement; asking them to restate it as
   a neighbour list is asking them to say the same thing twice, in a form that silently goes wrong
   the moment a block moves. Design A's list has to be maintained by hand on every edit; design B's
   graph cannot drift from the geometry because it is a function of it.

2. **Design A has no tolerable authoring path here.** A 120-block wall needs 120 lists. This editor
   has no graph-authoring widget, and building one is a panel-sized project in its own right (see
   `SceneHierarchyPanel.cpp` — a component's inspector is hand-written, and a list-of-entity-refs
   widget does not exist). Design B needs two checkboxes, three floats and an integer, which is what shipped.

3. **A `std::vector<UUID>` member costs the whole cross-binding chain.** It makes the component
   non-trivial, which means a hand-written `SceneSerializer` block, a hand-written save-game
   serializer that has to validate every id on load, and a copy path that has to remap ids when a
   scene is duplicated or a prefab instantiated — because a UUID list is a set of *references*, and
   references are exactly what breaks under copy. Design B's component is all-trivial: scene YAML
   round-trips are generated, and the only hand-written binding is the save-game overload every
   persistent component needs anyway.

4. **Prefabs and streaming would be actively wrong under A.** Duplicate a prefab wall and every
   copy's neighbour list still names the *original's* pieces. Under B the copy derives its own
   graph from its own blocks, and two identical walls behave identically without anyone thinking
   about it.

## What derivation cannot do, and what we do about it

Derivation is the cheaper thing to author and the harder thing to get *right*. Three places it is
weaker than an authored list, stated plainly rather than discovered later:

- **It cannot express a non-geometric connection.** A steel tie between two pieces that do not
  touch is invisible to an AABB test. Today there is no way to say it. If that turns out to matter,
  the extension is an *additive* authored link list on top of the derived graph — not a replacement
  for it, and not a reason to have started with A.
- **It reads adjacency from axis-aligned bounds.** The bound is the exact AABB of the transformed
  collider box, so a rotated piece gets a correct bound rather than a hand-waved one — but two
  rotated pieces whose AABBs overlap while the boxes themselves do not will be treated as adjacent.
  For structures built out of grid-placed blocks, which is what this feature is for, that case does
  not arise.
- **It needs slack.** Authored blocks rarely sit exactly flush, and a 1 mm gap would sever the
  graph. `m_ContactMargin` (default 5 cm) is that slack, and it is per piece so a loosely built
  structure can be told to be generous without making every structure generous. The slack is only
  allowed to close a gap, never to invent a contact: a pair must genuinely overlap on two of the
  three axes, so a corner touch is not support.

## The support rule, and why it is not just "below"

An edge is directed by height, because that is what "supports" means under gravity. For an adjacent
pair with a vertical centre offset `dy` and a tolerance derived from the pieces' own vertical
extents: the lower piece supports the higher one, and **two pieces on the same course support each
other**.

The mutual same-course edge is the interesting half. Without it, a lintel spanning two legs falls
the instant nothing is directly beneath its middle — which is wrong for anything built out of
bonded courses, and it is the difference between an arch that stands and a pile of blocks. With it,
load travels sideways along a row to reach an anchor, which is what a real bonded wall does.

## Sideways load transfer is bounded, and that is what makes a wall crumble

Plain reachability was tried first and is wrong, in a way that is only visible once you look at a
whole wall rather than a column. If a same-course edge carries load for free, then a wall with one
surviving base block is *fully supported*: every piece has a path down through the bonded courses to
that one block. The wall is intact until it is entirely unsupported and then it vanishes all at
once. "All or nothing" is exactly the behaviour #786 exists to replace.

So the flood is weighted — a **0-1 BFS**, still nothing like an FEM. A vertical edge costs 0; a
same-course edge costs 1; a piece is stable only while its cheapest path from an anchor stays inside
its own `m_MaxLateralSpan` (default 3). A piece past its budget is unstable *and* relays nothing
onward, so a hole punched in a wall widens by one course-step at a time instead of stopping at its
edge. That is the mechanism behind "progressive", and it costs one `u32` per piece and one byte per
edge.

`m_MaxLateralSpan = 0` means no cantilever at all: a piece must have something directly underneath.
That is the right setting for a dry-stacked pile, and the wrong one for anything with a doorway.

A piece is stable iff some **anchor** piece reaches it by following support edges within that
budget. Anchors are explicit (`m_Anchor`), not inferred from height or from being static: a structure floating in the
air with a piece that happens to sit low is not grounded, and guessing would be a silent wrong
answer. A structure with no anchor left collapses entirely, and the solver logs a warning naming
the structure — an unanchored structure is an authoring mistake, and it says so.

## Consequences

- `StructuralNodeComponent` is all-trivial. Scene YAML is generated; the save-game serializer is
  hand-written (it persists the collapse state, so a half-collapsed structure reloads
  half-collapsed) and the Lua usertype and editor inspector are hand-written as this repo requires.
- The graph is invalidated by an **order-independent signature over the pieces' identities and
  their authored solver inputs** — each piece's UUID mixed with its `m_Anchor`, `m_ContactMargin`
  and `m_MaxLateralSpan` — not by a piece count. A count cannot see a create and a destroy in the
  same tick cancelling out, which would leave a destroyed piece still holding up its neighbours;
  and it cannot see a piece being *retuned*, which every one of Lua, the editor inspector, MCP and
  C# can do at runtime without knowing the graph exists. So: **replacing a piece with a different
  one rebuilds even though the count is unchanged, and leaving the identities and their inputs
  alone does not rebuild.** A piece merely *detaching* is deliberately outside the signature —
  `m_State` is tracked in place, or a collapse would rebuild every tick.
- Re-solving is scoped separately and more tightly: only the connected components that lost a piece
  are re-flooded. The solve is the part that has to stay cheap as scenes grow, and it is O(island)
  rather than O(scene); `StructuralGraph::LastSolveVisitedNodes` reports it and
  `StructuralCollapseTest` pins it against untouched structures.
- A destructible with no `StructuralNodeComponent` keeps the #459 behaviour untouched: it shatters
  in isolation. Collapse composes with the existing break trigger, the `DEBRIS` layer and the global
  live-debris budget rather than replacing any of them.
- A structural piece authored `Static` is created with Jolt's `mAllowDynamicOrKinematic` set, so it
  can be switched to `Dynamic` when it is condemned. That costs a `MotionProperties` allocation per
  structural piece and is scoped to exactly those entities — see `JoltBody::CreateBodySettings`.
- If authored links are ever wanted, they enter as an **additional** edge source feeding the same
  graph. This ADR should be revisited (not reversed) at that point, exactly as ADR 0013 says of
  runtime fracture.

## Status

Accepted. Implemented by the `StructuralNodeComponent` / `StructuralGraph` + `DestructibleSystem`
collapse slice for #786.
