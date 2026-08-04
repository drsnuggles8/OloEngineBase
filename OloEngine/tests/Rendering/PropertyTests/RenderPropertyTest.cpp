// =============================================================================
// RenderPropertyTest.cpp
//
// See RenderPropertyTest.h for design notes.
// =============================================================================
#include "OloEnginePCH.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#if defined(OLO_TESTS_HAVE_EGL)
// Keep X11's headers out of this translation unit. `<EGL/eglplatform.h>`
// pulls in `<X11/Xlib.h>` unless told otherwise, and Xlib defines `None` as
// a bare `0L` macro — which silently mangles every `None` enumerator the
// engine headers declare (MeshComponent::Primitive::None among them). The
// surfaceless path never touches X, so opt out of the headers entirely.
#define EGL_NO_X11 1
#define MESA_EGL_NO_X11_HEADERS 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Which windowing/context path to use for the shared GL context, via
        // the OLO_TEST_GL_BACKEND environment variable.
        //
        //   Auto (default) — prefer GLFW, the context a developer gets locally;
        //                    fall back to EGL when GLFW cannot reach a display
        //                    server, i.e. a headless Linux box that still has a
        //                    perfectly usable GPU.
        //   Glfw / Egl     — pin one path. Headless CI pins `egl` so that a
        //                    missing display server is a deterministic choice
        //                    rather than a silent backend switch: two backends
        //                    can produce subtly different pixels, and a golden
        //                    baseline must know which one produced it.
        enum class GlBackend
        {
            Auto,
            Glfw,
            Egl
        };

        GlBackend SelectBackend()
        {
            const char* raw = std::getenv("OLO_TEST_GL_BACKEND");
            if (raw == nullptr || raw[0] == '\0')
                return GlBackend::Auto;

            std::string value(raw);
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });

            if (value == "egl")
                return GlBackend::Egl;
            if (value == "glfw")
                return GlBackend::Glfw;

            // Unrecognised value: fall back to Auto rather than failing hard —
            // a typo in CI config should degrade to the normal behaviour, and
            // the GPU tests will report themselves skipped if nothing works.
            return GlBackend::Auto;
        }

        // ---------------------------------------------------------------
        // Singleton GPU-context. Created on first call to IsGpuAvailable.
        // No teardown: process exit reclaims GL state. This avoids order-
        // of-destruction issues with spdlog / static engine singletons.
        // ---------------------------------------------------------------
        struct GpuContext
        {
            std::once_flag m_InitOnce;
            bool m_Available = false;
            GLFWwindow* m_Window = nullptr;
#if defined(OLO_TESTS_HAVE_EGL)
            EGLDisplay m_EglDisplay = EGL_NO_DISPLAY;
            EGLContext m_EglContext = EGL_NO_CONTEXT;
            EGLSurface m_EglSurface = EGL_NO_SURFACE;
#endif

            static GpuContext& Get()
            {
                static GpuContext s_instance;
                return s_instance;
            }

            void TryInitOnce()
            {
                std::call_once(m_InitOnce,
                               [this]()
                               {
                                   if (!ChangeToOloEditorDir())
                                       return;

                                   const GlBackend backend = SelectBackend();

                                   if (backend != GlBackend::Egl && TryInitGlfw())
                                   {
                                       m_Available = true;
                                       return;
                                   }

#if defined(OLO_TESTS_HAVE_EGL)
                                   if (backend != GlBackend::Glfw && TryInitEgl())
                                   {
                                       m_Available = true;
                                       return;
                                   }
#endif
                               });
            }

            // Shared by both backends: glad resolves against whichever loader
            // owns the now-current context, and we require a full 4.6 core
            // profile either way.
            static bool LoadGladAndCheckVersion(GLADloadfunc loader)
            {
                const int version = ::gladLoadGL(loader);
                if (version == 0)
                    return false;

                const int major = GLAD_VERSION_MAJOR(version);
                const int minor = GLAD_VERSION_MINOR(version);
                return (major > 4) || (major == 4 && minor >= 6);
            }

            bool TryInitGlfw()
            {
                if (!::glfwInit())
                    return false;

                ::glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                ::glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
                ::glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
                ::glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
                ::glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

                m_Window = ::glfwCreateWindow(1, 1, "OloEngine-RenderPropertyTest", nullptr, nullptr);
                if (!m_Window)
                    return false;

                ::glfwMakeContextCurrent(m_Window);
                if (!LoadGladAndCheckVersion(reinterpret_cast<GLADloadfunc>(::glfwGetProcAddress)))
                {
                    ::glfwDestroyWindow(m_Window);
                    m_Window = nullptr;
                    return false;
                }

                return true;
            }

#if defined(OLO_TESTS_HAVE_EGL)
            bool TryInitEgl()
            {
                auto getPlatformDisplay =
                    reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(::eglGetProcAddress("eglGetPlatformDisplayEXT"));
                if (getPlatformDisplay == nullptr)
                    return false;

                // 1. Mesa's surfaceless platform: no X, no Wayland, no GBM device
                //    to pick. This is the path a headless CI box takes.
                if (TryInitEglOnDisplay(getPlatformDisplay(kPlatformSurfacelessMesa, EGL_DEFAULT_DISPLAY, nullptr)))
                    return true;

                // 2. Otherwise enumerate EGL devices and take the first backed by
                //    a DRM render node. Skipping the node-less entries is not
                //    cosmetic: Mesa also publishes an EGL_MESA_device_software
                //    device, and binding that would hand us llvmpipe while the
                //    run is labelled with a hardware vendor's golden set.
                auto queryDevices =
                    reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(::eglGetProcAddress("eglQueryDevicesEXT"));
                auto queryDeviceString =
                    reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(::eglGetProcAddress("eglQueryDeviceStringEXT"));
                if (queryDevices == nullptr || queryDeviceString == nullptr)
                    return false;

                constexpr EGLint kMaxDevices = 8;
                EGLDeviceEXT devices[kMaxDevices]{};
                EGLint deviceCount = 0;
                if (!queryDevices(kMaxDevices, devices, &deviceCount))
                    return false;

                for (EGLint i = 0; i < deviceCount; ++i)
                {
                    if (queryDeviceString(devices[i], kDrmRenderNodeFileExt) == nullptr)
                        continue;

                    if (TryInitEglOnDisplay(getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr)))
                        return true;
                }

                return false;
            }

            bool TryInitEglOnDisplay(EGLDisplay display)
            {
                if (display == EGL_NO_DISPLAY)
                    return false;

                if (!::eglInitialize(display, nullptr, nullptr))
                    return false;

                // Desktop GL, not GLES — the engine is a GL 4.6 DSA renderer.
                if (!::eglBindAPI(EGL_OPENGL_API))
                    return false;

                constexpr EGLint configAttribs[] = {
                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                    EGL_RED_SIZE, 8,
                    EGL_GREEN_SIZE, 8,
                    EGL_BLUE_SIZE, 8,
                    EGL_ALPHA_SIZE, 8,
                    EGL_NONE
                };
                EGLConfig config{};
                EGLint configCount = 0;
                if (!::eglChooseConfig(display, configAttribs, &config, 1, &configCount) || configCount < 1)
                    return false;

                constexpr EGLint contextAttribs[] = {
                    EGL_CONTEXT_MAJOR_VERSION, 4,
                    EGL_CONTEXT_MINOR_VERSION, 6,
                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                    EGL_NONE
                };
                EGLContext context = ::eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
                if (context == EGL_NO_CONTEXT)
                    return false;

                // Bind a small pbuffer rather than going surfaceless.
                //
                // A surfaceless context has NO default framebuffer, and the
                // renderer does touch framebuffer 0 — every RendererAttachedTest
                // render tick then failed with
                //
                //     GL_INVALID_FRAMEBUFFER_OPERATION
                //
                // which is how the whole visual/golden layer failed on the
                // headless runner while the non-rendering tests sailed past. The
                // pbuffer is 1x1 because nothing is ever presented to it; its
                // only job is to give framebuffer 0 something complete to be.
                constexpr EGLint pbufferAttribs[] = {
                    EGL_WIDTH, 1,
                    EGL_HEIGHT, 1,
                    EGL_NONE
                };
                EGLSurface surface = ::eglCreatePbufferSurface(display, config, pbufferAttribs);
                if (surface == EGL_NO_SURFACE)
                {
                    ::eglDestroyContext(display, context);
                    return false;
                }

                if (!::eglMakeCurrent(display, surface, surface, context))
                {
                    ::eglDestroySurface(display, surface);
                    ::eglDestroyContext(display, context);
                    return false;
                }
                m_EglSurface = surface;

                if (!LoadGladAndCheckVersion(reinterpret_cast<GLADloadfunc>(::eglGetProcAddress)))
                {
                    ::eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                    ::eglDestroySurface(display, surface);
                    ::eglDestroyContext(display, context);
                    return false;
                }

                m_EglDisplay = display;
                m_EglContext = context;
                return true;
            }

            // Spelled out rather than relying on the SDK headers being new enough
            // to declare them.
            static constexpr EGLenum kPlatformSurfacelessMesa = 0x31DD; // EGL_PLATFORM_SURFACELESS_MESA
            static constexpr EGLint kDrmRenderNodeFileExt = 0x3377;     // EGL_DRM_RENDER_NODE_FILE_EXT
