#pragma once

// =============================================================================
// BlueNoiseTexture.h — upload/bind glue for the shared blue-noise tile (#706).
//
// The tile itself (and every property claim about it) lives in BlueNoise.h,
// which is deliberately free of renderer dependencies so the tests can exercise
// the generator without a GL context. This header is the thin layer that turns
// those bytes into a texture and puts it on TEX_BLUE_NOISE.
//
// -----------------------------------------------------------------------------
// WHY A HELPER RATHER THAN TWO COPIES
// -----------------------------------------------------------------------------
// Both adopting passes need the same texture with the same SAMPLER STATE, and
// the sampler state is not cosmetic: the shader indexes the tile with
// texelFetch and a bitmask, so a LINEAR filter would blend neighbouring
// blue-noise values into something measurably less blue, and it would do it
// silently — the image stays plausible, just noisier. One helper means the two
// passes cannot end up disagreeing about it.
//
// -----------------------------------------------------------------------------
// WHY EACH PASS OWNS ITS OWN TEXTURE
// -----------------------------------------------------------------------------
// 32 KB apiece, and in exchange there is no shared GPU object with a lifetime
// spanning subsystems — the shape docs/agent-rules/lazy-static-release-ownership.md
// is about, where a lazily-created resource released from Renderer3D::Shutdown
// leaks in every session that never brought 3D up. The CPU-side tile IS shared
// (BlueNoise::GetTileRG(), a POD array with no destruction order), which is
// where the cost that actually matters was.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/BlueNoise.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    // @brief Create the blue-noise tile as an RG8 texture. Call once, from a
    // pass's Init(); the caller owns the handle and must release it with
    // DestroyBlueNoiseTexture().
    [[nodiscard]] inline RHI::ResourceHandle CreateBlueNoiseTexture()
    {
        OLO_PROFILE_FUNCTION();

        const auto& tile = BlueNoise::GetTileRG();
        const RHI::ResourceHandle handle =
            RenderCommand::CreateTexture2DHandle(BlueNoise::kTileSize, BlueNoise::kTileSize, RHI::Format::RG8UNorm);
        RenderCommand::UploadTextureSubImage2D(handle, BlueNoise::kTileSize, BlueNoise::kTileSize,
                                               RHI::Format::RG8UNorm, tile.data());
        // Nearest + Repeat. Nearest because the shader texelFetches exact tile
        // entries and any interpolation between them is a low-pass filter on the
        // one property this texture exists to have.
        RenderCommand::SetTextureFilter(handle, RHI::Filter::Nearest, RHI::Filter::Nearest);
        RenderCommand::SetTextureWrap(handle, RHI::AddressMode::Repeat);
        return handle;
    }

    // @brief Release a texture from CreateBlueNoiseTexture(). Safe on an invalid
    // handle, so a pass that never reached Init() can call it unconditionally.
    inline void DestroyBlueNoiseTexture(RHI::ResourceHandle& handle)
    {
        if (!handle.IsValid())
            return;

        // Retire the heap descriptors naming this texture BEFORE deleting it:
        // ResourceRegistry keeps a handle alive across an in-place reload, so a
        // view's own generation cannot tell that its descriptor now names a
        // deleted object and the persistent view cache would keep serving the
        // stale entry (issue #691).
        RHI::DescriptorHeap::Get().InvalidateResource(handle);
        RenderCommand::DeleteTexture(handle);
        handle = {};
    }

    // @brief Bind the tile at TEX_BLUE_NOISE with the sampler state the shader
    // assumes. Persistent lifetime: the texture is pass-owned and its handle is
    // stable for the pass's lifetime, so the heap offset can be memoised.
    inline void BindBlueNoiseTexture(RGCommandContext& context, RHI::ResourceHandle handle)
    {
        if (!handle.IsValid())
            return;

        RHI::SamplerDesc sampler;
        sampler.Source = RHI::SamplerSource::Explicit;
        sampler.MinFilter = RHI::Filter::Nearest;
        sampler.MagFilter = RHI::Filter::Nearest;
        sampler.LinearMipFilter = false;
        sampler.AddressU = RHI::AddressMode::Repeat;
        sampler.AddressV = RHI::AddressMode::Repeat;
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_BLUE_NOISE, handle,
                                        RHI::HeapSlotLifetime::Persistent, sampler);
    }
} // namespace OloEngine
