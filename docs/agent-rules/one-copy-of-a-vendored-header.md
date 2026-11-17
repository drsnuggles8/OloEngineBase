# Include a vendored single-header library by the path the engine compiles it from

Rules for any `#include` of an stb header, and for adding a dependency that bundles one.

## Rules

1. **Include stb as `<stb_image/stb_image.h>`, `<stb_image/stb_image_write.h>`,
   `<stb_image/stb_truetype.h>`, never by the bare file name.** The implementation is compiled once
   (`STB_IMAGE_WRITE_IMPLEMENTATION` in `SaveGame/ThumbnailCapture.cpp`) from the `stb_image/` copy.
   A bare `<stb_image_write.h>` finds another dependency's bundled copy first.
   `VendoredStbIncludeTest` fails on a bare include anywhere in the engine, editor or tests.
2. **A dependency that bundles a header the engine also vendors is a second declaration of the same
   functions.** Before adding its folder to an include path, look for file names that collide with
   an existing vendored header.

## What happened

bcdec and xatlas each ship their own `stb_image_write.h` (v1.16 and v1.14), and their folders are on
the include path. The engine vendors v1.18. Six evidence tests included `<stb_image_write.h>` and
compiled against bcdec's copy while linking the engine's implementation.

That was harmless until the stb update of 2026-11-16 changed `stbi_write_png`'s last parameter,
`stride_in_bytes`, from `int` to `ptrdiff_t`. On Win64 the sixth argument is passed on the stack.
The six callers stored 32 bits; the implementation read 64. The upper half was whatever the stack
held: zero in most builds, garbage in one, where
`FluidVisualEvidenceTest.PoolRendersFromMultipleAnglesAndChangesTheFrame` died with an access
violation inside the PNG writer on its second capture. Nothing about the test or the frame was
wrong, and the other five passed.

The stack named the function (`stbi_write_png_to_mem`, a stride of 140694538686208). The fix was
the six includes.
