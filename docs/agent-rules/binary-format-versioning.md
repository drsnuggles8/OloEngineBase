# Versioning a fixed-order binary archive (save-game / asset-pack)

Short rule for anyone bumping `kSaveGameFormatVersion`, `AssetPackFile::Version`,
or introducing a new fixed-order binary format that needs to survive a schema
change without breaking existing files on disk.

## The trap

Scene YAML is self-describing (a key can be missing and the reader just skips
it), so versioning it is a tree-migration problem: read the file, note its
recorded version, walk an ordered chain of `Migrate_VN_to_VNplus1(node)` steps
over the parsed tree (see `SceneSerializer::MigrateSceneYAML`).

A `FArchive`-backed binary format (`SaveGameComponentSerializer`,
`AssetPack::Load`) is **not** self-describing at the field level: every
`ar << field` reads or writes the next N bytes unconditionally, in the exact
order they're written. If a save produced by an older build is missing the
bytes for a field added later, every `ar <<` after that field desyncs — the
reader silently reinterprets whatever bytes come next as the wrong field,
producing garbage instead of an error. That's why the exact-match version
check (`FormatVersion == kSaveGameFormatVersion`) existed: it was hiding this
desync risk by refusing to load anything but the exact current format,
turning "every future schema change permanently breaks every existing save"
into an accepted trade-off (issue #454).

## The rule

**Relax the header check to a range, not an exact match — reject only a
version *newer* than this build understands (its layout is unknowable), and
gate every field added after the format's first version behind the archive's
recorded version, not behind absence of a migration step.**

Concretely (see `SaveGameComponentSerializer.cpp` / `AssetPack.cpp`):

1. **Header check becomes a range.** `IsValid()` (save-game) / `AssetPack::Load`
   (asset-pack) accept `[MinSupportedVersion, CurrentVersion]`, not just
   `== CurrentVersion`. A version above `CurrentVersion` is still rejected
   outright — this build cannot safely guess a future layout.
2. **Thread the file's recorded version into the archive.** `FArchive` carries
   a generic `ArArchiveVersion` (`GetArchiveVersion()`/`SetArchiveVersion()`),
   set once from the header (`SaveGameSerializer::RestoreSceneState`'s
   `formatVersion` parameter → `reader.SetArchiveVersion(...)`) and copied onto
   every per-component sub-reader the deserializer spins up (the
   `LOAD_COMPONENT` macro's `cr.SetArchiveVersion(reader.GetArchiveVersion())`
   — easy to forget if a format ever nests archives).
3. **Gate each field at the version it was introduced in**, not with a comment
   promising the header check already excluded old data:
   ```cpp
   // WRONG — desyncs the moment the header check is relaxed:
   // Appended at the end; pre-vN archives are rejected by the header check.
   ar << c.NewField;

   // RIGHT — gate the read/write itself:
   if (ar.IsSaving() || ar.GetArchiveVersion() >= kIntroducedInVersion)
       ar << c.NewField;
   ```
   `ar.IsSaving()` short-circuits the check on write, so this build always
   *writes* the full current layout — only *loading* an old file skips a
   not-yet-existing field, leaving it at the component's constructor default.
   `SaveGameComponentSerializer.cpp`'s `HasFieldsSince(ar, introducedInVersion)`
   is the shared helper; `TerrainComponent` (3 gated blocks) and
   `IKTargetComponent` (1) are the reference examples.
4. **Keep a no-op migration-chain scaffold** for the day a change can't be
   expressed as a per-field gate (a field renamed/removed, a cross-field
   invariant) — `MigrateSceneYAML` (scene) and `MigrateAssetPackIndex`
   (asset-pack) are both empty today by design; they exist so the next
   breaking change has an obvious place to add a step instead of reinventing
   the plumbing under deadline pressure.
5. **Sanitize on load regardless of whether a gate fired.** A field skipped
   because the archive predates it is left at its constructor default — make
   sure that default passes whatever `Sanitize*`/clamp logic runs after the
   gated block (it should, by construction, but check when adding a new
   gate — a bad default only surfaces the first time an old save is loaded).

Asset packs are a **build artifact** (regenerable from source assets), not
irreplaceable player data like a save-game — so `AssetPackFile` doesn't yet
have a real historical layout change to migrate (its `Version` has never
actually been bumped). The scaffold above is intentionally speculative there:
it establishes the pattern so the *next* dev who adds a field to
`AssetInfo`/`SceneInfo`/`IndexTable` gates it correctly on the first try,
instead of learning the desync trap from a corrupted pack.

## Guard

`SaveGameFileTest.cpp` (`SaveGameHeaderTest.OlderSupportedFormatVersionIsValid`
/ `FormatVersionBelowMinSupportedIsInvalid` / `FormatVersionAboveCurrentIsInvalid`)
pins the header range check. `SaveGameVersionMigrationTest.cpp` exercises the
per-field gate directly for `TerrainComponent`/`IKTargetComponent` and, in
`FullSaveWithPreV3TerrainRestoresThroughRestoreSceneState`, proves the version
threads all the way from `RestoreSceneState` through the per-component reader
with no desync. `AssetPackTest.cpp`
(`LoadSucceedsWithOlderSupportedVersion` / `LoadFailsWithVersionBelowMinSupported`)
pins the equivalent range check for asset packs.
