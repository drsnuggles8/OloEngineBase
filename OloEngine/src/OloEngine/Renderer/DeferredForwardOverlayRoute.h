#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // Where the default-shaded PBR material of a classic mesh draw must go.
    //
    // The G-Buffer carries albedo/metallic, normal/roughness/AO, emissive/flags,
    // velocity, entity-ID and baked GI: one opaque surface per pixel. Two kinds
    // of material have no representation in it.
    //
    // - Transmissive (issue #970): nothing can hold a transmission factor, an
    //   IOR, a thickness or an extinction coefficient, so the surface comes back
    //   out of DeferredLightingPass as an ordinary opaque one.
    // - Alpha-blended (issue #1404): the material's SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    //   render state is applied to the G-Buffer channels themselves, blending
    //   albedo, normal and the packed flags lane as if they were colour, and the
    //   lighting pass shades the result to black.
    //
    // Both are sent to ForwardOverlayPass, which binds the scene framebuffer and
    // runs AFTER the deferred composite, so the forward PBR shader shades the
    // surface over the finished lit image, exactly as Forward and Forward+ do.
    // A material that is BOTH blended and transmissive needs no tie-break: both
    // rules want the same destination.
    enum class DeferredForwardOverlayRoute : u8
    {
        // Not the Deferred path, or a material the G-Buffer represents. The
        // caller keeps its normal shader choice.
        None,
        // Shade with the forward PBR shader and submit to ForwardOverlayPass.
        ForwardOverlay,
        // The material needs the overlay, but there is no ForwardOverlayPass or
        // no forward shader to shade it with. The caller keeps its normal
        // choice, and the draw is reported, never quietly accepted.
        Unavailable,
    };

    struct DeferredForwardOverlayInputs
    {
        bool Deferred = false;
        bool Blended = false;      // MaterialFlag::Blend, the flag the render state and sort key read.
        bool Transmissive = false; // Material::IsTransmissive().
        bool HasForwardOverlayPass = false;
        bool HasForwardShader = false;
    };

    [[nodiscard]] constexpr auto SelectDeferredForwardOverlayRoute(const DeferredForwardOverlayInputs& in)
        -> DeferredForwardOverlayRoute
    {
        if (!in.Deferred || (!in.Blended && !in.Transmissive))
            return DeferredForwardOverlayRoute::None;
        if (in.HasForwardOverlayPass && in.HasForwardShader)
            return DeferredForwardOverlayRoute::ForwardOverlay;
        return DeferredForwardOverlayRoute::Unavailable;
    }
} // namespace OloEngine
