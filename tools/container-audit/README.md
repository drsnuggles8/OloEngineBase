# Map borrow audit for #738

**Decision: keep the TMap gate closed.** Continue the array/string migration and
retain standard maps, as allowed by reversal condition 2 of
[ADR 0012](../../docs/adr/0012-adopt-the-ue-container-library-for-engine-owned-data.md).
The matcher is a candidate finder, not a reference-lifetime proof.

Current-code correction to the ADR/issue premise: `ContainerAllocationPolicies.h`
defines `OLO_USE_COMPACT_SET_AS_DEFAULT=0` before `Set.h` can apply its fallback of
1. Consequently the actual default `TSet` alias is `TSparseSet`, as confirmed by
the compilation probe during this migration. The sparse implementation is active,
not unreachable. Both set implementations can relocate element storage, so this
correction does not reopen the map reference-stability gate.

## Reproduce

Use the configured clang-cl developer environment and an exported CMake
`compile_commands.json`. The default database is `build-cached/compile_commands.json`;
`--database` and `--clang-query` override it and the tool executable.

```powershell
# Text inventory only; no compiler or build lock needed.
python tools/container-audit/run.py

# One AST per lock acquisition; each result is retained in the same summary.
foreach ($auditCase in @('fixture', 'Renderer', 'Terrain', 'Dialogue', 'Scene', 'UI')) {
    pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 `
        -Command "python tools/container-audit/run.py --case $auditCase"
}
```

Results go to `build-cached/container-audit`: `summary.json`, one raw log per
case, and `inventory.tsv`. The summary records the compiler database SHA-256,
clang-query version, exit code, parse failures and match counts. A parse error
is **not** a clean audit even when clang-query emits partial matches or exits 0.
The fixture exits unsuccessfully if the expected candidate/miss matrix changes.

## Coverage

The textual scan covers every standard map type spelling in Renderer, Terrain,
Dialogue, Scene and UI. It includes declarations, aliases and signature uses;
it is not a count of distinct map objects. Every row names its file and line and
whether that main file was scanned, failed parsing, or remains unscanned.
Headers remain unproved: the query deliberately matches functions expanded in
the main file, even when a representative TU parses the header.

The initial 2026-09-21 worktree inventory contains:

| Subsystem | Type-use lines | Files |
| --- | ---: | ---: |
| Renderer | 217 | 64 |
| Terrain | 21 | 12 |
| Dialogue | 5 | 4 |
| Scene | 26 | 11 |
| UI | 8 | 2 |

Representative bodies are `VirtualGeometryPageStore.cpp`, `TerrainStreamer.cpp`,
`DialogueSystem.cpp`, `Scene.cpp`, and `UINavigationSystem.cpp`. These samples
cover the four planned migration groups, with Scene and UI inspected separately;
they do not constitute exhaustive AST coverage of the inventory.

## Why the gate stays closed

The fixture includes one direct borrow followed by insertion, one safe local
borrow, three deliberately missed lifetime patterns, and two false positives.
The unsafe labels mean unsafe **after replacement with relocatable UE map storage**;
`std::unordered_map` element references survive insertion and rehash today.

| Fixture pattern | Expected candidate | Interpretation |
| --- | --- | --- |
| Local `operator[]` reference followed by `emplace` | yes | Useful direct candidate |
| Local borrow, consumed with no later mutation | no | Known safe control |
| Return `&find(...)->second` | no | Missed escaped borrow |
| Store a found-element pointer through an output parameter | no | Missed escaped borrow |
| Keep an `operator[]` reference across a mutating helper | no | Missed interprocedural mutation |
| Mutate before taking the reference | yes | False positive: no ordering analysis |
| Borrow one map and mutate another | yes | False positive: no container identity analysis |

These are structural gaps, not an estimated project-wide false-negative rate.
The query also lacks alias, callback and control-flow lifetime analysis, and
does not constrain `operator[]` to standard maps.

A concrete production counterexample is
[`VirtualGeometryPageStore::Fetch`](../../OloEngine/src/OloEngine/Renderer/VirtualGeometry/VirtualGeometryPageStore.cpp).
It publishes `&it->second.Payload` through `outPayload`; the header promises that
pointer until `Release`. `StageLocked` inserts other ready entries while such
leases exist. Node-based storage preserves the leased payload address, whereas
a UE map element-storage reallocation would invalidate it. The borrow and insertion are in
different functions and use `find`, so the matcher cannot prove this contract.

A contrasting safe pattern is `Scene::ScheduleAnimalPopulationForFrame`: populate
`m_AnimalScheduleRuntime` first, then build borrowed state pointers and consume
them without adding entries to that map. Mutations of `m_AnimalSchedules` later
in the function are a different map. Lexical co-occurrence cannot make this
distinction. Future TMap adoption needs a stronger analysis and a per-container
contract review; zero candidates alone is insufficient.
