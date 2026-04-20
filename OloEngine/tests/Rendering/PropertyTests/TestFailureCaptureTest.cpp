// =============================================================================
// TestFailureCaptureTest.cpp
//
// Positive tests for the auto-capture infrastructure. The GoogleTest listener
// itself is smoke-tested by the whole suite — whenever anything else fails,
// the capture directory appears. These tests drive the underlying primitives
// directly so a regression surfaces without waiting for a real failure.
// =============================================================================
#include "TestFailureCapture.h"
#include "RenderPropertyTest.h"

#include <gtest/gtest.h>
#include <glad/gl.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using OloEngine::Tests::TestFailureCapture::CaptureAll;
using OloEngine::Tests::TestFailureCapture::CaptureRoot;
using OloEngine::Tests::TestFailureCapture::DirectoryFor;
using OloEngine::Tests::TestFailureCapture::WriteCurrentDrawFboPng;
using OloEngine::Tests::TestFailureCapture::WriteGLStateSnapshot;
using OloEngine::Tests::TestFailureCapture::WriteLatestFrameSummary;
using OloEngine::Tests::TestFailureCapture::WriteMetadata;

namespace OloEngine::Tests
{
    namespace
    {
        // Anonymous namespace already gives internal linkage — no need
        // for the redundant `static` specifier.
        std::string ReadFile(const fs::path& p)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return {};
            std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return s;
        }

        static fs::path ScratchDir(const std::string& name)
        {
            // Route under the capture root so all dumps live in one place.
            fs::path dir = DirectoryFor("TestFailureCaptureSelfTest", name);
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
            return dir;
        }
    } // namespace

    TEST(TestFailureCaptureTest, DirectoryForSanitizesNames)
    {
        const fs::path dir = DirectoryFor("Suite/With Spaces", "Test.Name/With:Colons");
        const std::string asStr = dir.string();
        // Every character that's not [A-Za-z0-9_.-] must have been rewritten.
        for (char c : asStr)
        {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
                                 c == '\\' || c == '/' || c == ':';
            EXPECT_TRUE(allowed) << "Illegal char in sanitized path: " << c;
        }
        // Must still live underneath the capture root.
        EXPECT_NE(asStr.find(CaptureRoot().string()), std::string::npos);
    }

    TEST(TestFailureCaptureTest, MetadataIsWrittenWithoutGlContext)
    {
        const fs::path dir = ScratchDir("MetadataNoGl");
        const fs::path file = dir / "metadata.txt";
        ASSERT_TRUE(WriteMetadata(file, "SelfTest", "Metadata", "dummy assertion"));
        ASSERT_TRUE(fs::exists(file));
        const std::string text = ReadFile(file);
        EXPECT_NE(text.find("Suite      = SelfTest"), std::string::npos);
        EXPECT_NE(text.find("Test       = Metadata"), std::string::npos);
        EXPECT_NE(text.find("dummy assertion"), std::string::npos);
        EXPECT_NE(text.find("Timestamp  ="), std::string::npos);
    }

    TEST(TestFailureCaptureTest, LatestFrameSummaryIsNoOpWithoutCaptures)
    {
        const fs::path dir = ScratchDir("FrameSummary");
        const fs::path file = dir / "command_bucket.txt";
        // FrameCaptureManager is idle by default in tests — no captures,
        // so the writer must no-op cleanly and leave nothing on disk.
        EXPECT_FALSE(WriteLatestFrameSummary(file));
        EXPECT_FALSE(fs::exists(file));
    }

    TEST(TestFailureCaptureTest, GLStateSnapshotIsWrittenWhenGpuAvailable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const fs::path dir = ScratchDir("GlState");
        const fs::path file = dir / "gl_state.txt";
        ASSERT_TRUE(WriteGLStateSnapshot(file));
        const std::string text = ReadFile(file);
        // Spot-check: the snapshot must emit every section the dumper produces.
        EXPECT_NE(text.find("[toggles]"), std::string::npos);
        EXPECT_NE(text.find("[viewport]"), std::string::npos);
        EXPECT_NE(text.find("[bindings]"), std::string::npos);
        EXPECT_NE(text.find("[texture_units]"), std::string::npos);
        EXPECT_NE(text.find("[uniform_buffers]"), std::string::npos);
        EXPECT_NE(text.find("Texture2D[15]"), std::string::npos);
        EXPECT_NE(text.find("UBO[15]"), std::string::npos);
    }

    TEST(TestFailureCaptureTest, FboPngIsWrittenWhenGpuAvailable)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const fs::path dir = ScratchDir("FboPng");
        const fs::path file = dir / "framebuffer.png";
        // Set a known viewport so the PNG has predictable dimensions.
        ::glViewport(0, 0, 16, 16);
        ASSERT_TRUE(WriteCurrentDrawFboPng(file));
        ASSERT_TRUE(fs::exists(file));
        // PNG signature: 89 50 4E 47 0D 0A 1A 0A
        std::ifstream in(file, std::ios::binary);
        unsigned char hdr[8] = { 0 };
        in.read(reinterpret_cast<char*>(hdr), 8);
        EXPECT_EQ(hdr[0], 0x89);
        EXPECT_EQ(hdr[1], 'P');
        EXPECT_EQ(hdr[2], 'N');
        EXPECT_EQ(hdr[3], 'G');
    }

    TEST(TestFailureCaptureTest, CaptureAllProducesExpectedArtefacts)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ::glViewport(0, 0, 32, 32);
        const fs::path dir = CaptureAll("TestFailureCaptureSelfTest", "CaptureAllArtefacts",
                                        "synthetic assertion message");
        ASSERT_TRUE(fs::exists(dir));
        EXPECT_TRUE(fs::exists(dir / "metadata.txt"));
        EXPECT_TRUE(fs::exists(dir / "gl_state.txt"));
        EXPECT_TRUE(fs::exists(dir / "framebuffer.png"));
        // command_bucket.txt is only written when FrameCaptureManager has a
        // recorded frame; the default test harness doesn't record, so this
        // must be *absent* rather than empty.
        EXPECT_FALSE(fs::exists(dir / "command_bucket.txt"));

        const std::string meta = ReadFile(dir / "metadata.txt");
        EXPECT_NE(meta.find("synthetic assertion message"), std::string::npos);
        EXPECT_NE(meta.find("HasContext = yes"), std::string::npos);
    }
} // namespace OloEngine::Tests
