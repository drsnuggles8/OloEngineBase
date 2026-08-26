#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"
#include "Rendering/Texture2DArrayRoundTrip.h"

#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Texture.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

// OLO_TEST_LAYER: L3

namespace OloEngine::Tests
{
    TEST(Texture2DArrayRoundTrip, KnownHalfEncodedLayersSurviveOnOpenGL)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_EQ(RendererAPI::GetAPI(), RendererAPI::API::OpenGL);
        EXPECT_TRUE(KnownHalfEncodedTextureArrayLayersRoundTrip());
    }

    TEST(Texture2DArrayRoundTrip, CpuHalfPackingMatchesTheLegacyOpenGLConversionWithinOneUlp)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_EQ(RendererAPI::GetAPI(), RendererAPI::API::OpenGL);

        const f32 halfStepAtOne = std::ldexp(1.0f, -10);
        const f32 midpoint0 = 1.0f + halfStepAtOne * 0.5f;
        const f32 midpoint1 = 1.0f + halfStepAtOne * 1.5f;
        const std::array<f32, 26> values{
            0.0f,
            -0.0f,
            std::ldexp(1.0f, -24),    // minimum half subnormal
            std::ldexp(1023.0f, -24), // maximum half subnormal
            std::ldexp(1.0f, -14),    // minimum half normal
            -std::ldexp(1.0f, -24),
            -std::ldexp(1023.0f, -24),
            -std::ldexp(1.0f, -14),
            0.5f,
            1.0f,
            2.0f,
            65504.0f,  // maximum finite half
            midpoint0, // exact half-way cases
            std::nextafter(midpoint0, 0.0f),
            std::nextafter(midpoint0, 2.0f),
            midpoint1,
            std::nextafter(midpoint1, 0.0f),
            std::nextafter(midpoint1, 2.0f),
            -midpoint0,
            -midpoint1,
            0.0031f, // representative lightmap values
            0.137f,
            0.33333334f,
            0.73f,
            1.7f,
            150.125f,
        };

        std::vector<f32> upload(values.size() * 4);
        for (sizet texel = 0; texel < values.size(); ++texel)
        {
            std::fill_n(upload.begin() + static_cast<std::ptrdiff_t>(texel * 4), 4, values[texel]);
        }

        TextureSpecification specification;
        specification.Width = static_cast<u32>(values.size());
        specification.Height = 1;
        specification.Format = ImageFormat::RGBA16F;
        specification.GenerateMips = false;
        auto texture = Texture2D::Create(specification);
        ASSERT_TRUE(texture);
        texture->SetData(upload.data(), static_cast<u32>(upload.size() * sizeof(f32)));

        std::vector<u16> driverWords(upload.size());
        ASSERT_TRUE(RenderCommand::ReadTextureSubImage(texture->GetRHIHandle(), 0, 0, 0, 0,
                                                       specification.Width, 1, 1, RHI::Format::RGBA16Float,
                                                       driverWords.size() * sizeof(u16), driverWords.data()));

        const auto orderedHalf = [](u16 bits) -> i32
        {
            const auto magnitude = static_cast<i32>(bits & 0x7FFFu);
            return (bits & 0x8000u) != 0 ? 0x8000 - magnitude : 0x8000 + magnitude;
        };

        u32 differingWords = 0;
        i32 maximumUlp = 0;
        for (sizet word = 0; word < upload.size(); ++word)
        {
            const u16 cpuWord = glm::packHalf1x16(upload[word]);
            const u16 driverWord = driverWords[word];
            if (cpuWord != driverWord)
            {
                ++differingWords;
            }
            const i32 ulp = std::abs(orderedHalf(cpuWord) - orderedHalf(driverWord));
            maximumUlp = std::max(maximumUlp, ulp);
            EXPECT_LE(ulp, 1) << "value " << upload[word] << ": CPU half 0x" << std::hex << cpuWord
                              << ", OpenGL half 0x" << driverWord << std::dec;
        }

        ::testing::Test::RecordProperty("compared_half_words", static_cast<int>(upload.size()));
        ::testing::Test::RecordProperty("differing_half_words", static_cast<int>(differingWords));
        ::testing::Test::RecordProperty("maximum_half_ulp", maximumUlp);
        if (const auto* vendor = reinterpret_cast<const char*>(::glGetString(GL_VENDOR)))
            ::testing::Test::RecordProperty("opengl_vendor", vendor);
        if (const auto* renderer = reinterpret_cast<const char*>(::glGetString(GL_RENDERER)))
            ::testing::Test::RecordProperty("opengl_renderer", renderer);
        if (const auto* version = reinterpret_cast<const char*>(::glGetString(GL_VERSION)))
            ::testing::Test::RecordProperty("opengl_version", version);
    }
} // namespace OloEngine::Tests
