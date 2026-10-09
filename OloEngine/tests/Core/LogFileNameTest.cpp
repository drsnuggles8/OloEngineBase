// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"

#include "OloEngine/Core/Log.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// The log's file name is read back after it was opened: olo_asset_pack_build
// re-reads the warnings its build logged from Log::GetLogFileName(). The name
// used to be relative, so once anything changed the working directory (the
// renderer tests move it to OloEditor/) a reader opened a different file and
// found none of the lines the sink had written. In the full suite that made
// McpAssetPackBuild.RealBuildAutoSavesPollsAndReturnsWarningsWithBothCompressionSettings
// report no warnings, and pass when run alone. An absolute name cannot be
// redirected by a later cwd change; this test does not change the cwd itself,
// because the process is shared with whatever earlier tests left running.

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    TEST(LogFileName, IsAbsoluteAndNamesTheFileTheSinkWrites)
    {
        Log::Initialize();
        const fs::path logFile = Log::Get().GetLogFileName();
        ASSERT_TRUE(logFile.is_absolute()) << logFile.string();

        const std::string marker = "LogFileNameTest marker 7f3a91";
        OLO_CORE_WARN("{}", marker);

        std::ifstream in(logFile, std::ios::binary);
        ASSERT_TRUE(in) << "cannot reopen " << logFile.string();
        std::stringstream text;
        text << in.rdbuf();
        EXPECT_NE(text.str().find(marker), std::string::npos)
            << "the line just logged is not in the file GetLogFileName() names";
    }
} // namespace OloEngine::Tests
