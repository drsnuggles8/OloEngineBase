// OLO_TEST_LAYER: unit

#include "TestProcessLaunch.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

TEST(TestProcessOutput, FullCapturePreservesStartupAfterVerboseLogging)
{
    const auto path = OloEngine::Tests::TempFile("verbose-startup.out");
    const std::string startup = "Mono debugging disabled\n";
    const std::string shutdown = "clean shutdown\n";
    const std::string text = startup + std::string(128 * 1024, 'x') + shutdown;
    {
        std::ofstream output(path, std::ios::binary);
        output << text;
    }

    EXPECT_EQ(OloEngine::Tests::ReadCapturedOutput(path, 0), text);
    const auto tail = OloEngine::Tests::ReadCapturedOutput(path);
    EXPECT_EQ(tail.find(startup), std::string::npos);
    EXPECT_TRUE(tail.ends_with(shutdown));
}
