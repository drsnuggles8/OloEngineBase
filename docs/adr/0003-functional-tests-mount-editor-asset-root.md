# Functional fixtures mount an isolated copy of the editor asset root

The Functional test harness stages assets out of `OloEditor/SandboxProject/`
into a throwaway temp project per test (rather than going hermetic with
test-local or programmatic-only assets, or pointing at the real SandboxProject
directly). Tests reference the same skeletons, clips, prefabs, and scenes
as the runtime does, but `EditorAssetManager`'s on-disk side-effects (registry
writes, file-watcher metadata) are contained in the temp dir and reclaimed
by the OS. We accept that editor content drift can break Functional tests at
a distance — that is the desirable signal.

## Considered options

- **Programmatic by default, `tests/Functional/assets/` for files.**
  Hermetic, no editor coupling, but Functional tests would catch zero
  asset-pipeline regressions and we'd have to maintain a parallel set of
  test-only skeletons/clips/textures that drift from the real ones.
- **Per-fixture choice (programmatic / test-local / editor-mounted).**
  Most flexible, but three asset paths means three sets of "why isn't my
  asset loading" debugging surfaces.
- **Always programmatic, never load files.** Closes the door on
  serialization-round-trip-in-a-Scene tests and on asset-handle integration
  tests — exactly the cross-subsystem failure class Functional should catch.
- **Point directly at `OloEditor/SandboxProject/`.** Cleanest in code, but
  `EditorAssetManager::Initialize` rewrites `AssetRegistry.oar` on every
  scan; tests would dirty the working tree and races between parallel ctest
  invocations would corrupt the registry file.

## Consequences

- `EnableAssetManager(assetsToStage)` copies named SandboxProject-relative
  files into `<temp>/OloEngineL12/<suite>.<name>/Assets/`, writes a minimal
  `.oloproj`, and brings up `EditorAssetManager` against that temp path.
- An editor-content rename or removal can break a Functional test at a
  distance — this is treated as "asset-pipeline regression caught early,"
  not as test-suite flakiness.
- The harness resolves `OloEditor/` from a CMake-injected compile-define
  (`OLO_TEST_EDITOR_ROOT`) so it is independent of the test binary's cwd.
- Functional tests cannot run in environments where the editor asset tree
  is unavailable (matters mainly for CI sharding / WSL minimal images).
- `Project::s_ActiveProject` and `s_AssetManager` are process-static; once
  set, they remain until the next test re-mounts (or process exit). Tests
  that don't call `EnableAssetManager` inherit whichever project was last
  mounted in the process — usually fine because the asset manager is opt-in
  and unused tests don't query it.
