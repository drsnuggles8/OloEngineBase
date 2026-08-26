#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Texture2DArray.h"

#include <gtest/gtest.h>

#include <array>
#include <format>
#include <vector>

namespace OloEngine::Tests
{
    // Backend-neutral RGBA16F Texture2DArray contract (#951). The caller owns
    // backend/device setup; everything below stays on the public renderer
    // facade so the OpenGL and Vulkan invocations exercise the same behavior.
    [[nodiscard]] inline ::testing::AssertionResult KnownHalfEncodedTextureArrayLayersRoundTrip()
    {
        constexpr u32 kWidth = 2;
        constexpr u32 kHeight = 2;
        constexpr u32 kLayers = 3;
        constexpr u32 kChannels = 4;
        constexpr auto kWordsPerLayer = static_cast<sizet>(kWidth) * kHeight * kChannels;

        // Literal IEEE-754 binary16 words, not floats packed by the code under
        // test. Every layer and channel differs so a wrong byte stride, channel
        // order, or array-layer selection produces an attributable mismatch.
        constexpr std::array<std::array<u16, kWordsPerLayer>, kLayers> kLayerWords{ {
            { 0x0000u, 0x3C00u, 0xC000u, 0x3800u,
              0x4000u, 0x4200u, 0x4400u, 0x4500u,
              0xBC00u, 0x3400u, 0x0400u, 0x0001u,
              0x7BFFu, 0x03FFu, 0x3555u, 0x3BFFu },
            { 0x3000u, 0xB000u, 0x3C01u, 0xBC01u,
              0x4800u, 0xC800u, 0x5000u, 0xD000u,
              0x2400u, 0xA400u, 0x2C00u, 0xAC00u,
              0x5400u, 0xD400u, 0x5800u, 0xD800u },
            { 0x3A00u, 0xBA00u, 0x3E00u, 0xBE00u,
              0x4100u, 0xC100u, 0x4300u, 0xC300u,
              0x4600u, 0xC600u, 0x4A00u, 0xCA00u,
              0x5C00u, 0xDC00u, 0x6000u, 0xE000u },
        } };

        Texture2DArraySpecification specification;
        specification.Width = kWidth;
        specification.Height = kHeight;
        specification.Layers = kLayers;
        specification.Format = Texture2DArrayFormat::RGBA16F;
        specification.GenerateMipmaps = false;

        auto texture = Texture2DArray::Create(specification);
        if (!texture)
        {
            return ::testing::AssertionFailure() << "Texture2DArray::Create returned null";
        }
        for (u32 layer = 0; layer < kLayers; ++layer)
        {
            texture->SetLayerData(layer, kLayerWords[layer].data(), kWidth, kHeight);
        }

        std::vector<u16> readback(kWordsPerLayer * kLayers, 0xDEADu);
        if (!RenderCommand::ReadTextureSubImage(texture->GetRHIHandle(), 0, 0, 0, 0,
                                                kWidth, kHeight, kLayers, RHI::Format::RGBA16Float,
                                                readback.size() * sizeof(u16), readback.data()))
        {
            return ::testing::AssertionFailure()
                   << "RGBA16F Texture2DArray readback was refused";
        }

        for (u32 layer = 0; layer < kLayers; ++layer)
        {
            for (sizet word = 0; word < kWordsPerLayer; ++word)
            {
                const auto expected = kLayerWords[layer][word];
                const auto actual = readback[static_cast<sizet>(layer) * kWordsPerLayer + word];
                if (actual != expected)
                {
                    const auto texel = static_cast<u32>(word / kChannels);
                    const auto channel = static_cast<u32>(word % kChannels);
                    return ::testing::AssertionFailure()
                           << std::format("layer {}, texel ({}, {}), channel {}: expected half bits 0x{:04X}, got 0x{:04X}",
                                          layer, texel % kWidth, texel / kWidth, channel, expected, actual);
                }
            }
        }

        return ::testing::AssertionSuccess();
    }
} // namespace OloEngine::Tests
