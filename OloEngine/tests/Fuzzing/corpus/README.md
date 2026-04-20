# Fuzz Corpus

Seed inputs for each libFuzzer harness. Files are organised per-target:

```
corpus/
  animation_binary/
  mesh_binary/
  input_action_yaml/
  image_decoder/
  scene_yaml/
  assimp_mesh/
  spirv_cross/
```

## Seeding strategy

Each corpus directory is pre-populated with:

1. **Valid files** — a handful of real `.oanim` / `.omesh` / input-action
   YAML files produced by the editor, so the fuzzer starts with a known
   structure and can mutate meaningful fields.
2. **Known past crashers** — every bug that surfaces gets a minimised
   reproducer committed here so regressions are caught on the first run.
3. **Intentionally broken inputs** — truncated headers, zero-byte files,
   single-byte files, magic-only files — to accelerate coverage of early
   bailout paths.

Files are never deleted from the corpus without review: they represent
coverage that the harness should keep hitting forever.

## Adding a corpus file

```bash
echo -n "" > corpus/mesh_binary/empty
printf 'OMSH' > corpus/mesh_binary/magic-only
```

The fuzzer reads the corpus automatically when invoked by the `-smoke`
CMake target, or directly:

```bash
./FuzzMeshBinary -max_total_time=60 corpus/mesh_binary/
```
