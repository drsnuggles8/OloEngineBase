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

## Notable seeds

### `spirv_cross/crash_invalid_opcode_8962.spv`

48-byte SPIR-V blob the libFuzzer mutator discovered against
[`magic_only.spv`](spirv_cross/magic_only.spv). The header is well-formed
(magic, version 1.0, generator/bound/schema all parse), but instruction
word 5 declares opcode `0x2302` (8962) — not a real SPIR-V opcode. Stock
`spirv-cross.exe` from the Vulkan SDK crashes with `STATUS_ACCESS_VIOLATION`
during `Parser::parse()` on this input; SPIRV-Tools' `spvValidateBinary`
rejects it cleanly with "Invalid opcode: 8962".

Upstream's position on parser robustness against malformed input:
[KhronosGroup/SPIRV-Cross#2635](https://github.com/KhronosGroup/SPIRV-Cross/issues/2635)
(closed WONTFIX, 2026-05-21) — "The parser is not intended to exhaustively
deal with any invalid SPIR-V input. … SPIRV-Cross is not a bad clone of
spirv-val." `FuzzSpirvCross` now pre-validates with `spvValidateBinary` to
match the contract, so this seed is dropped on entry; it stays in the
corpus as documentation of the originally-discovered crasher and to guard
against the validator regressing.
