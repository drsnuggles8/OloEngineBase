// OLO_TEST_LAYER: unit

#include "OloEngine/Core/Application.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    TEST(MonoDebuggerPolicy, HostAndOptInMatrix)
    {
        char program[] = "OloHost";
        char flag[] = "--mono-debug";
        char* argv[] = { program, flag };
        for (bool isEditor : { false, true })
        {
            for (bool isHeadless : { false, true })
            {
                for (bool optIn : { false, true })
                {
                    SCOPED_TRACE(::testing::Message() << "editor=" << isEditor
                                                      << " headless=" << isHeadless << " optIn=" << optIn);
                    ApplicationSpecification spec;
                    spec.IsEditor = isEditor;
                    spec.IsHeadless = isHeadless;
                    spec.CommandLineArgs = { optIn ? 2 : 1, argv };
#if OLO_TEST_HOST_DEBUG
                    const bool expected = isEditor || optIn;
#else
                    const bool expected = optIn;
#endif
                    EXPECT_EQ(spec.ShouldEnableMonoDebugging(), expected);
                }
            }
        }
    }

    TEST(MonoDebuggerPolicy, DefaultHostDoesNotStartDebugger)
    {
        EXPECT_FALSE(ApplicationSpecification{}.ShouldEnableMonoDebugging());
    }

    TEST(MonoDebuggerPolicy, OptInRequiresAnExactArgumentAfterProgramName)
    {
        char program[] = "--mono-debug";
        char prefix[] = "--mono-debugger";
        char assignment[] = "--mono-debug=false";
        char embedded[] = "project--mono-debug";
        char* argv[] = { program, prefix, assignment, embedded };
        ApplicationSpecification spec;
        spec.CommandLineArgs = { 4, argv };
        EXPECT_FALSE(spec.ShouldEnableMonoDebugging());
    }
} // namespace OloEngine::Tests
