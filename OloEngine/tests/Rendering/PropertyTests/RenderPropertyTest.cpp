// =============================================================================
// RenderPropertyTest.cpp
//
// See RenderPropertyTest.h for design notes.
// =============================================================================
#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace OloEngine::Tests
{
    namespace
    {
        // ---------------------------------------------------------------
        // Singleton GPU-context. Created on first call to IsGpuAvailable.
        // No teardown: process exit reclaims GL state. This avoids order-
        // of-destruction issues with spdlog / static engine singletons.
        // ---------------------------------------------------------------
        struct GpuContext
        {
            bool m_Initialized = false;
            bool m_Available = false;
            GLFWwindow* m_Window = nullptr;

            static GpuContext& Get()
            {
                static GpuContext s_instance;
                return s_instance;
            }

            void TryInitOnce()
            {
                if (m_Initialized)
                    return;
                m_Initialized = true;

                if (!ChangeToOloEditorDir())
                    return;

                if (!::glfwInit())
                    return;

                ::glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                ::glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
                ::glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
                ::glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
                ::glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

                m_Window = ::glfwCreateWindow(1, 1, "OloEngine-RenderPropertyTest", nullptr, nullptr);
                if (!m_Window)
                {
                    ::glfwTerminate();
                    return;
                }

                ::glfwMakeContextCurrent(m_Window);
                const int version = ::gladLoadGL(reinterpret_cast<GLADloadfunc>(::glfwGetProcAddress));
                if (version == 0)
                {
                    ::glfwDestroyWindow(m_Window);
                    m_Window = nullptr;
                    ::glfwTerminate();
                    return;
                }

                const int major = GLAD_VERSION_MAJOR(version);
                const int minor = GLAD_VERSION_MINOR(version);
                if (major < 4 || (major == 4 && minor < 5))
                {
                    ::glfwDestroyWindow(m_Window);
                    m_Window = nullptr;
                    ::glfwTerminate();
                    return;
                }

                m_Available = true;
            }

            // Walk up from CWD looking for <candidate>/OloEditor/assets/shaders.
            // chdir into the OloEditor folder if found, so Shader::Create's
            // relative paths resolve correctly.
            static bool ChangeToOloEditorDir()
            {
                std::error_code ec;
                auto candidate = std::filesystem::current_path(ec);
                if (ec)
                    return false;

                for (int i = 0; i < 6; ++i)
                {
                    auto editorDir = candidate / "OloEditor";
                    auto shadersDir = editorDir / "assets" / "shaders";
                    if (std::filesystem::exists(shadersDir, ec) && std::filesystem::is_directory(shadersDir, ec))
                    {
                        std::filesystem::current_path(editorDir, ec);
                        return !ec;
                    }
                    if (!candidate.has_parent_path() || candidate == candidate.parent_path())
                        break;
                    candidate = candidate.parent_path();
                }
                return false;
            }
        };
    } // namespace

    bool RenderPropertyFixture::IsGpuAvailable()
    {
        auto& ctx = GpuContext::Get();
        ctx.TryInitOnce();
        return ctx.m_Available;
    }

    // -------------------------------------------------------------------------
    // GL pixel-store save/restore RAII
    //
    // `glTextureSubImage2D` / `glGetTextureImage` respect the context-global
    // GL_UNPACK_* / GL_PACK_* state. A prior test that left e.g. non-zero
    // GL_UNPACK_ROW_LENGTH or a non-default GL_PACK_ALIGNMENT will silently
    // corrupt tightly-packed uploads / readbacks. Wrap every helper that
    // performs a transfer with this guard: it forces the defaults we expect
    // (1-byte alignment, no row skipping, no sub-rectangle) and restores the
    // caller's state on scope exit. Mirrors TestFailureCapture.cpp's pattern.
    // -------------------------------------------------------------------------
    namespace
    {
        // Tag-dispatched constructor (instead of named factories returning
        // by value) so we can keep both move and copy deleted \u2014 NRVO of a
        // local is non-mandatory under C++17, so factories returning a local
        // by value would still need a move ctor on MSVC.
        struct UnpackTag
        {
        };
        struct PackTag
        {
        };

        struct PixelStoreDefaultsScope
        {
            bool m_Unpack = false;
            GLint m_UnpackAlignment = 4;
            GLint m_UnpackRowLength = 0;
            GLint m_UnpackImageHeight = 0;
            GLint m_UnpackSkipPixels = 0;
            GLint m_UnpackSkipRows = 0;
            GLint m_UnpackSkipImages = 0;
            GLint m_PackAlignment = 4;
            GLint m_PackRowLength = 0;
            GLint m_PackImageHeight = 0;
            GLint m_PackSkipPixels = 0;
            GLint m_PackSkipRows = 0;
            GLint m_PackSkipImages = 0;

            explicit PixelStoreDefaultsScope(UnpackTag)
                : m_Unpack(true)
            {
                ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &m_UnpackAlignment);
                ::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &m_UnpackRowLength);
                ::glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &m_UnpackImageHeight);
                ::glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &m_UnpackSkipPixels);
                ::glGetIntegerv(GL_UNPACK_SKIP_ROWS, &m_UnpackSkipRows);
                ::glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &m_UnpackSkipImages);
                ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                ::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                ::glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                ::glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                ::glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                ::glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
            }

            explicit PixelStoreDefaultsScope(PackTag)
                : m_Unpack(false)
            {
                ::glGetIntegerv(GL_PACK_ALIGNMENT, &m_PackAlignment);
                ::glGetIntegerv(GL_PACK_ROW_LENGTH, &m_PackRowLength);
                ::glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &m_PackImageHeight);
                ::glGetIntegerv(GL_PACK_SKIP_PIXELS, &m_PackSkipPixels);
                ::glGetIntegerv(GL_PACK_SKIP_ROWS, &m_PackSkipRows);
                ::glGetIntegerv(GL_PACK_SKIP_IMAGES, &m_PackSkipImages);
                ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
                ::glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                ::glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
                ::glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                ::glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                ::glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
            }

            ~PixelStoreDefaultsScope()
            {
                if (m_Unpack)
                {
                    ::glPixelStorei(GL_UNPACK_ALIGNMENT, m_UnpackAlignment);
                    ::glPixelStorei(GL_UNPACK_ROW_LENGTH, m_UnpackRowLength);
                    ::glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, m_UnpackImageHeight);
                    ::glPixelStorei(GL_UNPACK_SKIP_PIXELS, m_UnpackSkipPixels);
                    ::glPixelStorei(GL_UNPACK_SKIP_ROWS, m_UnpackSkipRows);
                    ::glPixelStorei(GL_UNPACK_SKIP_IMAGES, m_UnpackSkipImages);
                }
                else
                {
                    ::glPixelStorei(GL_PACK_ALIGNMENT, m_PackAlignment);
                    ::glPixelStorei(GL_PACK_ROW_LENGTH, m_PackRowLength);
                    ::glPixelStorei(GL_PACK_IMAGE_HEIGHT, m_PackImageHeight);
                    ::glPixelStorei(GL_PACK_SKIP_PIXELS, m_PackSkipPixels);
                    ::glPixelStorei(GL_PACK_SKIP_ROWS, m_PackSkipRows);
                    ::glPixelStorei(GL_PACK_SKIP_IMAGES, m_PackSkipImages);
                }
            }

            PixelStoreDefaultsScope(const PixelStoreDefaultsScope&) = delete;
            PixelStoreDefaultsScope& operator=(const PixelStoreDefaultsScope&) = delete;
            // Movability would let both the source and moved-to instances
            // run their destructor's glPixelStorei restores. Deleted \u2014
            // construct directly with the tag at the call site.
            PixelStoreDefaultsScope(PixelStoreDefaultsScope&&) = delete;
            PixelStoreDefaultsScope& operator=(PixelStoreDefaultsScope&&) = delete;
        };
    } // namespace

    // -------------------------------------------------------------------------
    // Procedural input helpers
    // -------------------------------------------------------------------------

    u32 CreateFloatTexture2D(u32 width, u32 height, const f32* pixels)
    {
        PixelStoreDefaultsScope unpackScope{ UnpackTag{} };
        GLuint tex = 0;
        ::glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        ::glTextureStorage2D(tex, 1, GL_RGBA32F, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        ::glTextureSubImage2D(tex, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                              GL_RGBA, GL_FLOAT, pixels);
        ::glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        ::glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ::glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return static_cast<u32>(tex);
    }

    u32 CreateUniformFloatTexture2D(u32 width, u32 height, f32 r, f32 g, f32 b, f32 a)
    {
        std::vector<f32> pixels(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < pixels.size(); i += 4)
        {
            pixels[i + 0] = r;
            pixels[i + 1] = g;
            pixels[i + 2] = b;
            pixels[i + 3] = a;
        }
        return CreateFloatTexture2D(width, height, pixels.data());
    }

    u32 CreateSinglePixelFloatTexture(f32 r, f32 g, f32 b, f32 a)
    {
        const f32 pixel[4] = { r, g, b, a };
        return CreateFloatTexture2D(1, 1, pixel);
    }

    u32 CreateRgba8Texture2D(u32 width, u32 height, const u8* pixels)
    {
        PixelStoreDefaultsScope unpackScope{ UnpackTag{} };
        GLuint tex = 0;
        ::glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        ::glTextureStorage2D(tex, 1, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        ::glTextureSubImage2D(tex, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                              GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        ::glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ::glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ::glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        ::glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return static_cast<u32>(tex);
    }

    // -------------------------------------------------------------------------
    // Readback helpers
    // -------------------------------------------------------------------------

    void ReadbackRgba8(u32 textureId, u32 width, u32 height, std::vector<u8>& out)
    {
        PixelStoreDefaultsScope packScope{ PackTag{} };
        out.resize(static_cast<std::size_t>(width) * height * 4);
        ::glGetTextureImage(static_cast<GLuint>(textureId), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                            static_cast<GLsizei>(out.size()), out.data());
    }

    void ReadbackRgbaFloat(u32 textureId, u32 width, u32 height, std::vector<f32>& out)
    {
        PixelStoreDefaultsScope packScope{ PackTag{} };
        out.resize(static_cast<std::size_t>(width) * height * 4);
        ::glGetTextureImage(static_cast<GLuint>(textureId), 0, GL_RGBA, GL_FLOAT,
                            static_cast<GLsizei>(out.size() * sizeof(f32)), out.data());
    }

    FloatStats ComputeStats(const std::vector<f32>& pixels)
    {
        FloatStats stats{};
        // Need a full RGBA quad to seed the min/max. A buffer with 1-3
        // trailing floats is malformed — bail out with default stats
        // rather than read past the end.
        if (pixels.size() < 4)
            return stats;

        stats.m_MinR = stats.m_MaxR = pixels[0];
        stats.m_MinG = stats.m_MaxG = pixels[1];
        stats.m_MinB = stats.m_MaxB = pixels[2];
        stats.m_MinA = stats.m_MaxA = pixels[3];

        const std::size_t count = pixels.size() / 4;
        stats.m_PixelCount = static_cast<u32>(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const f32 r = pixels[i * 4 + 0];
            const f32 g = pixels[i * 4 + 1];
            const f32 b = pixels[i * 4 + 2];
            const f32 a = pixels[i * 4 + 3];

            if (std::isnan(r) || std::isnan(g) || std::isnan(b) || std::isnan(a))
                ++stats.m_NanCount;
            if (std::isinf(r) || std::isinf(g) || std::isinf(b) || std::isinf(a))
                ++stats.m_InfCount;

            stats.m_MinR = std::min(stats.m_MinR, r);
            stats.m_MinG = std::min(stats.m_MinG, g);
            stats.m_MinB = std::min(stats.m_MinB, b);
            stats.m_MinA = std::min(stats.m_MinA, a);
            stats.m_MaxR = std::max(stats.m_MaxR, r);
            stats.m_MaxG = std::max(stats.m_MaxG, g);
            stats.m_MaxB = std::max(stats.m_MaxB, b);
            stats.m_MaxA = std::max(stats.m_MaxA, a);
            stats.m_SumR += r;
            stats.m_SumG += g;
            stats.m_SumB += b;
            stats.m_SumA += a;
        }
        return stats;
    }

    // -------------------------------------------------------------------------
    // FullscreenPass
    // -------------------------------------------------------------------------

    FullscreenPass::FullscreenPass()
    {
        // Two-triangle quad with position + UV matching the PostProcess shader
        // layout (location 0: vec3 pos, location 1: vec2 uv).
        struct Vertex
        {
            f32 x, y, z, u, v;
        };
        static constexpr Vertex verts[6] = {
            { -1.0f, -1.0f, 0.0f, 0.0f, 0.0f },
            { 1.0f, -1.0f, 0.0f, 1.0f, 0.0f },
            { 1.0f, 1.0f, 0.0f, 1.0f, 1.0f },
            { -1.0f, -1.0f, 0.0f, 0.0f, 0.0f },
            { 1.0f, 1.0f, 0.0f, 1.0f, 1.0f },
            { -1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
        };

        ::glCreateVertexArrays(1, &m_Vao);
        ::glCreateBuffers(1, &m_Vbo);
        ::glNamedBufferStorage(m_Vbo, sizeof(verts), verts, 0);

        ::glVertexArrayVertexBuffer(m_Vao, 0, m_Vbo, 0, sizeof(Vertex));
        ::glEnableVertexArrayAttrib(m_Vao, 0);
        ::glVertexArrayAttribFormat(m_Vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, x));
        ::glVertexArrayAttribBinding(m_Vao, 0, 0);
        ::glEnableVertexArrayAttrib(m_Vao, 1);
        ::glVertexArrayAttribFormat(m_Vao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, u));
        ::glVertexArrayAttribBinding(m_Vao, 1, 0);
    }

    FullscreenPass::~FullscreenPass()
    {
        if (m_Vbo)
            ::glDeleteBuffers(1, &m_Vbo);
        if (m_Vao)
            ::glDeleteVertexArrays(1, &m_Vao);
    }

    void FullscreenPass::Draw(u32 inputTexture) const
    {
        ::glBindTextureUnit(0, static_cast<GLuint>(inputTexture));
        ::glBindVertexArray(m_Vao);
        ::glDrawArrays(GL_TRIANGLES, 0, 6);
    }
} // namespace OloEngine::Tests
