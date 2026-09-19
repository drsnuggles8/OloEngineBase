#pragma once

// =============================================================================
// ProductionLeafLobeMirror.h — a C++ TRANSCRIPTION of the leaf transmission
// lobe, for issue #1255. NOT a reference, and not the production path.
//
// WHY IT EXISTS. The leaf term's only implementation is
// oloFoliageTransmissionDirect in assets/shaders/include/FoliageSurface.glsl.
// The independent slab reference that judges it is CPU arithmetic in an L1
// test, and an L1 test cannot run a shader — so the comparison needs the
// production side in C++.
//
// WHY THAT IS A SEAM, AND WHAT CLOSES IT. A transcription is exactly the
// substitution substituted-seams-compound.md is about: from here on, every
// claim the L1 test makes is a claim about THIS FILE, and the shader is free to
// drift out from under it while the tests stay green. So it does not stand
// alone. MaterialReferenceAovParityTest renders
// assets/shaders/tests/ShaderUnit_FoliageTransmissionSweep.glsl — which CALLS
// the production function rather than copying it — over a grid of view and
// light directions and pins this mirror against it texel for texel. Neither
// file is sufficient; the pair is.
//
// The header is separate from both so the dependency is visible: an L1 test and
// a shaderpipe test include the same declaration, and a reader can see that the
// mirror has a keeper.
//
// CONVENTION, matching the shader exactly:
//   N  the SHADING normal, already faced towards the viewer.
//   V  surface -> eye, unit.
//   L  surface -> light, unit. A backlight has dot(N, L) < 0.
//   The returned value has `tint`, `radiance` and `shadow` factored out,
//   because every question asked of it is about the lobe's shape and energy and
//   those three are plain multipliers the shader applies afterwards.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/FoliageLeafProfile.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine::Tests
{
    /// The scalar lobe: `thickness * (forward + wrap * back)`.
    ///
    /// Line for line against oloFoliageTransmissionDirect, including the
    /// degenerate half-vector fallback — a zero-length Ht is reachable at
    /// distortion 1 with the light exactly along -N, and normalising it would
    /// put a NaN in the frame, so the shader falls back to L and so does this.
    [[nodiscard]] inline f64 ProductionLeafLobe(const glm::dvec3& n, const glm::dvec3& v, const glm::dvec3& l,
                                                f64 thickness, const FoliageLeafProfile& profile) noexcept
    {
        if (thickness <= 0.0)
            return 0.0;

        glm::dvec3 ht = l + (n * static_cast<f64>(profile.Distortion));
        const f64 htLength = glm::length(ht);
        ht = (htLength > 1.0e-5) ? (ht / htLength) : l;

        const f64 forward =
            std::pow(std::max(glm::dot(v, -ht), 0.0), std::max(static_cast<f64>(profile.Power), 1.0));
        const f64 back = std::max(glm::dot(-n, l), 0.0);

        return thickness * (forward + (back * static_cast<f64>(profile.Wrap)));
    }
} // namespace OloEngine::Tests