#endif

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
                    if (auto shadersDir = editorDir / "assets" / "shaders"; std::filesystem::exists(shadersDir, ec) && std::filesystem::is_directory(shadersDir, ec))
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
        // trailing floats is malformed, and any non-multiple-of-4 length
        // would silently truncate during count = size / 4. Bail out with
        // default stats rather than read past the end or ignore tail data.
        if (pixels.size() < 4 || (pixels.size() % 4) != 0)
            return stats;

        stats.m_MinR = stats.m_MaxR = pixels[0];
        stats.m_MinG = stats.m_MaxG = pixels[1];
        stats.m_MinB = stats.m_MaxB = pixels[2];
        stats.m_MinA = stats.m_MaxA = pixels[3];
        bool hasFiniteR = std::isfinite(stats.m_MinR);
        bool hasFiniteG = std::isfinite(stats.m_MinG);
        bool hasFiniteB = std::isfinite(stats.m_MinB);
        bool hasFiniteA = std::isfinite(stats.m_MinA);

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

            if (std::isfinite(r))
            {
                if (!hasFiniteR)
                {
                    stats.m_MinR = r;
                    stats.m_MaxR = r;
                    hasFiniteR = true;
                }
                else
                {
                    stats.m_MinR = std::min(stats.m_MinR, r);
                    stats.m_MaxR = std::max(stats.m_MaxR, r);
                }
                stats.m_SumR += r;
            }
            if (std::isfinite(g))
            {
                if (!hasFiniteG)
                {
                    stats.m_MinG = g;
                    stats.m_MaxG = g;
                    hasFiniteG = true;
                }
                else
                {
                    stats.m_MinG = std::min(stats.m_MinG, g);
                    stats.m_MaxG = std::max(stats.m_MaxG, g);
                }
                stats.m_SumG += g;
            }
            if (std::isfinite(b))
            {
                if (!hasFiniteB)
                {
                    stats.m_MinB = b;
                    stats.m_MaxB = b;
                    hasFiniteB = true;
                }
                else
                {
                    stats.m_MinB = std::min(stats.m_MinB, b);
                    stats.m_MaxB = std::max(stats.m_MaxB, b);
                }
                stats.m_SumB += b;
            }
            if (std::isfinite(a))
            {
                if (!hasFiniteA)
                {
                    stats.m_MinA = a;
                    stats.m_MaxA = a;
                    hasFiniteA = true;
                }
                else
                {
                    stats.m_MinA = std::min(stats.m_MinA, a);
                    stats.m_MaxA = std::max(stats.m_MaxA, a);
                }
                stats.m_SumA += a;
            }
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

    void BindSlotBasedInput(const u32 slot, const u32 texture)
    {
        ::glBindTextureUnit(static_cast<GLuint>(slot), static_cast<GLuint>(texture));
    }

    ScopedSlotBasedShaders::ScopedSlotBasedShaders()
        : m_WasEnabled(RHI::DescriptorHeap::Get().IsEnabled())
    {
        if (m_WasEnabled)
        {
            RHI::DescriptorHeap::Get().SetEnabled(false);
        }
    }

    ScopedSlotBasedShaders::~ScopedSlotBasedShaders()
    {
        if (m_WasEnabled)
        {
            RHI::DescriptorHeap::Get().SetEnabled(true);
        }
    }

    void FullscreenPass::Draw(u32 inputTexture) const
    {
        // Plain bind — see BindSlotBasedInput / ScopedSlotBasedShaders for why these
        // harnesses deliberately stay on the slot-based path.
        ::glBindTextureUnit(0, static_cast<GLuint>(inputTexture));
        ::glBindVertexArray(m_Vao);
        ::glDrawArrays(GL_TRIANGLES, 0, 6);
    }
} // namespace OloEngine::Tests
