---
paths:
  - "**/*.glsl"
  - "**/*.{vert,frag,comp,geom,tesc,tese}"
  - "OloEditor/assets/shaders/**"
---

# Shaders

Every shader compiles through shaderc → SPIR-V, which is stricter than GL GLSL, and runs on two
backends. The full rules are [docs/agent-rules/glsl-shaders.md](../../docs/agent-rules/glsl-shaders.md);
`olo-read-gates` refuses a shader edit until it has been read this session. The ones that fail most:

- Every non-opaque `uniform` lives in a named `std140` UBO block; samplers may stand alone.
- Padding names are block-specific (`_pbrMaterialPad0`, not `_padding0`): nameless blocks share the
  global scope, and a duplicate is a hard SPIR-V error invisible to every OpenGL test.
- A UBO declared in two stages of one file matches field for field, names included; a mismatch
  fails only at link time and the pass then draws nothing, silently.
- `projection[1][1]` is negative on Vulkan: `abs()` it before using it as a scale.
- A new `#extension` must pass `python scripts/check_shader_extension_floor.py`.
- Validate with `glslc` first (target `vulkan1.2` for shaders using Vulkan builtins; the
  `opengl4.5` environment false-fails `gl_InstanceIndex`).
- The engine's include expander ignores `#ifdef`, so a guarded `#include` is always included.
- A shader change is a rendering change: see `renderer-verification.md` in this directory.
