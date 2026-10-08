// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"

#include "OloEngine/Core/Log.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// The log's file name is read back after it was opened: olo_asset_pack_build
// re-reads the warnings its build logged from Log::GetLogFileName(). The name
// used to be relative, so after anything changed the working directory (the
// renderer tests move it to OloEditor/) a reader opened a different file and
// found none of the lines the sink had written. In the full suite that made
// McpAssetPackBuild.RealBuildAutoSavesPollsAndReturnsWarningsWithBothCompressionSettings
// report no warnings, and pass when run alone.

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    TEST(LogFileName, StaysTheFileTheSinkWritesAfterTheWorkingDirectoryMoves)
    {
        Log::Initialize();
        const fs::path logFile = Log::Get().GetLogFileName();
        ASSERT_TRUE(logFile.is_absolute()) << logFile.string();

        const fs::path original = fs::current_path();
        struct RestoreCwd
        {
            fs::path Path;
            ~RestoreCwd()
            {
                std::error_code ec;
                fs::current_path(Path, ec);
            }
        } restore{ original };
        fs::current_path(TempDir());

        const std::string marker = "LogFileNameTest marker 7f3a91";
        OLO_CORE_WARN("{}", marker);

        std::ifstream in(Log::Get().GetLogFileName(), std::ios::binary);
        ASSERT_TRUE(in) << "cannot reopen " << Log::Get().GetLogFileName() << " from " << fs::current_path().string();
        std::stringstream text;
        text << in.rdbuf();
        EXPECT_NE(text.str().find(marker), std::string::npos)
            << "the line just logged is not in the file GetLogFileName() names once the cwd has moved";
    }
} // namespace OloEngine::Tests
