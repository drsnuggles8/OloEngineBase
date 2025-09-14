# OloEngine Asset Management: Feature Parity Plan with Hazel

This plan outlines concrete steps to bring OloEngine's asset management system to feature parity with Hazel's, based on a comparative review of both codebases as of 2025-09-13.

## Summary of Gaps (from comparison)

- Editor/runtime split exists in OloEngine, but some editor features are stubbed or incomplete (e.g., dispatching `AssetReloadedEvent`, dependency propagation).
- Asset dependency graph present but needs richer propagation and cycle-safety.
- Asset registry is thread-safe in OloEngine (good), but editor ergonomics like fast iteration helpers and public const view (Hazel) can be added.
- Serializer coverage is broader in OloEngine, but several serializers are not fully implemented (marked TODO or warn) for pack support.
- Asset Pack pipeline exists in OloEngine runtime, but there’s no explicit editor-side pack building workflow surfaced to users.
- Async asset system exists with file watcher; ensure parity with Hazel’s queueing/state, and polish polling fallback.
- Placeholder assets implemented for Texture2D and Font only; expand to other common types.
- Integration touchpoints: Ensure `Project` wire-up, `AssetManager` facade, and editor panels use full API (reload, ensure current, async sync, dependency updates).

## Goals

1. Complete editor features: hot-reload events, dependency update propagation, robust metadata updates, and improved iteration helpers.
2. Solidify async asset pipeline: queueing, status transitions, and file watcher fallback parity.
3. Complete serializer implementations and pack I/O parity.
4. Expose an editor asset pack build pipeline and settings.
5. Improve placeholder assets coverage for robustness.

## Phased Implementation Plan

### Phase 1 — Editor Asset Lifecycle & Events ✅ COMPLETE

✅ **COMPLETED:**
- ✅ Implement and dispatch `AssetReloadedEvent` when an asset is reloaded successfully in `EditorAssetManager::ReloadData`.
- ✅ Asset discovery and registration system (`ScanDirectoryForAssets`)
- ✅ File watcher integration with real-time asset change detection
- ✅ EditorAssetManager initialization and Project integration
- ✅ Asset registry persistence (.oar file serialization/deserialization)
- ✅ Hot-reload triggering for tracked assets
- ✅ On asset dependency changes, call `UpdateDependents` and invoke `OnDependencyUpdated` on dependents (complete propagation with cycle prevention).
- ✅ Ensure `EnsureCurrent` and `EnsureAllLoadedCurrent` mirror Hazel behavior (OloEngine implementation is actually superior with better error handling and thread safety).
- ✅ Add `ForEachLoadedAsset` helper and verify thread-safe `GetLoadedAssets()` copy in both managers.

**Acceptance criteria:** ✅ PASSED
- ✅ File change triggers reload; dependent materials/textures get notified; an editor layer can subscribe to `AssetReloadedEvent`.

### Phase 2 — Async Flow & File Watchers ✅ COMPLETE

✅ **COMPLETED:**
- ✅ Review async queue semantics to match Hazel: prevent duplicate queueing, set `AssetStatus::Loading`, clear on completion.
- ✅ Validate file watcher startup, error handling, and fallback thread loop cadence; add telemetry counters (loads per minute, queue length) to aid debugging.
- ✅ Add `SyncWithAssetThread()` no-op in runtime and full sync in editor (exists but verify lifecycle, called at editor tick).
- Queue limits: max 4096 items; per-asset dedupe window 500ms.
- Timeouts: load op 10s (configurable); retries: 3 with exponential backoff (250ms, 500ms, 1s).
- Cancellation: supersede in-flight loads on newer file mtime.
- Metrics: `asset.loads_per_min`, `asset.queue_depth`, `asset.load_latency_ms{result}`, `asset.reload_dropped{reason}`.

**Acceptance criteria:** ✅ PASSED
- ✅ Continuous changes to files reliably trigger single queued reload and settle to `Ready`.### Phase 3 — Serializer Coverage & Pack Parity

- Finish pack serialization for currently stubbed serializers:
  - `EnvironmentSerializer`, `AudioFileSourceSerializer`, `MeshColliderSerializer`, `ScriptFileSerializer`, `MeshSourceSerializer`, `MeshSerializer`, `AnimationGraphAssetSerializer`, `SoundGraphSerializer` (remove TODO logs, implement minimal working writes/reads).
- Validate dependency registration in `MeshSerializer`, `StaticMeshSerializer`, `AnimationAssetSerializer` to populate dependency graph.
- Add unit-like smoke tests for pack round-trip for at least: Texture2D, Font, Material, Scene, Prefab, StaticMesh, Animation.

Acceptance criteria:
- Build an asset pack containing the above types, load in runtime, and resolve dependencies without errors.

## Pack Format Contract
- Version: `AssetPackFile::Version` (semver). Reader must reject newer major versions.
- Index: varint-encoded entries keyed by `AssetHandle`; UTF-8; little-endian.
- Integrity: per-blob SHA-256 + pack-level Merkle root; fail closed on mismatch.
- Compression: zstd level 3 (configurable); store uncompressed threshold (≤4 KiB).
- Determinism: stable ordering by `AssetHandle`; identical inputs → identical pack hash.

### Phase 4 — Editor Pack Build Pipeline

- Add an editor command/panel action: “Build Asset Pack…”.
- Implement `AssetPackBuilder` (or equivalent utility) that:
  - Walks the `AssetRegistry`,
  - Calls `AssetImporter::SerializeToAssetPack` per asset,
  - Writes index and scenes with `SceneAssetSerializer::SerializeToAssetPack`.
- Persist pack output path in project settings; default to `Assets/AssetPack.olopack` (already used by runtime).

Acceptance criteria:
- Editor can build an `.olopack`, Runtime loads it at startup when present.

### Phase 5 — Placeholder Assets & UX

- Expand `AssetManager::GetPlaceholderAsset` to include:
  - `EnvironmentMap`, `MaterialAsset` (basic), `Mesh`/`StaticMesh` (unit cube), `AudioFile` (silence), `Prefab` (empty), `Scene` (empty).
- Ensure all engine code paths that require an asset gracefully fall back to placeholders when missing.

Acceptance criteria:
- No hard failures on missing assets; visible placeholders in renderer.

## Integration Points

- Project: Confirm `Project::SetAssetManager` is called appropriately for editor vs runtime.
- Renderer/Scene/Editor Panels: Replace direct file loads with asset API calls where needed; use `EnsureCurrent` and async as appropriate.
- Logging/Profiling: Use existing `OLO_CORE_*` and profiler scopes for all critical paths.

## Risks & Mitigations

- Pack versioning mismatches: include `AssetPackFile::Version` checks (already present) and graceful errors.
- Dependency cycles: add detection during `RegisterDependency` or during update propagation to avoid infinite recursion.
- Thread safety: maintain locking discipline around `m_LoadedAssets`, `m_MemoryAssets`, and registry access.

## Stretch Improvements (post-parity)

- Asset reference counting telemetry and leak detection views in editor.
- Background streaming for large meshes/textures.
- Content validation passes during pack build (e.g., missing referenced textures in materials).

Track progress via issues/PRs tied to these phases.
