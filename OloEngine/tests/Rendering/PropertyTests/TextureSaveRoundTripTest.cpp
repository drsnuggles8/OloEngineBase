// =============================================================================
// TextureSaveRoundTripTest.cpp
//
// Layer-3 (Data Round-Trip) tests for GPUResourceInspector::SaveTextureToFile.
// The save path was a long-standing TODO behind the "Save to File" button in
// the GPU inspector panel — these tests pin the new implementation so future
// changes to format handling, mip selection, or cubemap face indexing surface
// as failing round-trips rather than silently writing the wrong pixels.
//
// Y-flip convention
// -----------------
// SaveTextureToFile writes pixels in raw GPU memory order (GL bottom-left
// origin) with no software flip — matching RenderDoc / Nsight semantics. The
// tests upload byte buffers directly via glTextureSubImage* and compare the
// saved file against the unmodified source, so any future code path that
// silently introduces a flip will fail this round-trip.
//
// Coverage:
//   1. RGBA8 Texture2D → PNG → reload → byte-exact identity
//   2. RGB8 Texture2D  → PNG → reload → byte-exact identity (covers the
//      3-channel pack-alignment path that defaults to 4 in GL)
//   3. RGBA32F Texture2D → HDR → reload → within Radiance HDR tolerance
//   4. RGBA8 Cubemap face → PNG → reload → byte-exact identity
//      (uses face index 3 = -Y so a wrong-face regression is detectable —
//      using 0 would mask off-by-one bugs in the layer offset.)
//   5. Invalid inputs (zero ID / out-of-range mip / out-of-range face / packed
//      depth-stencil format) → SaveTextureToFile returns false without writing.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <stb_image/stb_image.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // RAII helpers — keep destructors reachable when ASSERT_* short-circuits.
        struct ScopedTexture
        {
            GLuint m_Id = 0;
            ScopedTexture() = default;
            explicit ScopedTexture(GLuint id) : m_Id(id) {}
            ~ScopedTexture()
            {
                if (m_Id != 0)
                    ::glDeleteTextures(1, &m_Id);
            }
            ScopedTexture(const ScopedTexture&) = delete;
            ScopedTexture& operator=(const ScopedTexture&) = delete;
            operator GLuint() const
            {
                return m_Id;
            }
        };

        struct ScopedFile
        {
            std::filesystem::path m_Path;
            ScopedFile() = default;
            explicit ScopedFile(std::filesystem::path p) : m_Path(std::move(p)) {}
            ~ScopedFile()
            {
                if (!m_Path.empty())
                {
                    std::error_code ec;
                    std::filesystem::remove(m_Path, ec);
                }
            }
            ScopedFile(const ScopedFile&) = delete;
            ScopedFile& operator=(const ScopedFile&) = delete;
        };

        // Build a deterministic test image — varies all three channels and uses
        // values across the 0..255 range so a wrong-channel-order bug shows up.
        std::vector<u8> MakeRgba8Pattern(u32 w, u32 h)
        {
            std::vector<u8> out(static_cast<sizet>(w) * h * 4);
            for (u32 y = 0; y < h; ++y)
            {
                for (u32 x = 0; x < w; ++x)
                {
                    sizet i = (static_cast<sizet>(y) * w + x) * 4;
                    out[i + 0] = static_cast<u8>((x * 17u + 11u) & 0xFFu);
                    out[i + 1] = static_cast<u8>((y * 23u + 7u) & 0xFFu);
                    out[i + 2] = static_cast<u8>(((x + y) * 13u + 3u) & 0xFFu);
                    out[i + 3] = static_cast<u8>((x * y + 5u) & 0xFFu);
                }
            }
            return out;
        }

        std::vector<u8> MakeRgb8Pattern(u32 w, u32 h)
        {
            std::vector<u8> out(static_cast<sizet>(w) * h * 3);
            for (u32 y = 0; y < h; ++y)
            {
                for (u32 x = 0; x < w; ++x)
                {
                    sizet i = (static_cast<sizet>(y) * w + x) * 3;
                    out[i + 0] = static_cast<u8>((x * 17u + 11u) & 0xFFu);
                    out[i + 1] = static_cast<u8>((y * 23u + 7u) & 0xFFu);
                    out[i + 2] = static_cast<u8>(((x + y) * 13u + 3u) & 0xFFu);
                }
            }
            return out;
        }

        // Float pattern in [0, 16) — exercises HDR range, but small enough that
        // the RGBE quantisation in Radiance HDR keeps per-channel relative
        // error under 1%.
        std::vector<f32> MakeRgbaFPattern(u32 w, u32 h)
        {
            std::vector<f32> out(static_cast<sizet>(w) * h * 4);
            for (u32 y = 0; y < h; ++y)
            {
                for (u32 x = 0; x < w; ++x)
                {
                    sizet i = (static_cast<sizet>(y) * w + x) * 4;
                    out[i + 0] = 0.10f + 0.30f * static_cast<f32>(x);
                    out[i + 1] = 0.20f + 0.40f * static_cast<f32>(y);
                    out[i + 2] = 1.50f + 0.10f * static_cast<f32>(x + y);
                    out[i + 3] = 1.0f; // HDR alpha not preserved by stbi
                }
            }
            return out;
        }

        // Upload an RGB8 texture with explicit pack/unpack alignment of 1 so
        // odd-width rows don't pick up padding via shared GL state.
        GLuint UploadRgb8Texture(u32 w, u32 h, const u8* pixels)
        {
            GLuint id = 0;
            ::glCreateTextures(GL_TEXTURE_2D, 1, &id);
            ::glTextureStorage2D(id, 1, GL_RGB8, static_cast<GLsizei>(w), static_cast<GLsizei>(h));

            GLint prevUnpack = 4;
            ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
            ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            ::glTextureSubImage2D(id, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                                  GL_RGB, GL_UNSIGNED_BYTE, pixels);
            ::glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
            return id;
        }

        // Upload a cubemap with each face filled by a known pattern so we can
        // detect which face was saved.
        GLuint UploadRgba8Cubemap(u32 size, const std::vector<std::vector<u8>>& facePixels)
        {
            GLuint id = 0;
            ::glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &id);
            ::glTextureStorage2D(id, 1, GL_RGBA8, static_cast<GLsizei>(size), static_cast<GLsizei>(size));

            GLint prevUnpack = 4;
            ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
            ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            // For DSA cubemap upload, the `zOffset` parameter selects the face
            // (0..5 = +X,-X,+Y,-Y,+Z,-Z) — same layout SaveTextureToFile uses.
            for (u32 f = 0; f < 6; ++f)
            {
                ::glTextureSubImage3D(id, 0,
                                      0, 0, static_cast<GLint>(f),
                                      static_cast<GLsizei>(size), static_cast<GLsizei>(size), 1,
                                      GL_RGBA, GL_UNSIGNED_BYTE, facePixels[f].data());
            }
            ::glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
            return id;
        }

        // Synthesise a TextureInfo without going through the public Register
        // entry points (which would query the live GL state). All fields needed
        // by SaveTextureToFile are populated explicitly.
        GPUResourceInspector::TextureInfo MakeTexture2DInfo(GLuint id, u32 w, u32 h, GLenum internalFormat,
                                                            GLenum format, GLenum dataType)
        {
            GPUResourceInspector::TextureInfo info{};
            info.m_RendererID = id;
            info.m_Type = GPUResourceInspector::ResourceType::Texture2D;
            info.m_Width = w;
            info.m_Height = h;
            info.m_MipLevels = 1;
            info.m_InternalFormat = internalFormat;
            info.m_Format = format;
            info.m_DataType = dataType;
            return info;
        }

        std::filesystem::path MakeTempPath(const std::string& filename)
        {
            std::error_code ec;
            auto base = std::filesystem::temp_directory_path(ec);
            if (ec)
                base = std::filesystem::current_path();
            return base / ("olo_gpu_inspector_test_" + filename);
        }
    } // namespace

    TEST(TextureSaveRoundTripTest, Rgba8Texture2DToPngIsByteIdentical)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 W = 16;
        constexpr u32 H = 8;
        std::vector<u8> src = MakeRgba8Pattern(W, H);

        GLuint id = static_cast<GLuint>(CreateRgba8Texture2D(W, H, src.data()));
        ScopedTexture texOwner{ id };

        auto info = MakeTexture2DInfo(id, W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        auto path = MakeTempPath("rgba8.png");
        ScopedFile fileOwner{ path };

        ASSERT_TRUE(GPUResourceInspector::GetInstance().SaveTextureToFile(info, path.string(), 0));
        ASSERT_TRUE(std::filesystem::exists(path));

        int rw = 0, rh = 0, rc = 0;
        // Defensively reset both the global and thread-local stbi flip flags
        // before reloading — production paths (OpenGLTexture.cpp, AssetSerializer.cpp)
        // set the thread-local flag and don't always restore it, which would
        // otherwise return the saved file's pixels vertically flipped relative
        // to our src buffer and fail the comparison.
        ::stbi_set_flip_vertically_on_load(0);
        ::stbi_set_flip_vertically_on_load_thread(0);
        u8* loaded = ::stbi_load(path.string().c_str(), &rw, &rh, &rc, 4);
        ASSERT_NE(loaded, nullptr) << "Failed to reload PNG: " << ::stbi_failure_reason();
        // ASSERT on dimensions: the memcmp below would OOB-read if the reloaded
        // image is smaller than expected.
        ASSERT_EQ(rw, static_cast<int>(W));
        ASSERT_EQ(rh, static_cast<int>(H));

        const sizet expected = static_cast<sizet>(W) * H * 4;
        EXPECT_EQ(0, std::memcmp(loaded, src.data(), expected));
        ::stbi_image_free(loaded);
    }

    TEST(TextureSaveRoundTripTest, Rgb8Texture2DToPngHandlesOddWidthAlignment)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Odd width — RGB8 rows are 3*W bytes; default GL_PACK_ALIGNMENT=4 would
        // round up and leak padding into the saved file. SaveTextureToFile sets
        // PACK_ALIGNMENT to 1 explicitly; this test pins that.
        constexpr u32 W = 7;
        constexpr u32 H = 5;
        std::vector<u8> src = MakeRgb8Pattern(W, H);

        GLuint id = UploadRgb8Texture(W, H, src.data());
        ScopedTexture texOwner{ id };

        auto info = MakeTexture2DInfo(id, W, H, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE);
        auto path = MakeTempPath("rgb8_odd.png");
        ScopedFile fileOwner{ path };

        ASSERT_TRUE(GPUResourceInspector::GetInstance().SaveTextureToFile(info, path.string(), 0));

        int rw = 0, rh = 0, rc = 0;
        // Reset stbi flip flags before reload — production paths (OpenGLTexture.cpp,
        // AssetSerializer.cpp) leave the thread-local flag set, which would return
        // the pixels vertically flipped relative to src and fail the comparison.
        ::stbi_set_flip_vertically_on_load(0);
        ::stbi_set_flip_vertically_on_load_thread(0);
        u8* loaded = ::stbi_load(path.string().c_str(), &rw, &rh, &rc, 3);
        ASSERT_NE(loaded, nullptr) << "Failed to reload PNG: " << ::stbi_failure_reason();
        ASSERT_EQ(rw, static_cast<int>(W));
        ASSERT_EQ(rh, static_cast<int>(H));

        const sizet expected = static_cast<sizet>(W) * H * 3;
        EXPECT_EQ(0, std::memcmp(loaded, src.data(), expected));
        ::stbi_image_free(loaded);
    }

    TEST(TextureSaveRoundTripTest, FloatTextureSavesAsPngWithoutGLInvalidOperation)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Regression test: previously SaveTextureToFile called glGetTextureSubImage
        // with format=GL_RGBA, type=GL_UNSIGNED_BYTE against an RGBA32F texture.
        // That combination is rejected with GL_INVALID_OPERATION on most drivers
        // because the spec disallows the implicit float→ubyte conversion. The
        // fix reads in native float precision and converts to u8 in software,
        // clamping to [0,1].
        constexpr u32 W = 4;
        constexpr u32 H = 4;
        // Mix in-range, out-of-range, and negative values so the clamp + quantise
        // path is exercised end-to-end.
        std::vector<f32> src{
            0.00f,
            0.00f,
            0.00f,
            1.0f,
            1.00f,
            1.00f,
            1.00f,
            1.0f,
            0.50f,
            0.25f,
            0.75f,
            1.0f,
            2.00f,
            -0.5f,
            0.50f,
            1.0f,
            0.10f,
            0.20f,
            0.30f,
            1.0f,
            0.40f,
            0.50f,
            0.60f,
            1.0f,
            0.70f,
            0.80f,
            0.90f,
            1.0f,
            1.00f,
            0.00f,
            0.50f,
            1.0f,
            0.05f,
            0.95f,
            0.05f,
            1.0f,
            0.95f,
            0.05f,
            0.95f,
            1.0f,
            0.33f,
            0.66f,
            0.99f,
            1.0f,
            0.11f,
            0.22f,
            0.44f,
            1.0f,
            0.50f,
            0.50f,
            0.50f,
            1.0f,
            0.25f,
            0.75f,
            0.50f,
            1.0f,
            0.99f,
            0.01f,
            0.49f,
            1.0f,
            0.51f,
            0.99f,
            0.01f,
            1.0f,
        };

        GLuint id = static_cast<GLuint>(CreateFloatTexture2D(W, H, src.data()));
        ScopedTexture texOwner{ id };

        auto info = MakeTexture2DInfo(id, W, H, GL_RGBA32F, GL_RGBA, GL_FLOAT);
        auto path = MakeTempPath("rgba32f_to_png.png");
        ScopedFile fileOwner{ path };

        ASSERT_TRUE(GPUResourceInspector::GetInstance().SaveTextureToFile(info, path.string(), 0))
            << "Float-to-PNG save should succeed (previously triggered GL 0x502).";

        ::stbi_set_flip_vertically_on_load(0);
        ::stbi_set_flip_vertically_on_load_thread(0);
        int rw = 0, rh = 0, rc = 0;
        u8* loaded = ::stbi_load(path.string().c_str(), &rw, &rh, &rc, 4);
        ASSERT_NE(loaded, nullptr) << ::stbi_failure_reason();
        ASSERT_EQ(rw, static_cast<int>(W));
        ASSERT_EQ(rh, static_cast<int>(H));

        // Verify the clamp+quantise: each pixel matches round(clamp(src,0,1)*255).
        for (u32 i = 0; i < static_cast<u32>(W) * H * 4; ++i)
        {
            f32 clamped = std::clamp(src[i], 0.0f, 1.0f);
            u8 expected = static_cast<u8>(clamped * 255.0f + 0.5f);
            EXPECT_EQ(loaded[i], expected) << "channel " << i;
        }
        ::stbi_image_free(loaded);
    }

    TEST(TextureSaveRoundTripTest, FloatTextureWithNaNAndInfSavesAsZeroOrSaturated)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Regression: previously the float→PNG conversion ran
        // static_cast<u8>(std::clamp(NaN, 0, 1) * 255 + 0.5), which is
        // undefined behavior per [conv.fpint] because clamp returns NaN.
        // Now NaN / -Inf substitute as 0 (black); +Inf saturates to 255.
        constexpr u32 W = 4;
        constexpr u32 H = 1;
        const f32 qnan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();
        const std::vector<f32> src{
            qnan,
            0.0f,
            0.5f,
            1.0f, // pixel 0: NaN R channel
            inf,
            -inf,
            qnan,
            0.0f, // pixel 1: +Inf R, -Inf G, NaN B
            0.25f,
            0.5f,
            0.75f,
            1.0f, // pixel 2: well-formed
            -1.0f,
            2.0f,
            -2.0f,
            0.5f, // pixel 3: out-of-range (existing clamp path)
        };

        GLuint id = static_cast<GLuint>(CreateFloatTexture2D(W, H, src.data()));
        ScopedTexture texOwner{ id };

        auto info = MakeTexture2DInfo(id, W, H, GL_RGBA32F, GL_RGBA, GL_FLOAT);
        auto path = MakeTempPath("rgba32f_nan_inf.png");
        ScopedFile fileOwner{ path };

        ASSERT_TRUE(GPUResourceInspector::GetInstance().SaveTextureToFile(info, path.string(), 0))
            << "Save must succeed even when source texture contains NaN/Inf.";

        ::stbi_set_flip_vertically_on_load(0);
        ::stbi_set_flip_vertically_on_load_thread(0);
        int rw = 0, rh = 0, rc = 0;
        u8* loaded = ::stbi_load(path.string().c_str(), &rw, &rh, &rc, 4);
        ASSERT_NE(loaded, nullptr) << ::stbi_failure_reason();
        ASSERT_EQ(rw, static_cast<int>(W));
        ASSERT_EQ(rh, static_cast<int>(H));

        // Pixel 0: NaN R → 0, then well-formed channels
        EXPECT_EQ(loaded[0], 0u);
        EXPECT_EQ(loaded[1], 0u);
        EXPECT_EQ(loaded[2], 128u);
        EXPECT_EQ(loaded[3], 255u);
        // Pixel 1: +Inf R → 255, -Inf G → 0, NaN B → 0
        EXPECT_EQ(loaded[4], 255u);
        EXPECT_EQ(loaded[5], 0u);
        EXPECT_EQ(loaded[6], 0u);
        EXPECT_EQ(loaded[7], 0u);
        // Pixel 2: well-formed → round(x * 255)
        EXPECT_EQ(loaded[8], 64u);
        EXPECT_EQ(loaded[9], 128u);
        EXPECT_EQ(loaded[10], 191u);
        EXPECT_EQ(loaded[11], 255u);
        // Pixel 3: out-of-range clamps to {0, 255, 0, 128}
        EXPECT_EQ(loaded[12], 0u);
        EXPECT_EQ(loaded[13], 255u);
        EXPECT_EQ(loaded[14], 0u);
        EXPECT_EQ(loaded[15], 128u);
        ::stbi_image_free(loaded);
    }

    TEST(TextureSaveRoundTripTest, R32FTextureSavesAsPngClampedToGrayscale)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Regression for the exact case reported from the editor: a single-
        // channel R32F texture (e.g. a shadow map / depth visualisation) saved
        // as PNG. Previously hit GL_INVALID_OPERATION at glGetTextureSubImage
        // because GL_RED + GL_UNSIGNED_BYTE is invalid against an R32F source.
        constexpr u32 W = 4;
        constexpr u32 H = 2;
        const std::vector<f32> src{ 0.0f, 0.25f, 0.5f, 1.0f,
                                    -0.1f, 0.75f, 1.5f, 0.125f };

        GLuint id = 0;
        ::glCreateTextures(GL_TEXTURE_2D, 1, &id);
        ::glTextureStorage2D(id, 1, GL_R32F, static_cast<GLsizei>(W), static_cast<GLsizei>(H));
        // Save/restore GL_UNPACK_ALIGNMENT so we don't pollute subsequent
        // tests in the same process with our tight-packed setting.
        GLint prevUnpack = 4;
        ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
        ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        ::glTextureSubImage2D(id, 0, 0, 0, static_cast<GLsizei>(W), static_cast<GLsizei>(H),
                              GL_RED, GL_FLOAT, src.data());
        ::glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
        ScopedTexture texOwner{ id };

        auto info = MakeTexture2DInfo(id, W, H, GL_R32F, GL_RED, GL_FLOAT);
        auto path = MakeTempPath("r32f_to_png.png");
        ScopedFile fileOwner{ path };

        ASSERT_TRUE(GPUResourceInspector::GetInstance().SaveTextureToFile(info, path.string(), 0));

        ::stbi_set_flip_vertically_on_load(0);
        ::stbi_set_flip_vertically_on_load_thread(0);
        int rw = 0, rh = 0, rc = 0;
        // Force-load as 1 channel; saved PNG is grayscale.
        u8* loaded = ::stbi_load(path.string().c_str(), &rw, &rh, &rc, 1);
        ASSERT_NE(loaded, nullptr) << ::stbi_failure_reason();
        ASSERT_EQ(rw, static_cast<int>(W));
        ASSERT_EQ(rh, static_cast<int>(H));

        for (u32 i = 0; i < src.size(); ++i)
        {
            f32 clamped = std::clamp(src[i], 0.0f, 1.0f);
            u8 expected = static_cast<u8>(clamped * 255.0f + 0.5f);
            EXPECT_EQ(loaded[i], expected) << "texel " << i;
        }
        ::stbi_image_free(loaded);
    }

    TEST(TextureSaveRoundTripTest, Rgba32FTexture2DToHdrRoundTripsWithinRGBETolerance)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 W = 4;
        constexpr u32 H = 3;
        std::vector<f32> src = MakeRgbaFPattern(W, H);

        GLuint id = static_cast<GLuint>(CreateFloatTexture2D(W, H, src.data()));
        ScopedTexture texOwner{ id };

        auto info = MakeTexture2DInfo(id, W, H, GL_RGBA32F, GL_RGBA, GL_FLOAT);
        auto path = MakeTempPath("rgba32f.hdr");
        ScopedFile fileOwner{ path };

        ASSERT_TRUE(GPUResourceInspector::GetInstance().SaveTextureToFile(info, path.string(), 0));

        int rw = 0, rh = 0, rc = 0;
        // Force-load as 3 channels: Radiance HDR doesn't carry an alpha channel.
        // Reset stbi flip flags for the same reason as the PNG tests above.
        ::stbi_set_flip_vertically_on_load(0);
        ::stbi_set_flip_vertically_on_load_thread(0);
        f32* loaded = ::stbi_loadf(path.string().c_str(), &rw, &rh, &rc, 3);
        ASSERT_NE(loaded, nullptr) << "Failed to reload HDR: " << ::stbi_failure_reason();
        ASSERT_EQ(rw, static_cast<int>(W));
        ASSERT_EQ(rh, static_cast<int>(H));

        // RGBE shares one 8-bit exponent across RGB → max relative error per
        // channel is ~1/256 of the shared mantissa peak. 1% tolerance is safe.
        for (u32 i = 0; i < static_cast<u32>(W) * H; ++i)
        {
            for (u32 c = 0; c < 3; ++c)
            {
                f32 expected = src[i * 4 + c];
                f32 got = loaded[i * 3 + c];
                f32 absErr = std::fabs(got - expected);
                f32 tol = std::max(0.01f, std::fabs(expected) * 0.01f);
                EXPECT_LE(absErr, tol)
                    << "pixel " << i << " channel " << c
                    << " expected=" << expected << " got=" << got;
            }
        }
        ::stbi_image_free(loaded);
    }

    TEST(TextureSaveRoundTripTest, CubemapFaceSelectsCorrectLayer)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 SIZE = 8;
        // Each face gets a unique, recognisable pattern. We tag a single pixel
        // per face with the face index in the red channel so picking the wrong
        // face is unmissable when reading back.
        std::vector<std::vector<u8>> faces(6);
        for (u32 f = 0; f < 6; ++f)
        {
            auto& buf = faces[f];
            buf.resize(SIZE * SIZE * 4);
            for (u32 i = 0; i < SIZE * SIZE; ++i)
            {
                buf[i * 4 + 0] = static_cast<u8>(f * 40u + 10u); // face tag
                buf[i * 4 + 1] = static_cast<u8>(i & 0xFFu);
                buf[i * 4 + 2] = static_cast<u8>((i * 3u) & 0xFFu);
                buf[i * 4 + 3] = 255u;
            }
        }

        GLuint id = UploadRgba8Cubemap(SIZE, faces);
        ScopedTexture texOwner{ id };

        // Pick face 3 = -Y deliberately — face 0 would let off-by-one bugs in
        // the zOffset slip through.
        constexpr u32 FACE = 3;
        GPUResourceInspector::TextureInfo info{};
        info.m_RendererID = id;
        info.m_Type = GPUResourceInspector::ResourceType::TextureCubemap;
        info.m_Width = SIZE;
        info.m_Height = SIZE;
        info.m_MipLevels = 1;
        info.m_InternalFormat = GL_RGBA8;
        info.m_Format = GL_RGBA;
        info.m_DataType = GL_UNSIGNED_BYTE;

        auto path = MakeTempPath("cube_face3.png");
        ScopedFile fileOwner{ path };

        ASSERT_TRUE(GPUResourceInspector::GetInstance().SaveTextureToFile(info, path.string(), 0, FACE));

        int rw = 0, rh = 0, rc = 0;
        // Defensively reset both the global and thread-local stbi flip flags
        // before reloading — production paths (OpenGLTexture.cpp, AssetSerializer.cpp)
        // set the thread-local flag and don't always restore it, which would
        // otherwise return the saved file's pixels vertically flipped relative
        // to our src buffer and fail the comparison.
        ::stbi_set_flip_vertically_on_load(0);
        ::stbi_set_flip_vertically_on_load_thread(0);
        u8* loaded = ::stbi_load(path.string().c_str(), &rw, &rh, &rc, 4);
        ASSERT_NE(loaded, nullptr) << "Failed to reload cubemap PNG: " << ::stbi_failure_reason();
        ASSERT_EQ(rw, static_cast<int>(SIZE));
        ASSERT_EQ(rh, static_cast<int>(SIZE));

        const sizet bytes = static_cast<sizet>(SIZE) * SIZE * 4;
        EXPECT_EQ(0, std::memcmp(loaded, faces[FACE].data(), bytes))
            << "Saved cubemap pixels did not match the -Y face — check the zOffset path.";
        ::stbi_image_free(loaded);
    }

    TEST(TextureSaveRoundTripTest, InvalidInputsAreRejected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& inspector = GPUResourceInspector::GetInstance();

        // Zero ID
        {
            auto info = MakeTexture2DInfo(0, 4, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            auto path = MakeTempPath("invalid_zero_id.png");
            ScopedFile fileOwner{ path };
            EXPECT_FALSE(inspector.SaveTextureToFile(info, path.string(), 0));
            EXPECT_FALSE(std::filesystem::exists(path));
        }

        // Out-of-range mip level on a single-mip texture
        {
            std::vector<u8> src = MakeRgba8Pattern(4, 4);
            GLuint id = static_cast<GLuint>(CreateRgba8Texture2D(4, 4, src.data()));
            ScopedTexture texOwner{ id };
            auto info = MakeTexture2DInfo(id, 4, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            info.m_MipLevels = 1;
            auto path = MakeTempPath("invalid_mip.png");
            ScopedFile fileOwner{ path };
            EXPECT_FALSE(inspector.SaveTextureToFile(info, path.string(), 5));
            EXPECT_FALSE(std::filesystem::exists(path));
        }

        // Cubemap face out of range
        {
            std::vector<std::vector<u8>> faces(6);
            for (auto& f : faces)
                f.assign(4 * 4 * 4, 0u);
            GLuint id = UploadRgba8Cubemap(4, faces);
            ScopedTexture texOwner{ id };
            GPUResourceInspector::TextureInfo info{};
            info.m_RendererID = id;
            info.m_Type = GPUResourceInspector::ResourceType::TextureCubemap;
            info.m_Width = 4;
            info.m_Height = 4;
            info.m_MipLevels = 1;
            info.m_InternalFormat = GL_RGBA8;
            info.m_Format = GL_RGBA;
            info.m_DataType = GL_UNSIGNED_BYTE;
            auto path = MakeTempPath("invalid_face.png");
            ScopedFile fileOwner{ path };
            EXPECT_FALSE(inspector.SaveTextureToFile(info, path.string(), 0, 6));
            EXPECT_FALSE(std::filesystem::exists(path));
        }

        // Packed depth-stencil format → ChannelsFromGLFormat returns 0
        {
            std::vector<u8> src = MakeRgba8Pattern(4, 4);
            GLuint id = static_cast<GLuint>(CreateRgba8Texture2D(4, 4, src.data()));
            ScopedTexture texOwner{ id };
            auto info = MakeTexture2DInfo(id, 4, 4, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8);
            auto path = MakeTempPath("invalid_packed.png");
            ScopedFile fileOwner{ path };
            EXPECT_FALSE(inspector.SaveTextureToFile(info, path.string(), 0));
            EXPECT_FALSE(std::filesystem::exists(path));
        }

        // Unsupported extension — .jpg/.bmp/.tga used to silently get PNG
        // bytes written under the wrong name. Now we reject them up front.
        for (const char* badExt : { ".jpg", ".bmp", ".tga", "" })
        {
            std::vector<u8> src = MakeRgba8Pattern(4, 4);
            GLuint id = static_cast<GLuint>(CreateRgba8Texture2D(4, 4, src.data()));
            ScopedTexture texOwner{ id };
            auto info = MakeTexture2DInfo(id, 4, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            auto path = MakeTempPath(std::string("invalid_ext") + badExt);
            ScopedFile fileOwner{ path };
            EXPECT_FALSE(inspector.SaveTextureToFile(info, path.string(), 0))
                << "Extension '" << badExt << "' should be rejected";
            EXPECT_FALSE(std::filesystem::exists(path));
        }
    }
} // namespace OloEngine::Tests
