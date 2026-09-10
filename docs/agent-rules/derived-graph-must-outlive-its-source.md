# Before trusting an existing graph, check who writes its edges — and which way

**The rule.** When you need to answer "what points at X", do not assume the in-memory graph that
looks like it holds the answer actually does. Enumerate its writers, check the direction of every
edge they register, and check what fraction of the real relationships they cover. If the graph is
built by a handful of call sites, it covers only what those call sites happen to run for — and a
graph that answers *most* of the question is worse than none, because it answers confidently.

**The second rule.** A direction bug in a graph can be invisible because a matching direction bug
in its reader cancels it. Fixing either half alone breaks working behaviour. Change both together,
and pin each half with its own test.

## The case (issue #1128)

`olo_asset_delete` has to refuse when anything references the asset, and `olo_asset_move` has to
rewrite every reference. `EditorAssetManager` already keeps a dependency graph —
`m_AssetDependencies` / `m_AssetDependents`, exposed as `GetDependencies` / `GetAllDependencies`.
It looked like the answer. It was not, for four independent reasons:

1. **`SceneSerializer` registers nothing.** The only writers were six `AssetSerializer` call sites
   (material→texture, mesh→meshsource, staticmesh→{meshsource, material}, meshcollider→collidermesh,
   animation→{source, mesh}). Scenes are the majority of referrers in this project, and every one of
   them was absent.
2. **Load-gated.** An edge appears only when an asset is *deserialized*. An asset nobody opened
   this session contributes nothing, so the referrer set is short by an amount that varies with what
   the user happened to click on.
3. **Every edge was backwards.** The contract is `RegisterDependency(handle, dependency)` —
   "`handle` depends on `dependency`", spelled out in `AssetManager.h` as "a material (handle)
   depends on a texture (dependency)". All six sites passed the pair the other way round. Each one
   sat directly beneath a `OLO_CORE_TRACE` line stating the correct relation, so the code and its
   own log disagreed and nobody noticed.
4. **A handle-set graph cannot produce an edit.** Rewriting a reference on a move needs the file,
   the line and the exact text of the mention. Even a complete and correctly-directed graph of
   handles could not have driven the rewrite.

### Why nobody noticed the direction

`ReloadData` called `UpdateDependencies(handle)`, which walked the **forward** map — what `handle`
depends on — and called `OnDependencyUpdated` on each. That is also backwards. Composed with
backwards registration it produced *correct* hot-reload notification: reloading a texture did reach
its material.

So the tree contained two errors that cancelled, plus two artefacts of them:

- `m_AssetDependents` was empty for every asset, and `UpdateDependents(handle)` — called on the very
  next line of the reload path — was a no-op for everything.
- `GetDependencies(texture)` answered with the *materials that use it*. Any new caller asking the
  documented question got the opposite answer, confidently.

`AnimationAsset::OnDependencyUpdated` spelled its own re-registration the correct way round, so one
asset ended up with edges in **both** directions depending on which path last ran.

The fix had to move both halves at once: swap the six call sites, and delete
`UpdateDependencies` so the reload path keeps only `UpdateDependents`, which is now the one that
works. Swapping the call sites alone would have silently stopped every texture hot-reload.

## What to do instead

Derive the answer from the artefacts that actually hold it, and state the boundary.

`AutomationAssetIndex` scans the project's text asset files and resolves each candidate reference.
Three decisions in it are worth reusing:

- **Extract generously, resolve exactly.** Path references are recognised by their *value* — any
  scalar ending in a known asset extension — not by a list of key names. A key list goes stale the
  moment a component gains a field, and the failure is a missed referrer, i.e. silent data loss. An
  over-collected candidate costs one line in the unresolved count; resolution is the filter.
- **Mirror the engine's own resolver rather than writing a better one.** `EditorAssetManager::ImportAsset`
  resolves a relative asset path in a documented order (project-relative, then the legacy
  project-prefixed spelling, then cwd-relative; issues #887 and #1098). The index copies it step for
  step, because the point of the answer is to predict what the engine will *load*. A smarter
  resolver would make the index disagree with reality. Concretely: `Checkerboard.png` exists under
  both `OloEditor/assets/textures/` and the project's own `Assets/Textures/`, so a filename or
  path-suffix match would invent a referrer.
- **Ship the coverage with the answer.** Every referrer result carries what was *not* searched —
  binary formats skipped and named, files that could not be read, references that resolved to
  nothing, and whether the walk was truncated. An empty referrer list means "nothing found in this
  set", never "nothing references this", and a destructive command refuses outright on a truncated
  index — `force` deliberately does not waive that one, because it is a claim about the *quality of
  the list*, not about the risk the caller is accepting.

## Smell test

- A graph whose writers you can count on one hand is a graph with a coverage boundary. Find it
  before you build on it.
- A comment or log line next to a call that describes the opposite of what the call does is a bug,
  not a stale comment. Check which one the tests pin. Here: neither.
- If a wrong-looking direction seems to work, look for the second wrong direction.
