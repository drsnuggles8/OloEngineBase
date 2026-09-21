#pragma once

// =============================================================================
// GroomProxyDiagnostics.h — the two levers the live A/B for #1253 needs, and
// nothing else.
//
// WHY THESE EXIST AT ALL. Ray tracing is Vulkan-only here, so every cell of
// this feature's verification matrix is LIVE-ONLY — the headless evidence
// fixtures need a GL 4.6 context and skip without one, so the suite can never
// capture a Vulkan frame of it. A live A/B therefore has to be doable against a
// RUNNING editor, and without these it would need a rebuild per arm, which is
// how a comparison quietly turns into two runs of different binaries.
//
// `Disabled` is the arm that proves the double-count invariant: with groom
// proxies off and on, the COAT's own pixels must be identical while the body
// and the ground beneath it darken. A coat that dimmed would mean it is being
// occluded by a second copy of itself — see GroomRayTracingProxy.h.
//
// `ForceTier` is the arm that makes criterion 1's detailed-versus-proxy
// comparison a controlled one: the two frames differ in the REPRESENTATION and
// in nothing else, rather than in the camera distance that would otherwise be
// the only way to reach the far tier.
//
// Render-thread-only, transient, and owned by NOTHING on disk: no scene, no
// save game, no asset and no renderer setting carries this. It is off on every
// launch and there is no path that persists it, which is what keeps a
// diagnostic from becoming a configuration.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomRayTracingProxy.h"

#include <optional>

namespace OloEngine::RayTracing::GroomProxyDiagnostics
{
    /// Refuse every groom, as though the frame had asked for no hybrid effect.
    /// Counted as NotRequested, so the panel says why rather than showing a
    /// scene that silently lost its coats.
    ///
    /// Seeded once, at first use, from `OLO_GROOM_RT_PROXIES=0` — so the
    /// comparison is runnable across two launches as well as inside one
    /// session. Set it BEFORE launching; reading it per frame would make a
    /// control that can change under its own measurement.
    void SetDisabled(bool disabled);
    [[nodiscard]] bool GetDisabled();

    /// Pin every coat to one tier regardless of its apparent size. Empty is the
    /// shipping behaviour: the ladder decides.
    void SetForcedTier(std::optional<GroomProxyTier> tier);
    [[nodiscard]] std::optional<GroomProxyTier> GetForcedTier();
} // namespace OloEngine::RayTracing::GroomProxyDiagnostics
