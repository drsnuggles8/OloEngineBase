# Progression data (experience curves, skill trees, character classes) ships as AssetManager asset types — not as `QuestDatabase`-style static registries

Issue [#635](https://github.com/drsnuggles8/OloEngineBase/issues/635) adds the
RPG character-progression pillar: an XP/level curve, skill/talent trees with a
prerequisite DAG, and character class archetypes. All three are authored
gameplay *data*, so the first design question was which of the engine's two
existing database patterns they follow.

## The two existing patterns

1. **Static string-keyed registries** — `QuestDatabase`, `ItemDatabase`,
   `AffixDatabase` (`Gameplay/Quest/`, `Gameplay/Inventory/`): all-static
   classes over a Meyers-singleton `unordered_map`, loaded eagerly by
   `EditorLayer::OpenProject` from a directory of YAML files with a dedicated
   extension (`.oloquest`, `.oloitem`). Simple global lookup by string ID, but:
   they are **editor-only** (neither `OloRuntime` nor `OloServer` ever calls
   `LoadFromDirectory`, so standalone games get empty databases), they have
   **no hot-reload** (their extensions are invisible to `AssetExtensions`, so
   the filewatcher ignores them), no asset-pack support, and process-global
   state that every test must `Clear()` in SetUp/TearDown.

2. **AssetManager asset types** — `DialogueTreeAsset`, `FluidSettings` et al.:
   `Asset`-derived classes with an `AssetType` enum value, an
   `AssetSerializer`, and a registered extension. Referenced by `AssetHandle`,
   resolved lazily, packaged into `.olopack` for the runtime, hot-reloaded by
   the filewatcher, and creatable/openable from the editor content browser.

## Decision

`ExperienceCurve` (`.oloxpcurve`, AssetType 36), `SkillTreeDatabase`
(`.oloskilltree`, 37), and `CharacterClassDatabase` (`.olocharclass`, 38) are
**AssetManager asset types** (pattern 2).

Rationale:

- **Runtime parity.** Progression is core game state; the static-registry
  pattern's editor-only loading hole (a standalone `OloRuntime` game would
  have no classes/curves/trees at all) is disqualifying for the pillar that
  drives save-games and combat scaling. As assets, the databases ride the
  asset pack and the runtime asset manager for free.
- **Issue contract.** #635's acceptance criteria say "databases load via
  `AssetManager`" explicitly.
- **Validated load.** Skill trees need structural validation (unique node
  ids, prerequisite closure, cycle rejection) at load time; the serializer's
  parse-into-locals → `Validate()` → commit shape (the `DialogueTreeSerializer`
  discipline) gives that a natural home, and a failing asset is rejected
  loudly instead of half-registered.
- **Editor tooling.** The Skill Tree editor panel and the content-browser
  create/open flows key off `ContentFileType`/`AssetType`, which only exist
  for real asset types.

Consequences accepted:

- **By-handle indirection instead of global string lookup.** Consumers hold
  `AssetHandle`s (`ProgressionComponent::ExperienceCurveHandle`,
  `ClassDatabaseHandle` + a `ClassID` string key *inside* the class database,
  `SkillTreeHandle`, plus per-class `SkillTrees` handle lists) and re-fetch via
  `AssetManager::GetAsset<T>` at use time. `ProgressionSystem` deliberately
  **never caches** the `Ref`s, so object-replacing hot-reload needs no
  `EditorLayer::OnAssetReloaded` case and stale-Ref bugs cannot exist.
- **Graceful degradation without an asset manager.** Headless unit tests (and
  a component with handle 0) resolve no assets; the system falls back to the
  engine-default curve (`ExperienceCurve::DefaultXPForLevelUp`, 100 XP × level,
  max level 99) and class-less semantics (any defined attribute spendable at
  1.0/point, default per-level point grants). Tests that need real data create
  memory-only assets via `AssetManager::AddMemoryOnlyAsset` under
  `FunctionalTest::EnableAssetManager({})`.
- **Cross-asset references are handles in YAML.** A class database referencing
  its skill trees/curve stores raw `u64` handles, which only resolve inside
  the project whose registry assigned them — authored sample content must be
  wired in-editor (or via memory-only assets in tests), not by hand-typing
  handle literals.

`QuestDatabase`/`ItemDatabase` stay as they are — migrating them is out of
scope here, but this ADR is the precedent for where new gameplay databases
should land.
