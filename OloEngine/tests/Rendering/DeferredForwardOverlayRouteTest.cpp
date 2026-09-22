// OLO_TEST_LAYER: unit
// =============================================================================
// DeferredForwardOverlayRouteTest.cpp
//
// The routing contract for materials the G-Buffer cannot represent: which PBR
// draws the Deferred path sends to ForwardOverlayPass instead of writing them
// into the G-Buffer. Transmission is issue #970; alpha blending is issue #1404,
// where a blended mesh written into the G-Buffer shaded to pure black.
//
// Every combination of the five inputs is enumerated, so the two reasons for
// rerouting cannot quietly start disagreeing (#1404 AC4). The real-pixel proof
// that the reroute composites correctly is
// PropertyTests/TransparentBlendOrderVisualEvidenceTest.cpp.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/DeferredForwardOverlayRoute.h"
#include "OloEngine/Renderer/Material.h"

#include <gtest/gtest.h>

#include <string>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    [[nodiscard]] auto Route(bool deferred, bool blended, bool transmissive, bool hasOverlay = true, bool hasShader = true)
        -> DeferredForwardOverlayRoute
    {
        return SelectDeferredForwardOverlayRoute({ .Deferred = deferred,
                                                   .Blended = blended,
                                                   .Transmissive = transmissive,
                                                   .HasForwardOverlayPass = hasOverlay,
                                                   .HasForwardShader = hasShader });
    }
} // namespace

TEST(DeferredForwardOverlayRoute, OpaqueMaterialStaysInTheGBuffer)
{
    EXPECT_EQ(Route(true, false, false), DeferredForwardOverlayRoute::None);
}

TEST(DeferredForwardOverlayRoute, BlendedMaterialIsReroutedOnDeferred)
{
    // Issue #1404: the case that rendered black.
    EXPECT_EQ(Route(true, true, false), DeferredForwardOverlayRoute::ForwardOverlay);
}

TEST(DeferredForwardOverlayRoute, TransmissiveMaterialKeepsItsReroute)
{
    // Issue #970's behaviour, unchanged.
    EXPECT_EQ(Route(true, false, true), DeferredForwardOverlayRoute::ForwardOverlay);
}

TEST(DeferredForwardOverlayRoute, BlendedAndTransmissiveAgreeOnTheOverlay)
{
    // AC4: the two reasons do not fight. Both want the overlay, so a material
    // carrying both goes there, and removing either reason alone keeps it there.
    EXPECT_EQ(Route(true, true, true), DeferredForwardOverlayRoute::ForwardOverlay);
    EXPECT_EQ(Route(true, true, true), Route(true, true, false));
    EXPECT_EQ(Route(true, true, true), Route(true, false, true));
}

TEST(DeferredForwardOverlayRoute, ForwardPathsNeverReroute)
{
    // Forward and Forward+ have no G-Buffer: ForwardOverlayPass is not even in
    // their graph, and the caller's forward shader choice already handles both.
    for (int mask = 0; mask < 16; ++mask)
    {
        const bool blended = (mask & 1) != 0;
        const bool transmissive = (mask & 2) != 0;
        const bool hasOverlay = (mask & 4) != 0;
        const bool hasShader = (mask & 8) != 0;
        EXPECT_EQ(Route(false, blended, transmissive, hasOverlay, hasShader), DeferredForwardOverlayRoute::None)
            << "blended=" << blended << " transmissive=" << transmissive << " hasOverlay=" << hasOverlay
            << " hasShader=" << hasShader;
    }
}

TEST(DeferredForwardOverlayRoute, MissingOverlayOrShaderIsReportedNotAccepted)
{
    // A draw that needs the overlay but cannot have it must come back as
    // Unavailable, so the caller reports it, and never as None, which would
    // mean "the G-Buffer is fine for this" and is exactly the #1404 defect.
    for (const bool blended : { false, true })
    {
        for (const bool transmissive : { false, true })
        {
            if (!blended && !transmissive)
                continue;
            const std::string what = "blended=" + std::to_string(blended) + " transmissive=" + std::to_string(transmissive);
            EXPECT_EQ(Route(true, blended, transmissive, false, true), DeferredForwardOverlayRoute::Unavailable) << what;
            EXPECT_EQ(Route(true, blended, transmissive, true, false), DeferredForwardOverlayRoute::Unavailable) << what;
            EXPECT_EQ(Route(true, blended, transmissive, false, false), DeferredForwardOverlayRoute::Unavailable) << what;
        }
    }
    // An opaque material never needs the overlay, so its absence is no problem.
    EXPECT_EQ(Route(true, false, false, false, false), DeferredForwardOverlayRoute::None);
}

TEST(DeferredForwardOverlayRoute, EveryCombinationMatchesTheRule)
{
    // The whole table, stated once as the rule and checked against all 32 rows.
    for (int mask = 0; mask < 32; ++mask)
    {
        const bool deferred = (mask & 1) != 0;
        const bool blended = (mask & 2) != 0;
        const bool transmissive = (mask & 4) != 0;
        const bool hasOverlay = (mask & 8) != 0;
        const bool hasShader = (mask & 16) != 0;

        DeferredForwardOverlayRoute expected = DeferredForwardOverlayRoute::None;
        if (deferred && (blended || transmissive))
            expected = (hasOverlay && hasShader) ? DeferredForwardOverlayRoute::ForwardOverlay
                                                 : DeferredForwardOverlayRoute::Unavailable;

        EXPECT_EQ(Route(deferred, blended, transmissive, hasOverlay, hasShader), expected) << "mask=" << mask;
    }
}

TEST(DeferredForwardOverlayRoute, TheRouteReadsTheSameBlendFlagAsTheRenderState)
{
    // The call sites feed `Blended` from MaterialFlag::Blend, the flag
    // CreatePODRenderStateForMaterial turns into SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    // and the sort key turns into a depth-major transparent key. AlphaMode::Blend
    // on its own does not enable blending (Material.h: "also requires
    // MaterialFlag::Blend"), so it must not be what the route keys on either.
    Material material;
    material.SetAlphaMode(AlphaMode::Blend);
    EXPECT_FALSE(material.GetFlag(MaterialFlag::Blend))
        << "AlphaMode::Blend now implies MaterialFlag::Blend; revisit which input the route reads";
    material.SetFlag(MaterialFlag::Blend, true);
    EXPECT_TRUE(material.GetFlag(MaterialFlag::Blend));
}
