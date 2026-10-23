---
paths:
  - "OloEngine/tests/**"
---

# Tests

- **Classify before writing.** Two independent axes: the renderer pyramid (L1–L11 plus
  plumbing/cullinglod/shaderpipe/integration/meta) and the Functional axis (cross-subsystem seams
  driven by the real `Scene::OnUpdateRuntime`).
  [testing-architecture.md](../../docs/agent-rules/testing-architecture.md) says where a test belongs.
- **Every test `.cpp` carries `// OLO_TEST_LAYER: <id>`** near the top (or a `file_layer_map` entry
  in `test_catalogue.json`, not both). Pre-commit blocks an unclassified file. The rendered
  catalogue under `docs/test-catalogue.*.md` is generated and git-ignored; never hand-edit it.
- **A new test file needs an entry in `OloEngine/tests/CMakeLists.txt`**, which lists every source;
  an unlisted file is never built and its filter matches nothing.
- **A test that builds a texture needs the GPU gate**, even when the load fails. Run a new test
  alone before pushing.
- Size tests like their neighbours. Scratch checks are not committed.
- Every substituted seam hides one: read
  [substituted-seams-compound.md](../../docs/agent-rules/substituted-seams-compound.md) before
  stubbing a subsystem.
- Rebase modes `--olo-golden-rebase` and `--olo-perf-rebase` only after a deliberate visual change
  or a hardware move. Benchmark captures: `--olo-capture-manifest=<manifest>`
  ([renderer-benchmarks.md](../../docs/guides/renderer-benchmarks.md)).
- The test binary runs from the repo root; temp output stays in the per-test temp dir (pre-commit
  `test-temp-dir-isolated`).
