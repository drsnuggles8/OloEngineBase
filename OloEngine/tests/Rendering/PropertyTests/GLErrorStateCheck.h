// =============================================================================
// GLErrorStateCheck.h
//
// Cross-test GL-error-state guard (issue #485).
//
// Every GPU test in this binary shares ONE process-wide GL 4.6 context. A test
// that returns while `glGetError()` still has a pending error silently corrupts
// a *later, unrelated* test: the victim's first GL readback drains the leaked
// error, misattributes it to its own call, and fails (or self-heals) for
// reasons that have nothing to do with the victim. The real case that motivated
// this: `SphereAreaLightShadowEvidenceTest` left `GL_INVALID_OPERATION`, and
// `ProceduralSkyBakeTest` then got a spurious all-black cubemap. Production was
// fixed (readback helpers now drain stale errors, commit 69aa9357), but
// *detecting* which test was the source cost hours of bisecting.
//
// This installs a GoogleTest event listener whose `OnTestEnd` drains + inspects
// the GL error queue after EVERY test. If a test left the context dirty, the
// listener drains it (so the next test starts clean regardless) and fails THAT
// test — pinning pollution to its source, not its victim.
//
// Why a listener and not a fixture `TearDown()`? Most GPU tests are plain
// `TEST()` bodies gated on `OLO_ENSURE_GPU_OR_SKIP()`, not `TEST_F` subclasses
// of a shared renderer fixture — a fixture teardown would miss them. A global
// listener catches every test regardless of fixture, mirroring the existing
// `TestFailureCapture` listener. Because gtest fires `OnTestEnd` in reverse
// listener order (the repeater's "End" events are reversed), a failure added
// here lands in the test result before the result printer's `OnTestEnd` reads
// it, so the polluting test prints `[ FAILED ]`.
//
// Headless-safe: when there is no live GL context (CI without a GPU, where every
// GL test SKIPs) the check is a clean no-op — it never fabricates a failure.
// =============================================================================
#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine::Tests::GLErrorState
{
    // Drains ALL pending GL errors from the current context and returns the
    // FIRST one encountered (`GL_NO_ERROR` / 0 if the queue was clean, or if
    // there is no live GL context). `outCount` receives the number of distinct
    // errors drained. Bounded at 64 iterations so a lost context (glGetError
    // stuck on GL_CONTEXT_LOST) can't spin forever — matches Utils::DrainGLErrors.
    //
    // Safe to call with no GL context: returns 0 without touching GL.
    u32 DrainAndGetFirstError(u32& outCount);

    // Human-readable name for a GL error enum (e.g. "GL_INVALID_OPERATION").
    // Pure; needs no GL context.
    const char* GlErrorString(u32 err);

    // Installs the GL-error-state listener on the global gtest listener list.
    // Call once from main() after InitGoogleTest. Idempotent.
    void RegisterListener();
} // namespace OloEngine::Tests::GLErrorState
