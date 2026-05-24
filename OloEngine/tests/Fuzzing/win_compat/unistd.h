// Minimal `<unistd.h>` shim for compiling compiler-rt's
// `ubsan_minimal_handlers.cpp` under clang-cl. We only need `write(fd, buf, n)`
// (stderr diagnostic write). Windows MSVCRT spells it `_write` in `<io.h>`.
//
// Added to the include path for the `olo_libubsan_minimal` target only — see
// `cmake/Fuzzing.cmake`. Not used by anything else in the engine.
#pragma once

#include <io.h>

// `ubsan_minimal_handlers.cpp` calls `write(2, msg, strlen(msg))`. The MSVC
// equivalent is `_write`; the signature matches.
#define write _write
