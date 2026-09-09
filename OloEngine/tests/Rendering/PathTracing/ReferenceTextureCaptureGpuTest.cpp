// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ReferenceTextureCaptureGpuTest.cpp
//
// The one part of the #869 bake path that needs a real graphics device:
// turning a live `Texture2D` / `TextureCubemap` into the reference tracer's own
// CPU images (`ReferenceTextureCapture.h`).
//
// WHY IT NEEDS ITS OWN TEST. Everything else about the richer bake world is
// covered headlessly by injecting synthetic images — which is exactly what
// `ReferenceSceneBuildOptions::MaterialMapProvider` exists to allow, and what
// keeps `ReferenceScene` and `ReferenceSceneBuilder` GL-free. The consequence
// is that the readback itself is the one link no headless test exercises: a
// captor that returned nothing, or decoded the wrong transfer function, would
// leave the whole suite green while every bake silently went back to
// factor-only. That is precisely the class of silent fidelity loss ADR 0022 §5
// says must be countable rather than quiet, so it gets a device test.
//
// Each case uploads KNOWN pixels and asserts what comes back, so a failure
// names the defect: a wrong sRGB decision moves a value in a known direction, a
// transposed face lands on the wrong colour, a dead readback returns null.
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/PathTracing/ReferenceTextureCapture.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <glm/glm.hpp>

