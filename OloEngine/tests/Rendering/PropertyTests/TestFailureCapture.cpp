#include "OloEnginePCH.h"
#include "TestFailureCapture.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace OloEngine::Tests::TestFailureCapture
{
    namespace
    {
        // Sanitize a test identifier into a filesystem-safe token. Keeps ascii
        // alphanum / '_' / '-' / '.'; everything else becomes '_'.
        static std::string Sanitize(std::string_view in)
        {
            std::string out;
            out.reserve(in.size());
            for (char c : in)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
                out.push_back(ok ? c : '_');
            }
            if (out.empty())
                out = "unnamed";
            return out;
        }

        static std::string IsoTimestampUtc()
        {
            std::time_t now = std::time(nullptr);
            std::tm tmUtc{};
#if defined(_WIN32)
            ::gmtime_s(&tmUtc, &now);
#else
            ::gmtime_r(&now, &tmUtc);
#endif
            char buf[32] = { 0 };
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
            return std::string{ buf };
        }

        // Non-throwing query of a GL string. Returns "unavailable" if there is
        // no current context — guards against glad's function-pointer table
        // being uninitialised (gladLoadGL never ran in this process).
        static std::string SafeGlString(GLenum name)
        {
            if (glad_glGetString == nullptr)
                return "unavailable";
            const GLubyte* s = ::glGetString(name);
            if (s == nullptr)
                return "unavailable";
            // Clear the GL error flag if the call set one (no current context).
            if (glad_glGetError != nullptr)
            {
                while (::glGetError() != GL_NO_ERROR)
                {
                }
            }
            return std::string{ reinterpret_cast<const char*>(s) };
        }

        static bool HasGlContext()
        {
            if (glad_glGetString == nullptr)
                return false;
            return ::glGetString(GL_VERSION) != nullptr;
        }

        // Drain any outstanding GL error so our probes don't report stale ones.
        static void ClearGlErrors()
        {
            if (glad_glGetError == nullptr)
                return;
            while (::glGetError() != GL_NO_ERROR)
            {
            }
        }
    } // namespace

    fs::path CaptureRoot()
    {
        return fs::path{ "assets" } / "tests" / "captures";
    }

    fs::path DirectoryFor(std::string_view suite, std::string_view test)
    {
        return CaptureRoot() / (Sanitize(suite) + "__" + Sanitize(test));
    }

    bool WriteGLStateSnapshot(const fs::path& path)
    {
        if (!HasGlContext())
            return false;

        ClearGlErrors();
        const GLStateSnapshot snap = GLStateSnapshot::Capture();

        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        std::ofstream out(path, std::ios::trunc);
        if (!out)
            return false;

        out << "# OloEngine GLStateSnapshot\n";
        out << "# Written by TestFailureCapture on failure.\n\n";

        out << "[toggles]\n";
        out << "DepthTest      = " << snap.m_DepthTest << '\n';
        out << "DepthMask      = " << snap.m_DepthMask << '\n';
        out << "DepthFunc      = 0x" << std::hex << snap.m_DepthFunc << std::dec << '\n';
        out << "Blend          = " << snap.m_Blend << '\n';
        out << "BlendSrcRgb    = 0x" << std::hex << snap.m_BlendSrcRgb << std::dec << '\n';
        out << "BlendDstRgb    = 0x" << std::hex << snap.m_BlendDstRgb << std::dec << '\n';
        out << "BlendSrcAlpha  = 0x" << std::hex << snap.m_BlendSrcAlpha << std::dec << '\n';
        out << "BlendDstAlpha  = 0x" << std::hex << snap.m_BlendDstAlpha << std::dec << '\n';
        out << "BlendEqRgb     = 0x" << std::hex << snap.m_BlendEqRgb << std::dec << '\n';
        out << "BlendEqAlpha   = 0x" << std::hex << snap.m_BlendEqAlpha << std::dec << '\n';
        out << "StencilTest    = " << snap.m_StencilTest << '\n';
        out << "StencilFunc    = 0x" << std::hex << snap.m_StencilFunc << std::dec << '\n';
        out << "StencilRef     = " << snap.m_StencilRef << '\n';
        out << "StencilMask    = 0x" << std::hex << snap.m_StencilMask << std::dec << '\n';
        out << "CullFace       = " << snap.m_CullFace << '\n';
        out << "CullFaceMode   = 0x" << std::hex << snap.m_CullFaceMode << std::dec << '\n';
        out << "FrontFace      = 0x" << std::hex << snap.m_FrontFace << std::dec << '\n';
        out << "ScissorTest    = " << snap.m_ScissorTest << '\n';
        out << "PolygonMode    = 0x" << std::hex << snap.m_PolygonMode << std::dec << '\n';

        out << "\n[viewport]\n";
        out << "Viewport = [" << snap.m_Viewport[0] << "," << snap.m_Viewport[1] << ","
            << snap.m_Viewport[2] << "," << snap.m_Viewport[3] << "]\n";
        out << "Scissor  = [" << snap.m_Scissor[0] << "," << snap.m_Scissor[1] << ","
            << snap.m_Scissor[2] << "," << snap.m_Scissor[3] << "]\n";

        out << "\n[bindings]\n";
        out << "FboDraw       = " << snap.m_FboDraw << '\n';
        out << "FboRead       = " << snap.m_FboRead << '\n';
        out << "ActiveProgram = " << snap.m_ActiveProgram << '\n';
        out << "Vao           = " << snap.m_Vao << '\n';

        out << "\n[texture_units]\n";
        for (u32 i = 0; i < snap.m_Textures2D.size(); ++i)
        {
            out << "Texture2D[" << i << "] = " << snap.m_Textures2D[i] << '\n';
        }

        out << "\n[uniform_buffers]\n";
        for (u32 i = 0; i < snap.m_UniformBuffers.size(); ++i)
        {
            out << "UBO[" << i << "] = " << snap.m_UniformBuffers[i] << '\n';
        }

        return out.good();
    }

    bool WriteCurrentDrawFboPng(const fs::path& path)
    {
        if (!HasGlContext())
            return false;

        ClearGlErrors();

        GLint viewport[4] = { 0, 0, 0, 0 };
        ::glGetIntegerv(GL_VIEWPORT, viewport);
        const i32 x = viewport[0];
        const i32 y = viewport[1];
        const i32 w = viewport[2];
        const i32 h = viewport[3];
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
            return false;

        // Read GL_COLOR_ATTACHMENT0 (or GL_BACK for default FBO) as float,
        // then clamp + gamma encode. Doing the readback as float tolerates
        // HDR attachments (RGBA16F / RGBA32F) without banding or clamping
        // losing the interesting detail.
        std::vector<f32> pixelsF(static_cast<sizet>(w) * h * 4, 0.0f);
        ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
        ::glReadPixels(x, y, w, h, GL_RGBA, GL_FLOAT, pixelsF.data());
        const GLenum err = ::glGetError();
        if (err != GL_NO_ERROR)
        {
            // Clear for subsequent diagnostics but don't assert — the error
            // itself is useful info and will show up in metadata.
            ClearGlErrors();
            return false;
        }

        std::vector<u8> pixels8(pixelsF.size(), 0);
        // Simple tone map: clamp to [0,4], then Reinhard + gamma 2.2. This is
        // *only* for the PNG artefact; we want to show HDR hotspots without
        // pegging the whole image to 255.
        for (sizet i = 0; i < pixelsF.size(); i += 4)
        {
            for (sizet c = 0; c < 3; ++c)
            {
                f32 v = pixelsF[i + c];
                if (std::isnan(v) || std::isinf(v))
                    v = 0.0f;
                v = std::max(0.0f, v);
                v = v / (1.0f + v);                                 // Reinhard
                v = std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f); // gamma encode
                pixels8[i + c] = static_cast<u8>(std::lround(v * 255.0f));
            }
            pixels8[i + 3] = 255;
        }

        // stb_image_write writes top-to-bottom, but glReadPixels returns
        // bottom-to-top. Flip rows into scratch.
        std::vector<u8> flipped(pixels8.size(), 0);
        const sizet rowBytes = static_cast<sizet>(w) * 4;
        for (i32 row = 0; row < h; ++row)
        {
            const u8* src = pixels8.data() + static_cast<sizet>(h - 1 - row) * rowBytes;
            u8* dst = flipped.data() + static_cast<sizet>(row) * rowBytes;
            std::memcpy(dst, src, rowBytes);
        }

        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const int ok = ::stbi_write_png(path.string().c_str(), w, h, 4, flipped.data(), static_cast<int>(rowBytes));
        return ok != 0;
    }

    bool WriteLatestFrameSummary(const fs::path& path)
    {
        auto& mgr = OloEngine::FrameCaptureManager::GetInstance();
        const auto frames = mgr.GetCapturedFramesCopy();
        if (frames.empty())
            return false;

        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
            return false;

        const OloEngine::CapturedFrameData& last = frames.back();
        out << "# OloEngine FrameCaptureManager — last captured frame\n";
        out << "FrameNumber        = " << last.FrameNumber << '\n';
        out << "TimestampSeconds   = " << last.TimestampSeconds << '\n';
        out << "PreSortCommands    = " << last.PreSortCommands.size() << '\n';
        out << "PostSortCommands   = " << last.PostSortCommands.size() << '\n';
        out << "PostBatchCommands  = " << last.PostBatchCommands.size() << '\n';
        out << "Stats.TotalCommands  = " << last.Stats.TotalCommands << '\n';
        out << "Stats.BatchedCommands= " << last.Stats.BatchedCommands << '\n';
        out << "Stats.DrawCalls      = " << last.Stats.DrawCalls << '\n';
        out << "Stats.StateChanges   = " << last.Stats.StateChanges << '\n';
        out << "Stats.ShaderBinds    = " << last.Stats.ShaderBinds << '\n';
        out << "Stats.TextureBinds   = " << last.Stats.TextureBinds << '\n';
        out << "Stats.SortTimeMs     = " << last.Stats.SortTimeMs << '\n';
        out << "Stats.BatchTimeMs    = " << last.Stats.BatchTimeMs << '\n';
        out << "Stats.ExecuteTimeMs  = " << last.Stats.ExecuteTimeMs << '\n';
        out << "Stats.TotalFrameTime = " << last.Stats.TotalFrameTimeMs << '\n';
        if (!last.Notes.empty())
            out << "Notes = " << last.Notes << '\n';
        return out.good();
    }

    bool WriteMetadata(const fs::path& path, std::string_view suite, std::string_view test,
                       std::string_view assertionMessage)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
            return false;

        out << "# OloEngine TestFailureCapture metadata\n";
        out << "Suite      = " << suite << '\n';
        out << "Test       = " << test << '\n';
        out << "Timestamp  = " << IsoTimestampUtc() << '\n';
        out << "GL_Vendor  = " << SafeGlString(GL_VENDOR) << '\n';
        out << "GL_Renderer= " << SafeGlString(GL_RENDERER) << '\n';
        out << "GL_Version = " << SafeGlString(GL_VERSION) << '\n';
        out << "GLSL       = " << SafeGlString(GL_SHADING_LANGUAGE_VERSION) << '\n';
        out << "HasContext = " << (HasGlContext() ? "yes" : "no") << '\n';
        if (!assertionMessage.empty())
            out << "\n[assertion]\n" << assertionMessage << '\n';
        return out.good();
    }

    fs::path CaptureAll(std::string_view suite, std::string_view test, std::string_view assertion)
    {
        const fs::path dir = DirectoryFor(suite, test);
        std::error_code ec;
        fs::create_directories(dir, ec);

        // Keep going even if a sub-capture fails — each one is independently
        // useful and we never want a capture path to mask the real failure.
        (void)WriteMetadata(dir / "metadata.txt", suite, test, assertion);
        (void)WriteGLStateSnapshot(dir / "gl_state.txt");
        (void)WriteCurrentDrawFboPng(dir / "framebuffer.png");
        (void)WriteLatestFrameSummary(dir / "command_bucket.txt");
        return dir;
    }

    // =========================================================================
    // GoogleTest listener — triggers CaptureAll on first failure per test.
    // =========================================================================
    namespace
    {
        class FailureListener : public ::testing::EmptyTestEventListener
        {
          public:
            void OnTestStart(const ::testing::TestInfo& info) override
            {
                m_Captured = false;
                m_FirstMessage.clear();
                // Clear any stale capture directory from a previous run. Per
                // test rather than per binary so parallel invocations don't
                // race on the root directory.
                std::error_code ec;
                fs::remove_all(DirectoryFor(info.test_suite_name(), info.name()), ec);
            }

            void OnTestPartResult(const ::testing::TestPartResult& result) override
            {
                if (!result.failed() || m_Captured)
                    return;
                m_Captured = true;
                m_FirstMessage = result.summary();
                const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
                if (info == nullptr)
                    return;
                CaptureAll(info->test_suite_name(), info->name(), m_FirstMessage);
            }

          private:
            bool m_Captured = false;
            std::string m_FirstMessage;
        };

        static bool s_Registered = false;
    } // namespace

    void RegisterFailureListener()
    {
        if (s_Registered)
            return;
        s_Registered = true;
        auto& listeners = ::testing::UnitTest::GetInstance()->listeners();
        listeners.Append(new FailureListener());
    }
} // namespace OloEngine::Tests::TestFailureCapture
