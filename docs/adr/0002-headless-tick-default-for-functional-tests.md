# Headless `Scene::OnUpdateRuntime` is the default tick model for Functional tests

`Scene::OnUpdateRuntime(Timestep)` is already separable from `RenderScene*`,
so Functional fixtures tick simulation only by default — no GL context, runs
on WSL/Linux, parallelisable. A separate `RenderingFunctionalTest` fixture
base (deferred) will attach the GL context and call `RenderScene` after each
tick for the rare test where a cross-subsystem bug genuinely manifests
through the renderer.

## Considered options

- **Always full sim + render** (UE-FunctionalTest's default). Mirrors
  production exactly but requires a GL context on every test runner, kills
  WSL parallelism, and pays the renderer cost for tests that don't need it.
  Rejected because >90% of cross-subsystem bugs we expect to catch
  (Animation/Physics/Scripting/Networking/Audio/Asset interactions) are
  logic bugs the renderer cannot help diagnose.
- **Strictly headless, never render.** Cleanest separation but rules out
  cross-cutting bugs that span sim and render in the same frame (rare in
  practice, but they do exist — ImGui overlays that mutate Scene state on
  draw are an obvious example).
- **Full by default, headless when annotated.** Inverse default; less
  ergonomic for the common case and pulls the GL stack into every CI shard.

## Consequences

- Functional tests are expected to be fast and shardable; CI can run them
  on every PR without the full GL stack.
- Renderer-attached tests will be a minority and will pay the cost they
  incur — they live in a separate fixture base (deferred) so the default
  path stays cheap.
- Tests must not assume rendering side-effects (e.g., GPU buffer state,
  framebuffer contents) unless they inherit the renderer-attached base.
