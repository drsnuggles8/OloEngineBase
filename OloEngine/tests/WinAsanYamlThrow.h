#pragma once

// =============================================================================
// OLO_SKIP_YAML_THROW_UNDER_WIN_ASAN — one guard for the clang-cl ASan
// throw-dispatch crash (issue #661).
//
// THE BUG IS IN THE TOOLCHAIN, NOT IN OUR CODE. Under clang-cl +
// /fsanitize=address on Windows, a C++ `throw` travelling through certain
// sanitizer-instrumented frame shapes crashes with SEH 0xc0000005 *inside the
// exception-dispatch machinery*, before any catch clause runs. Reproduced
// locally on LLVM 21: ASan reports an access violation reading null+8 during
// the throw with r9 = 0x19930520, the MSVC C++ throw magic. Neither the input
// shape nor the catch type changes the outcome — only the frame layout does,
// which is why it is clang-version dependent and why some throwing tests pass
// while others crash.
//
// gtest's SEH catcher intercepts first, so the failure reads as
// "SEH exception with code 0xc0000005 thrown in the test body" with no ASan
// report and no stack — which is what makes it expensive to recognise a fourth
// time. Hence this header.
//
// Wrap ONLY the sub-assertion that makes the third-party library throw. The
// rest of the test — including every assertion about well-formed input and
// about rejecting valid-YAML-but-wrong-schema — must keep running everywhere,
// because those paths are ours and are not affected by the toolchain bug.
//
//     TEST(Foo, SerializerRejectsMalformedYAML)
//     {
//     #if OLO_SKIP_YAML_THROW_UNDER_WIN_ASAN
//         // Skipped: issue #661 (clang-cl ASan throw dispatch), see this header.
//     #else
//         EXPECT_FALSE(Parse("key: [unclosed"));
//     #endif
//         EXPECT_FALSE(Parse("WrongRootKey: 1\n"));   // always runs
//     }
//
// Windows-ASan coverage of the yaml-cpp throw/catch plumbing itself is not
// lost: `EngineSubsystemSmoke.ProjectLoadMalformedYAMLFailsCleanly` throws
// through a frame shape the bug does not hit and stays enabled in every
// configuration.
//
// Retire this header (and every use of it) once the upstream LLVM fix lands —
// #661 tracks that, along with the standalone repro to file upstream.
// =============================================================================

#include "OloEngine/Memory/Platform.h" // OLO_ASAN_ENABLED

#if OLO_ASAN_ENABLED && defined(_WIN32)
#define OLO_SKIP_YAML_THROW_UNDER_WIN_ASAN 1
#else
#define OLO_SKIP_YAML_THROW_UNDER_WIN_ASAN 0
#endif