#include <cmath>
#include <memory>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;

    namespace
    {
        // A 2x2 RGBA8 texture of one colour, sRGB or not per the caller.
        [[nodiscard]] Ref<Texture2D> MakeUploadedTexture(const glm::u8vec4& colour, bool srgb)
        {
            TextureSpecification spec;
            spec.Width = 2;
            spec.Height = 2;
            spec.Format = ImageFormat::RGBA8;
            spec.SRGB = srgb;
            spec.GenerateMips = false;
            Ref<Texture2D> texture = Texture2D::Create(spec);
            if (!texture)
                return nullptr;
            std::vector<u8> pixels;
            pixels.reserve(16);
            for (u32 i = 0; i < 4; ++i)
            {
                pixels.push_back(colour.r);
                pixels.push_back(colour.g);
                pixels.push_back(colour.b);
                pixels.push_back(colour.a);
            }
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
            return texture;
        }

        [[nodiscard]] f32 DecodeSrgb(u8 value)
        {
            const f32 c = static_cast<f32>(value) / 255.0f;
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
    } // namespace

    TEST(ReferenceTextureCaptureGpu, ALiveTextureRoundTripsIntoTheReferencesOwnImage)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ReferenceTextureCaptor captor;

        // A LINEAR texture: the bytes are the values, no transfer function.
        Ref<Texture2D> linear = MakeUploadedTexture({ 64, 128, 192, 255 }, /*srgb=*/false);
        ASSERT_TRUE(linear);
        const std::shared_ptr<const ReferenceTexture> linearImage = captor.Capture(linear);
        ASSERT_NE(linearImage, nullptr) << "the readback produced nothing — every bake would be factor-only";
        EXPECT_EQ(linearImage->Width, 2u);
        EXPECT_EQ(linearImage->Height, 2u);
        const glm::vec4 linearTexel = linearImage->SampleBilinear(glm::vec2(0.5f, 0.5f));
        EXPECT_NEAR(linearTexel.r, 64.0f / 255.0f, 1e-4f);
        EXPECT_NEAR(linearTexel.g, 128.0f / 255.0f, 1e-4f);
        EXPECT_NEAR(linearTexel.b, 192.0f / 255.0f, 1e-4f);

        // The SAME bytes tagged sRGB must come back DECODED. A readback hands
        // back the stored bytes undecoded — the hardware only applies the EOTF
        // when sampling — so this is the captor's own job, and getting it
        // backwards would darken or brighten every textured bounce by a factor
        // that still looks like a plausible image.
        Ref<Texture2D> srgb = MakeUploadedTexture({ 64, 128, 192, 255 }, /*srgb=*/true);
        ASSERT_TRUE(srgb);
        const std::shared_ptr<const ReferenceTexture> srgbImage = captor.Capture(srgb);
        ASSERT_NE(srgbImage, nullptr);
        const glm::vec4 srgbTexel = srgbImage->SampleBilinear(glm::vec2(0.5f, 0.5f));
        EXPECT_NEAR(srgbTexel.r, DecodeSrgb(64), 1e-4f);
        EXPECT_NEAR(srgbTexel.g, DecodeSrgb(128), 1e-4f);
        EXPECT_NEAR(srgbTexel.b, DecodeSrgb(192), 1e-4f);
        // And the two are genuinely different, so neither assertion above can
        // be passing for the wrong reason.
        EXPECT_LT(srgbTexel.g, linearTexel.g * 0.9f);

        EXPECT_EQ(captor.GetStats().Captured, 2u);
        EXPECT_EQ(captor.GetStats().Failed, 0u);
    }

    TEST(ReferenceTextureCaptureGpu, AnRgb8TextureWhoseRowIsNotFourByteAlignedStillReadsBack)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // GL's default PACK alignment is 4 and the readback allocates a
        // TIGHTLY packed buffer, so a 3-wide RGB8 row (9 bytes) would need 12
        // bytes per row if that alignment applied — and glGetTextureImage
        // would reject the buffer it was handed, silently costing every such
        // albedo its texture. 3x2 is the smallest case that asks the question.
        // The engine sets GL_UNPACK_ALIGNMENT on the upload side but never
        // GL_PACK_ALIGNMENT, so whether this passes is a fact about the
        // driver's defaults rather than something the code arranges.
        TextureSpecification spec;
        spec.Width = 3;
        spec.Height = 2;
        spec.Format = ImageFormat::RGB8;
        spec.SRGB = false;
        spec.GenerateMips = false;
        Ref<Texture2D> texture = Texture2D::Create(spec);
        ASSERT_TRUE(texture);

        std::vector<u8> pixels;
        for (u32 i = 0; i < 6; ++i)
        {
            pixels.push_back(static_cast<u8>(10 * i));
            pixels.push_back(static_cast<u8>(20 * i));
            pixels.push_back(static_cast<u8>(30 * i));
        }
        texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));

        ReferenceTextureCaptor captor;
        const std::shared_ptr<const ReferenceTexture> image = captor.Capture(texture);
        ASSERT_NE(image, nullptr) << "an RGB8 readback with a 9-byte row failed — GL_PACK_ALIGNMENT bites";
        EXPECT_EQ(image->Width, 3u);
        EXPECT_EQ(image->Height, 2u);

        // Texel (2, 1) is index 5 -> (50, 100, 150). Sampled at its centre.
        const glm::vec4 last = image->SampleBilinear(glm::vec2(5.0f / 6.0f, 3.0f / 4.0f));
        EXPECT_NEAR(last.r, 50.0f / 255.0f, 1e-4f);
        EXPECT_NEAR(last.g, 100.0f / 255.0f, 1e-4f);
        EXPECT_NEAR(last.b, 150.0f / 255.0f, 1e-4f);
    }

    TEST(ReferenceTextureCaptureGpu, OneImageSharedByManyMaterialsIsReadBackOnce)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // A readback is not free and a scene routinely shares one albedo across
        // many materials. The captor caches by texture pointer; if that ever
        // stops working the bake still produces the right answer, just far
        // slower, which is the kind of regression nothing else would catch.
        ReferenceTextureCaptor captor;
        Ref<Texture2D> shared = MakeUploadedTexture({ 200, 40, 40, 255 }, /*srgb=*/true);
        ASSERT_TRUE(shared);

        const std::shared_ptr<const ReferenceTexture> first = captor.Capture(shared);
        const std::shared_ptr<const ReferenceTexture> second = captor.Capture(shared);
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(first.get(), second.get()) << "the same texture decoded twice";
        EXPECT_EQ(captor.GetStats().Captured, 1u);

        // A null slot is not a failure — it is a material that simply has no
        // map there, which is the common case and must not inflate the tally
        // the bake logs.
        EXPECT_EQ(captor.Capture(Ref<Texture2D>()), nullptr);
        EXPECT_EQ(captor.GetStats().Failed, 0u);
    }

    TEST(ReferenceTextureCaptureGpu, ALiveSkyCubemapRoundTripsWithItsFacesInTheRightPlaces)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Six distinguishable faces, uploaded through the engine's own cubemap,
        // read back, and probed along the six axes. This is the end-to-end
        // version of ReferenceEnvironmentTest's face-table assertions: that one
        // pins Sample() against the GL table, this one pins that a REAL
        // cubemap's faces arrive in the order Sample() expects. A backend that
        // returned faces in another order would place the sky's ground on the
        // ceiling, and the bake would look merely "a bit off".
        constexpr u32 kFaceSize = 4;
        CubemapSpecification spec;
        spec.Width = kFaceSize;
        spec.Height = kFaceSize;
        spec.Format = ImageFormat::RGBA32F;
        spec.GenerateMips = false;
        Ref<TextureCubemap> cubemap = TextureCubemap::Create(spec);
        ASSERT_TRUE(cubemap);

        const glm::vec3 faceColours[6] = {
            { 4.0f, 0.0f, 0.0f },
            { 0.5f, 0.0f, 0.0f },
            { 0.0f, 4.0f, 0.0f },
            { 0.0f, 0.5f, 0.0f },
            { 0.0f, 0.0f, 4.0f },
            { 0.0f, 0.0f, 0.5f },
        };
        for (u32 face = 0; face < 6; ++face)
        {
            std::vector<f32> texels;
            texels.reserve(static_cast<sizet>(kFaceSize) * kFaceSize * 4u);
            for (u32 i = 0; i < kFaceSize * kFaceSize; ++i)
            {
                texels.push_back(faceColours[face].r);
                texels.push_back(faceColours[face].g);
                texels.push_back(faceColours[face].b);
                texels.push_back(1.0f);
            }
            cubemap->SetFaceData(face, texels.data(), static_cast<u32>(texels.size() * sizeof(f32)));
        }

        const std::shared_ptr<const ReferenceEnvironmentCubemap> captured = CaptureEnvironmentCubemap(cubemap);
        ASSERT_NE(captured, nullptr) << "the sky readback produced nothing — the bake would run with no sky";
        ASSERT_TRUE(captured->IsValid());
        EXPECT_EQ(captured->FaceSize, kFaceSize);

        const glm::vec3 axes[6] = {
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
        };
        for (u32 face = 0; face < 6; ++face)
        {
            const glm::vec3 sampled = captured->Sample(axes[face]);
            EXPECT_NEAR(sampled.r, faceColours[face].r, 1e-4f) << "face " << face;
            EXPECT_NEAR(sampled.g, faceColours[face].g, 1e-4f) << "face " << face;
            EXPECT_NEAR(sampled.b, faceColours[face].b, 1e-4f) << "face " << face;
        }

        // HDR survives the trip: a sky above 1.0 is the whole point of a float
        // environment, and an 8-bit path anywhere in the chain would clamp it.
        EXPECT_GT(captured->Sample(glm::vec3(0.0f, 1.0f, 0.0f)).g, 1.0f);
    }

    TEST(ReferenceTextureCaptureGpu, ANullSkyIsAbsentRatherThanBlack)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // "There is no sky" and "there is a sky and it is black" are different
        // worlds, and only one of them is true. Returning an all-zero cubemap
        // for a missing sky would trace the second while meaning the first.
        EXPECT_EQ(CaptureEnvironmentCubemap(Ref<TextureCubemap>()), nullptr);
    }
} // namespace OloEngine::Tests
