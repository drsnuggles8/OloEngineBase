#include "OloEnginePCH.h"
#include "GLErrorStateCheck.h"

#include <gtest/gtest.h>
#include <glad/gl.h>

#include <string>

namespace OloEngine::Tests::GLErrorState
{
    namespace
    {
        // True only when a GL context is actually current. glad's function
        // pointers are populated by gladLoadGL; if the table is null no test in
        // this process ever created a context. Even with the table populated,
        // glGetString(GL_VERSION) returns null when no context is current — the
        // same guard TestFailureCapture uses. On success glGetString sets no
        // error, so probing here can't fabricate one.
        bool HasGlContext()
        {
            if (glad_glGetString == nullptr)
                return false;
            return ::glGetString(GL_VERSION) != nullptr;
        }
    } // namespace

    const char* GlErrorString(u32 err)
    {
        switch (err)
        {
            case GL_NO_ERROR:
                return "GL_NO_ERROR";
            case GL_INVALID_ENUM:
                return "GL_INVALID_ENUM";
            case GL_INVALID_VALUE:
                return "GL_INVALID_VALUE";
            case GL_INVALID_OPERATION:
                return "GL_INVALID_OPERATION";
            case GL_INVALID_FRAMEBUFFER_OPERATION:
                return "GL_INVALID_FRAMEBUFFER_OPERATION";
            case GL_OUT_OF_MEMORY:
                return "GL_OUT_OF_MEMORY";
            case GL_STACK_UNDERFLOW:
                return "GL_STACK_UNDERFLOW";
            case GL_STACK_OVERFLOW:
                return "GL_STACK_OVERFLOW";
            case GL_CONTEXT_LOST:
                return "GL_CONTEXT_LOST";
            default:
                return "GL_<unknown error>";
        }
    }

    u32 DrainAndGetFirstError(u32& outCount)
    {
        outCount = 0;
        if (glad_glGetError == nullptr || !HasGlContext())
            return GL_NO_ERROR;

        // glGetError reports one flag per call and clears it; loop to drain the
        // whole queue. 64-iteration bound (mirrors Utils::DrainGLErrors) so a
        // lost context stuck on GL_CONTEXT_LOST can't spin forever.
        constexpr u32 kMaxDrainIterations = 64;
        u32 first = GL_NO_ERROR;
        for (u32 i = 0; i < kMaxDrainIterations; ++i)
        {
            const GLenum e = ::glGetError();
            if (e == GL_NO_ERROR)
                break;
            if (first == GL_NO_ERROR)
                first = e;
            ++outCount;
        }
        return first;
    }

    u32 DrainAndGetFirstError()
    {
        u32 ignoredCount = 0;
        return DrainAndGetFirstError(ignoredCount);
    }

    // =========================================================================
    // GoogleTest listener — asserts a clean GL error queue after every test.
    // =========================================================================
    namespace
    {
        class GLErrorStateListener : public ::testing::EmptyTestEventListener
        {
          public:
            void OnTestEnd(const ::testing::TestInfo& info) override
            {
                // Always drain first — even for a SKIPPED test. A test that did
                // GL work and then skipped mid-body would otherwise leak its
                // error into the next test (the very cross-test pollution this
                // guard exists to prevent). On headless CI (no context) the
                // drain is a no-op, so this can't fabricate a skip failure.
                u32 count = 0;
                const u32 err = DrainAndGetFirstError(count);
                if (err == GL_NO_ERROR)
                    return;

                // A skip is not a failure: we drained above so nothing leaks
                // onward, but we don't fail a test the harness chose to skip.
                const ::testing::TestResult* result = info.result();
                if (result != nullptr && result->Skipped())
                    return;

                // This test returned with a pending GL error, leaving the
                // shared process-wide context dirty. We already drained it above
                // so the next test starts clean; now fail THIS test so the
                // pollution is attributed to its source rather than misattributed
                // to a later unrelated GPU test (issue #485).
                //
                // OnTestEnd fires in reverse listener order, so this failure is
                // recorded before the result printer's OnTestEnd reads the
                // result — the test correctly prints [ FAILED ]. Attribute the
                // failure to the polluting test's own source location (the TEST
                // macro), not this listener file, so IDE/CI "jump to failure"
                // and XML location fields point at the culprit.
                std::string message = "GL error state not clean after test: left ";
                message += GlErrorString(err);
                if (count > 1)
                {
                    message += " (and " + std::to_string(count - 1) + " more pending error";
                    message += (count - 1 == 1) ? ")" : "s)";
                }
                message += ". This test polluted the shared GL context; the leaked "
                           "error was drained here so it cannot corrupt a later test "
                           "(see issue #485). Fix the source test to leave a clean "
                           "glGetError() state.";

                const char* const file = info.file() != nullptr ? info.file() : __FILE__;
                const int line = info.file() != nullptr ? info.line() : __LINE__;
                ADD_FAILURE_AT(file, line) << message;
            }
        };

        bool s_Registered = false;
    } // namespace

    void RegisterListener()
    {
        if (s_Registered)
            return;
        s_Registered = true;
        ::testing::UnitTest::GetInstance()->listeners().Append(new GLErrorStateListener());
    }
} // namespace OloEngine::Tests::GLErrorState
