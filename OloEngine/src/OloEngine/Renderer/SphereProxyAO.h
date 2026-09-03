#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace OloEngine
{
    // @brief Analytic sphere-proxy ambient occlusion — the CPU half (issue #710).
    //
    // Screen-space AO (GTAO, SSAO) can only occlude with geometry that is in the
    // depth buffer, so a large object at the edge of the frame stops occluding
    // the moment it leaves it and its contact darkening POPS. This term fixes
    // exactly that gap: a handful of coarse spheres fitted to the scene's
    // occluders, evaluated with a closed-form integral that references the
    // RECEIVER's hemisphere and never the camera — so a proxy beside or behind
    // the camera contributes just as it did on screen.
    //
    // Everything here is pure glm/std, no GL: SphereOcclusion is the exact
    // integral the compute shader evaluates (SphereProxyAO.comp keeps a textual
    // mirror), and the fitting/selection below is what the render pass feeds it.
    // That is deliberate — SphereProxyAOMathTest pins the integral and the fit
    // on the CPU, where a wrong constant is a failed assertion instead of a
    // plausible-looking frame.
    namespace SphereProxyAO
    {
        // One proxy occluder: a world-space sphere.
        struct Proxy
        {
            glm::vec3 Center{ 0.0f };
            f32 Radius = 0.0f;
        };

        // Upper bound on the spheres one bounding box is split into. An AABB is
        // split along its longest axis when a single sphere would fit it badly
        // (a corridor, a wall, a felled tree); four is where the split stops
        // paying for itself against the per-pixel cost of the extra spheres.
        inline constexpr u32 kMaxSpheresPerBounds = 4u;

        // The UBO carries a fixed-length proxy array (see SphereProxyAOUBO), so
        // this is a hard cap on what one frame can evaluate, not a soft budget.
        // 128 vec4s is 2 KB — an eighth of the 16 KB uniform-block size GL 4.6
        // guarantees, and small enough that the per-tile binning loop below is
        // cheaper than the shading it saves.
        inline constexpr u32 kMaxProxies = 128u;

        // A receiver sitting ON a proxy's surface has h == 1, where the
        // horizon-crossing branch's sqrt(h^2 - 1) is 0 and its reciprocal
        // diverges. Inside the proxy (h < 1) the same square root is imaginary.
        // Both are reachable — the proxy is fitted to the occluder's own bounds,
        // so the occluder's own surface is right at h == 1 — so h^2 is clamped
        // to just above the tangent case, which is finite, correct in the limit
        // (occlusion -> nl, the tangent-sphere hemisphere) and continuous.
        //
        // The margin is not free: it biases the exact-tangent answer LOW by
        // about sqrt(margin)/pi. 1e-4 cost 0.0064 of occlusion at h == 1 (the
        // known-exact 0.5 case in SphereProxyAOMathTest read 0.4936); 1e-6 costs
        // 0.0006, still comfortably above the f32 epsilon of 1.0 (1.2e-7) so
        // `h2 - 1` stays strictly positive.
        inline constexpr f32 kMinTangentMargin = 1e-6f;

        // Below this the direction to the sphere centre is not recoverable.
        inline constexpr f32 kMinCentreDistance = 1e-6f;

        // Width, in proxy radii, of the ramp that fades a proxy out against its
        // OWN object's surface — see SelfOcclusionFade.
        //
        // Kept NARROW because the fitted sphere is contained in its bounding box
        // (FitProxySpheres caps the radius at the smallest half-extent), so the
        // object's own surface sits at h == 1 rather than somewhere inside the
        // sphere. The ramp only has to bridge the tangent point, not a
        // poke-through region — and every radius the ramp covers is a radius of
        // CONTACT occlusion it deletes, which is the most valuable part of the
        // term. At 0.35 with an uncontained fit it deleted the whole contact
        // ring around an object's base.
        inline constexpr f32 kSelfOcclusionFadeWidth = 0.15f;

        // @brief Closed-form ambient occlusion of one sphere at one receiver.
        //
        // Returns the fraction of the receiver's cosine-weighted hemisphere that
        // the sphere covers, in [0, 1] — 0 for a sphere entirely below the
        // horizon, 1 for one that fills the hemisphere.
        //
        // Implemented from Inigo Quilez, "Sphere Ambient Occlusion"
        // (https://iquilezles.org/articles/sphereao). Two regimes:
        //
        //   * The cone subtended by the sphere lies entirely above the receiver's
        //     tangent plane (`h * nl >= 1`, i.e. k2 <= 0). The integral collapses
        //     to the classic sphere form factor `cos(gamma) * sin^2(alpha)`,
        //     which in these variables is `nl / h^2`.
        //   * The cone crosses the horizon. Then it is the horizon-clipped form
        //     (Lagarde/de Rousiers), which after substituting
        //     `sin(theta) * sqrt(1 - y^2) == sqrt(k2)` reduces to the compact
        //     expression below.
        //
        // The two agree exactly at the boundary (k2 == 0 gives acos(-1) = pi and
        // atan(0) = 0, leaving nl / h^2), so the function is continuous across it.
        //
        // `sphere` is xyz = centre, w = radius, in the SAME space as `position`
        // and `normal` (the pass works in view space; the tests work in world
        // space — the integral does not care, it is rigid-transform invariant).
        [[nodiscard]] inline f32 SphereOcclusion(const glm::vec3& position, const glm::vec3& normal,
                                                 const glm::vec4& sphere)
        {
            const f32 radius = sphere.w;
            if (!(radius > 0.0f))
                return 0.0f;

            const glm::vec3 toCentre = glm::vec3(sphere) - position;
            const f32 distanceSq = glm::dot(toCentre, toCentre);
            if (!(distanceSq > kMinCentreDistance * kMinCentreDistance))
            {
                // The receiver is at the sphere's centre: every direction is
                // blocked.
                return 1.0f;
            }

            const f32 distance = std::sqrt(distanceSq);
            const f32 nl = glm::dot(normal, toCentre / distance);

            // h is the centre distance in radii; h^2 - 1 is what both branches
            // are built on, hence the clamp described at kMinTangentMargin.
            const f32 ratio = distance / radius;
            const f32 h2 = std::max(ratio * ratio, 1.0f + kMinTangentMargin);
            const f32 k2 = 1.0f - h2 * nl * nl;

            if (k2 <= 0.0f)
            {
                // Entirely above (nl > 0) or entirely below (nl <= 0) the horizon.
                return std::clamp(std::max(0.0f, nl) / h2, 0.0f, 1.0f);
            }

            // Horizon-crossing. `1 - nl*nl` is strictly positive here: k2 > 0
            // forces |nl| < 1/h <= 1.
            const f32 sinThetaSq = std::max(1.0f - nl * nl, kMinTangentMargin);
            const f32 hSq = h2 - 1.0f;
            const f32 y = std::clamp(-nl * std::sqrt(hSq / sinThetaSq), -1.0f, 1.0f);

            const f32 clipped = nl * std::acos(y) - std::sqrt(k2 * hSq);
            const f32 result = (clipped / h2 + std::atan(std::sqrt(k2 / hSq))) / glm::pi<f32>();
            return std::clamp(result, 0.0f, 1.0f);
        }

        // @brief Fade a proxy out where the receiver is its OWN object's surface.
        //
        // Returns 0 for a receiver inside the sphere, 1 beyond
        // `1 + kSelfOcclusionFadeWidth` radii, and a smooth ramp between.
        // Multiply SphereOcclusion by it; the integral itself stays the pure
        // closed form so it can be pinned against known-exact configurations.
        //
        // This is a MODELLING decision, not a numerical guard, and it is not
        // optional. A volume-matched sphere is larger than the object's
        // inscribed sphere, so it pokes through the object's own faces — and at
        // h <= 1 the closed form is not merely imprecise, it is being asked a
        // question with no answer (a receiver strictly inside a sphere is fully
        // enclosed by it, so the geometric truth there is occlusion 1). Clamping
        // h to the tangent case instead, which is what this replaced, painted a
        // soft dark disc across the occluder's own front face — plainly visible
        // in SphereProxyAO_ProxyTerm_*.png before the fade existed. Objects do
        // not occlude themselves in this term; their own surface detail is
        // exactly what the screen-space AO pass is for.
        //
        // The ramp rather than a hard cut-off is what keeps the term continuous
        // across the proxy's surface, so no seam appears where a receiver
        // crosses it.
        [[nodiscard]] inline f32 SelfOcclusionFade(f32 centreDistance, f32 radius)
        {
            if (!(radius > 0.0f))
                return 0.0f;
            const f32 ratio = centreDistance / radius;
            const f32 t = std::clamp((ratio - 1.0f) / kSelfOcclusionFadeWidth, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t); // smoothstep
        }

        // @brief The proxy's contribution at a receiver: the integral, faded.
        //
        // What the shader accumulates, and what the pass means by "occlusion
        // from this proxy". Kept separate from SphereOcclusion so the integral
        // stays pinnable against exact geometry.
        [[nodiscard]] inline f32 ProxyOcclusion(const glm::vec3& position, const glm::vec3& normal,
                                                const glm::vec4& sphere)
        {
            const glm::vec3 toCentre = glm::vec3(sphere) - position;
            const f32 fade = SelfOcclusionFade(std::sqrt(glm::dot(toCentre, toCentre)), sphere.w);
            if (!(fade > 0.0f))
                return 0.0f;
            return SphereOcclusion(position, normal, sphere) * fade;
        }

        // @brief Fit up to `kMaxSpheresPerBounds` proxy spheres inside a world AABB.
        //
        // The spheres are VOLUME-MATCHED and CONTAINED. Volume-matched so a proxy
        // neither swallows the box (a circumscribed sphere over-occludes by 3.4x
        // in volume on a cube) nor rattles inside it; contained — the radius is
        // capped at the box's smallest half-extent — so the sphere never pokes
        // out through the object's own faces. Containment is what keeps
        // SelfOcclusionFade a thin guard around the tangent point instead of a
        // hole punched through the contact region, and it is why a flat slab
        // gets a small proxy: a sphere is a poor model for a slab, and
        // under-occluding is the safe direction to be wrong in.
        //
        // The count is chosen from the box's anisotropy — a
        // corridor gets a line of spheres along its long axis rather than one
        // sphere that is far too fat at the ends and far too thin in the middle,
        // which is what "one or MORE bounding spheres per occluder" is for.
        //
        // Returns the number written to `out` (0 when the box is degenerate,
        // non-finite, the NoBounds sentinel, or so large that the fitted radius
        // exceeds `maxRadius` — see SelectProxies for why that filter matters).
        [[nodiscard]] u32 FitProxySpheres(const BoundingBox& bounds, f32 maxRadius, Proxy* out, u32 outCapacity);

        // @brief Pick the `maxCount` proxies that matter most from a frame's bounds.
        //
        // Ranked by the peak occlusion each proxy could contribute, `r^2 / d^2`
        // against the view position — the `nl / h^2` term above with nl = 1.
        // That is the right ranking precisely because it is the integral's own
        // magnitude, so the proxies that get dropped are the ones that would have
        // moved the AO buffer least.
        //
        // `maxRadius` rejects proxies too big to be an "object": the ground, a
        // terrain chunk, a whole building shell. Those are not what this term is
        // for — a kilometre-wide sphere fitted to the ground plane sits under
        // every receiver in the scene and tints the entire frame — and the
        // screen-space term plus shadows already handle them.
        [[nodiscard]] std::vector<Proxy> SelectProxies(std::span<const BoundingBox> bounds,
                                                       const glm::vec3& viewPosition, u32 maxCount, f32 maxRadius);
    } // namespace SphereProxyAO
} // namespace OloEngine
