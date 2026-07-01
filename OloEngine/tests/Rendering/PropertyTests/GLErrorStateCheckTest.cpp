// OLO_TEST_LAYER: L4
//
// Self-test for the cross-test GL-error-state guard (issue #485). Proves the
// detection helper behind the global listener:
//   - reports GL_NO_ERROR on a clean context,
//   - detects a deliberately-leaked GL error and identifies it,
//   - actually DRAINS the queue (a second read comes back clean).
//
// Each case leaves the GL error queue clean on exit (it drains through the
// helper under test), so the global GLErrorStateListener does not fail it —
// which is itself the drain half of the contract, verified in place.
#include "OloEnginePCH.h"

#include "GLErrorStateCheck.h"
#include "RenderPropertyTest.h"

#include <gtest/gtest.h>
#include <glad/gl.h>

namespace OloEngine::Tests
{
    // The pure enum→string mapping needs no GL context.
    TEST(GLErrorStateCheckTest, MapsKnownErrorEnumsToNames)
    {
        EXPECT_STREQ(GLErrorState::GlErrorString(GL_NO_ERROR), "GL_NO_ERROR");
        EXPECT_STREQ(GLErrorState::GlErrorString(GL_INVALID_ENUM), "GL_INVALID_ENUM");
        EXPECT_STREQ(GLErrorState::GlErrorString(GL_INVALID_VALUE), "GL_INVALID_VALUE");
        EXPECT_STREQ(GLErrorState::GlErrorString(GL_INVALID_OPERATION), "GL_INVALID_OPERATION");
        EXPECT_STREQ(GLErrorState::GlErrorString(GL_OUT_OF_MEMORY), "GL_OUT_OF_MEMORY");
        // An enum outside the known set still yields a non-null, stable string.
        EXPECT_STREQ(GLErrorState::GlErrorString(0xDEAD), "GL_<unknown error>");
    }

    // A clean context drains to GL_NO_ERROR with a zero count.
    TEST(GLErrorStateCheckTest, CleanContextReportsNoError)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Establish a known-clean baseline.
        u32 preCount = 0;
        (void)GLErrorState::DrainAndGetFirstError(preCount);

        u32 count = 0;
        const u32 err = GLErrorState::DrainAndGetFirstError(count);
        EXPECT_EQ(err, static_cast<u32>(GL_NO_ERROR));
        EXPECT_EQ(count, 0u);
    }

    // A deliberately-leaked error is detected AND drained: the guard reports it
    // once, and an immediate re-check comes back clean (proving the drain).
    TEST(GLErrorStateCheckTest, DetectsAndDrainsLeakedError)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Start clean so the assertions below see only our injected error.
        u32 preCount = 0;
        (void)GLErrorState::DrainAndGetFirstError(preCount);

        // GL_TRIANGLES (0x0004) is not a valid glEnable capability →
        // GL_INVALID_ENUM. This is exactly the class of "unrelated earlier GL
        // op left an error" the listener exists to catch.
        ::glEnable(GL_TRIANGLES);

        u32 count = 0;
        const u32 err = GLErrorState::DrainAndGetFirstError(count);
        EXPECT_EQ(err, static_cast<u32>(GL_INVALID_ENUM))
            << "the guard must surface the leaked error's enum";
        EXPECT_GE(count, 1u);

        // The queue must now be empty — the guard drains, it does not merely peek.
        u32 afterCount = 0;
        const u32 afterErr = GLErrorState::DrainAndGetFirstError(afterCount);
        EXPECT_EQ(afterErr, static_cast<u32>(GL_NO_ERROR))
            << "DrainAndGetFirstError must clear the queue so the next test starts clean";
        EXPECT_EQ(afterCount, 0u);
    }
} // namespace OloEngine::Tests
