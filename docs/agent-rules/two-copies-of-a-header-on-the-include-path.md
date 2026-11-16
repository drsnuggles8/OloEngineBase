# A header that exists twice on the include path is included by one spelling only

Applies to stb today, and to any library that is both pinned here (FetchContent) and installed by
vcpkg as another port's dependency.

## Rules

1. **Include stb as `<stb_image/stb_image.h>`, `<stb_image/stb_image_write.h>`,
   `<stb_image/stb_truetype.h>`. Never by the bare name.** The bare `<stb_image_write.h>` is the
   copy vcpkg installs for assimp, about two years older than the pin, and the implementation that
   links is the pinned one. The pre-commit hook `stb-include-path`
   (`scripts/check_stb_include_path.py`) fails on a bare `<stb_*.h>` outside `vendor/`.
2. **When a dependency bump changes a function's signature, grep for every spelling of its
   header.** The compiler checks a call against the header that translation unit included, and the
   linker matches a C function by name alone, so a caller built against the old declaration links
   without a diagnostic.
3. **Read an access violation inside a C library as a possible caller/callee disagreement.** In a
   debugger, an argument printed with a value its declared type cannot hold (an `int` stride of
   140,698,833,651,200) is the sign.

## What happened (2026-11-16)

The stb pin moved to a revision where `stbi_write_png` takes its stride as `ptrdiff_t`; the older
header declares `int`. Six evidence tests included the bare name. On Windows x64 the sixth argument
is passed on the stack, the caller wrote four bytes of it and the writer read eight, so the stride
was whatever the stack held above: 26 tests in the skin and fluid suites died with an access
violation in `stbiw__encode_png_line`, deterministically, also when run alone. The other 195 test
files that write a PNG include the staged path and were unaffected.

It was green in CI and on the Renovate PR. The affected suites need a GPU and do not run there, and
on Linux the sixth integer argument travels in a register whose upper half the caller clears.
