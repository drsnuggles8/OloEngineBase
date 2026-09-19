#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkinOcularSurface.h"

#include "OloEngine/Math/Math.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{

    namespace
    {
        // TWO epsilons, because the quantities they guard are in two units and
        // one number cannot be right for both. Repeated here rather than
        // included so this file has no dependency on the shader tree;
        // ShaderUnit_SkinOcularSurface asserts the two sides agree about both.
        //
        // The SQUARED-LENGTH one, shared with include/PBRCommon.glsl's
        // `kOcularLinearEpsilon`, which is what every `dot(v, v)` guard in this
        // engine compares against.
        constexpr f32 kSquaredLengthEpsilon = 1.0e-20f;
        // The LINEAR one, for lengths, axial components and interval widths. It
        // is NOT 1e-20, and the difference is not pedantry: a length compared
        // against 1e-20 is compared against zero, so a tangential component of
        // 1e-12 — which normalizes to noise — would pass a "is this usable"
        // test and then be divided by. 1e-6 is below anything a unit-sphere
        // normal legitimately produces and above the noise floor of one.
        constexpr f32 kOcularLinearEpsilon = 1.0e-6f;

        // A direction that is finite and long enough to normalize. The
        // `!(x > eps)` form throughout this file, so a NaN takes the failure
        // branch instead of being normalized into one.
        [[nodiscard]] bool IsUsableDirection(const glm::vec3& v) noexcept
        {
            if (!Math::IsFinite(v))
                return false;
            return glm::dot(v, v) > kSquaredLengthEpsilon;
        }

        // smoothstep, transcribed from the GLSL builtin. Written out rather
        // than called through glm so the parity test compares two identical
        // expressions and not two libraries' idea of the same one.
        [[nodiscard]] f32 SmoothStep(f32 edge0, f32 edge1, f32 x) noexcept
        {
            const f32 span = edge1 - edge0;
            if (!(std::abs(span) > kOcularLinearEpsilon))
                return x < edge0 ? 0.0f : 1.0f;
            const f32 t = std::clamp((x - edge0) / span, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
    } // namespace

    f32 SkinCorneaEta(f32 ior) noexcept
    {
        if (!std::isfinite(ior))
            return kSkinOcularAmbientIor / SkinOcularParameters{}.CorneaIor;

        const f32 clamped = std::clamp(ior, kMinSkinCorneaIor, kMaxSkinCorneaIor);
        return kSkinOcularAmbientIor / clamped;
    }

    glm::vec3 SkinCornealNormal(const glm::vec3& globeNormal, const glm::vec3& axis, f32 curvatureRatio) noexcept
    {
        if (!IsUsableDirection(globeNormal) || !IsUsableDirection(axis))
            return globeNormal;
        if (!std::isfinite(curvatureRatio))
            return globeNormal;

        const f32 ratio = std::clamp(curvatureRatio, kMinSkinCorneaCurvatureRatio, kMaxSkinCorneaCurvatureRatio);
        // A ratio of exactly 1 returns the input UNTOUCHED — not renormalized,
        // not reconstructed from an angle it round-tripped through, untouched.
        // That exactness is what makes "this mesh already has corneal geometry"
        // a true identity rather than a frame that merely looks the same, and
        // it is the arm a mesh-based eye asset would ship on.
        if (ratio == 1.0f)
            return globeNormal;

        const glm::vec3 n = glm::normalize(globeNormal);
        const glm::vec3 a = glm::normalize(axis);

        const f32 cosTheta = std::clamp(glm::dot(n, a), -1.0f, 1.0f);
        const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));

        // The tangential direction, away from the axis. Exactly on the axis
        // there is none, and there is also nothing to bend: the corneal apex's
        // normal IS the optical axis.
        const glm::vec3 tangential = n - a * cosTheta;
        const f32 tangentialLen = glm::length(tangential);
        if (!(tangentialLen > kOcularLinearEpsilon))
            return globeNormal;

        // sin(phi) = ratio * sin(theta) — see the derivation in the header.
        // Clamped at 1 because beyond it the corneal cap has ended; on a
        // clinically authored profile the product never exceeds 0.75 inside the
        // limbus, so this clamp is unreachable and exists for a corrupt lane.
        const f32 sinPhi = std::clamp(ratio * sinTheta, 0.0f, 1.0f);
        const f32 cosPhi = std::sqrt(std::max(0.0f, 1.0f - sinPhi * sinPhi));

        const glm::vec3 bent = a * cosPhi + (tangential / tangentialLen) * sinPhi;
        if (!IsUsableDirection(bent))
            return globeNormal;
        return glm::normalize(bent);
    }

    bool SkinOcularRefract(const glm::vec3& incident, const glm::vec3& normal, f32 eta,
                           glm::vec3& refracted) noexcept
    {
        if (!IsUsableDirection(incident) || !IsUsableDirection(normal) || !std::isfinite(eta))
            return false;

        const glm::vec3 i = glm::normalize(incident);
        const glm::vec3 n = glm::normalize(normal);

        // Exactly GLSL's refract(): cosI is the angle between the normal and
        // the ray coming IN, so it is the negated dot.
        const f32 cosI = -glm::dot(n, i);
        const f32 k = 1.0f - eta * eta * (1.0f - cosI * cosI);
        // `!(k >= 0)` rather than `k < 0`, so a NaN k reports total internal
        // reflection instead of propagating into a square root.
        if (!(k >= 0.0f))
            return false;

        const glm::vec3 out = eta * i + (eta * cosI - std::sqrt(k)) * n;
        if (!IsUsableDirection(out))
            return false;

        refracted = glm::normalize(out);
        return true;
    }

    bool SkinIrisPlaneHit(const glm::vec3& globeNormal, const glm::vec3& axis, const glm::vec3& refracted,
                          f32 irisPlaneDepth, glm::vec3& hit) noexcept
    {
        if (!IsUsableDirection(globeNormal) || !IsUsableDirection(axis) || !IsUsableDirection(refracted))
            return false;
        if (!std::isfinite(irisPlaneDepth))
            return false;

        const glm::vec3 a = glm::normalize(axis);
        // The entry point. On a unit sphere the surface point IS the normal,
        // which is the whole reason this march needs no vertex position and no
        // eye centre — see the header.
        const glm::vec3 entry = glm::normalize(globeNormal);
        const glm::vec3 dir = glm::normalize(refracted);

        const f32 depth = std::clamp(irisPlaneDepth, kMinSkinIrisPlaneDepthRatio, kMaxSkinIrisPlaneDepthRatio);
        // The apex is at axial coordinate 1, so the plane sits at 1 - depth.
        const f32 planeAxial = 1.0f - depth;

        const f32 entryAxial = glm::dot(entry, a);
        const f32 dirAxial = glm::dot(dir, a);
        // The ray must travel INWARD. A ray that does not cannot reach the
        // plane, and solving for t anyway would place the iris in front of the
        // eye — a landing point that is finite, continuous and behind the
        // camera, which is the shape of bug that survives a screenshot.
        if (!(dirAxial < -kOcularLinearEpsilon))
            return false;

        const f32 t = (entryAxial - planeAxial) / -dirAxial;
        if (!(t >= 0.0f) || !std::isfinite(t))
            return false;

        hit = entry + dir * t;
        return Math::IsFinite(hit);
    }

    f32 SkinIrisDiscMask(f32 radial, f32 band) noexcept
    {
        if (!std::isfinite(radial) || !std::isfinite(band))
            return 0.0f;
        const f32 clampedBand = std::clamp(band, kMinSkinLimbalRingWidthRatio, kMaxSkinLimbalRingWidthRatio);
        return 1.0f - SmoothStep(1.0f - clampedBand, 1.0f, radial);
    }

    f32 SkinIrisLimbalRing(f32 radial, f32 band, f32 strength) noexcept
    {
        if (!std::isfinite(radial) || !std::isfinite(band) || !std::isfinite(strength))
            return 1.0f;

        const f32 clampedStrength = std::clamp(strength, kMinSkinLimbalRingStrength, kMaxSkinLimbalRingStrength);
        // A zero strength returns EXACTLY 1 for every input — not a smoothstep
        // multiplied by zero and subtracted, exactly 1 — so a profile with no
        // ring is bit-identical to one with no ring code.
        if (!(clampedStrength > 0.0f))
            return 1.0f;

        // One band inside the iris edge, so the disc mask's fade over
        // [1 - band, 1] has somewhere to work. See the header.
        const f32 clampedBand = std::clamp(band, kMinSkinLimbalRingWidthRatio, kMaxSkinLimbalRingWidthRatio);
        return 1.0f - clampedStrength * SmoothStep(1.0f - 2.0f * clampedBand, 1.0f - clampedBand, radial);
    }

    f32 SkinIrisPupilMask(f32 radial, f32 pupilRadial, f32 darkening) noexcept
    {
        if (!std::isfinite(radial) || !std::isfinite(pupilRadial) || !std::isfinite(darkening))
            return 1.0f;

        const f32 clampedDarkening = std::clamp(darkening, 0.0f, 1.0f);
        if (!(clampedDarkening > 0.0f))
            return 1.0f;

        const f32 edge = std::clamp(pupilRadial, kMinSkinPupilRadialRatio, kMaxSkinPupilRadialRatio);
        // A SOFT edge, over a band proportional to the pupil rather than a
        // fixed one: the pupil margin is a real physical boundary a few tens of
        // microns across, and a hard step there aliases into a crawling dotted
        // ring under any temporal upscaler. The band scales with the pupil so
        // that a constricted pupil does not become all edge.
        const f32 band = std::max(edge * kSkinPupilEdgeBandFraction, kOcularLinearEpsilon);
        // 1 inside the pupil, 0 outside it — hence `1 - smoothstep`.
        const f32 inside = 1.0f - SmoothStep(edge - band, edge + band, radial);
        return 1.0f - clampedDarkening * inside;
    }

    glm::vec3 SkinIrisShadingNormal(const glm::vec3& normal, const glm::vec3& axis,
                                    const glm::vec3& irisPlanePoint, f32 irisRadius, f32 concavity) noexcept
    {
        if (!IsUsableDirection(normal) || !IsUsableDirection(axis))
            return normal;
        if (!Math::IsFinite(irisPlanePoint) || !std::isfinite(concavity) || !std::isfinite(irisRadius))
            return normal;
        if (!(irisRadius > 0.0f))
            return normal;

        const f32 clampedConcavity = std::clamp(concavity, kMinSkinIrisConcavity, kMaxSkinIrisConcavity);
        // Exactly 0 returns the input UNTOUCHED — see the header; this is the
        // bit-identity a golden image asserts against.
        if (!(clampedConcavity > 0.0f))
            return normal;

        const glm::vec3 a = glm::normalize(axis);
        const glm::vec3 perpendicular = irisPlanePoint - a * glm::dot(irisPlanePoint, a);
        const f32 perpendicularLen = glm::length(perpendicular);
        // On the axis the funnel has no radial direction and no tilt.
        if (!(perpendicularLen > kOcularLinearEpsilon))
            return normal;

        // A PERTURBATION OF THE CORNEAL NORMAL, NOT A REPLACEMENT, and that is
        // the decision in this function worth defending. The surface being
        // shaded is still the CORNEA: the bright highlight on an eye is the
        // corneal reflection, and a shader that swapped in an iris-plane normal
        // would slide that highlight across the eye as the iris tilt changed —
        // which is the single most recognisable way for an eye to look wrong.
        //
        // So the tilt is added to the existing normal and bounded by the
        // authored concavity, giving the iris a diffuse gradient that MOVES
        // WITH THE LIGHT (the depth response issue #1244's second criterion
        // asks for) while leaving the specular where the cornea put it.
        // RAMPED TO ZERO AT THE IRIS EDGE, which makes the dish a paraboloid
        // rather than a cone. A cone's slope is constant, so its tilt would not
        // vanish at the limbus and the shading normal would step discontinuously
        // across that boundary — a hard ring around the iris that no amount of
        // filtering removes, at exactly the radius the limbal ring is already
        // drawing attention to. `perpendicularLen` is in eye radii and the iris
        // edge is where it reaches the iris radius, so the ramp is expressed in
        // the disc coordinate the rest of the response uses.
        const glm::vec3 outward = perpendicular / perpendicularLen;
        const f32 edgeRamp = std::clamp(1.0f - perpendicularLen / std::max(irisRadius, kOcularLinearEpsilon), 0.0f, 1.0f);
        return glm::normalize(normal + outward * (clampedConcavity * edgeRamp));
    }

    SkinOcularResult ApplySkinOcularSurface(const glm::vec3& albedo, const glm::vec3& normal,
                                            const glm::vec3& view, const glm::vec3& axis,
                                            const glm::vec4& corneaLane, const glm::vec4& irisLane,
                                            const glm::vec4& responseLane,
                                            const glm::vec4& tintLane) noexcept
    {
        SkinOcularResult result{ albedo, normal, -1.0f, false, SkinOcularFallbackReason::NotAuthored };

        const f32 ocularStrength = irisLane.w;
        // A zero master returns the inputs UNTOUCHED — not blended with
        // themselves, untouched — so a version-5 profile whose author has not
        // asked for an eye is bit-identical to the version-4 frame and costs
        // one compare rather than a refraction.
        if (!(ocularStrength > 0.0f))
            return result;

        if (!Math::IsFinite(albedo) || !IsUsableDirection(normal) || !IsUsableDirection(view) ||
            !IsUsableDirection(axis))
            return result;

        const glm::vec3 n = glm::normalize(normal);
        const glm::vec3 v = glm::normalize(view);
        const glm::vec3 a = glm::normalize(axis);

        // ---- Is this pixel cornea, or is it sclera? -------------------------
        //
        // The limbus is a real anatomical boundary and it is also the boundary
        // of this feature: outside it there is no anterior chamber and nothing
        // to refract, and the surface is scleral collagen — which is version-4
        // skin and is already shading correctly. Returning UNTOUCHED here, not
        // "refracting by zero", is what keeps an eye's white a skin term.
        const f32 cosTheta = glm::dot(n, a);
        if (!(cosTheta >= corneaLane.w))
        {
            result.Reason = SkinOcularFallbackReason::OutsideLimbus;
            return result;
        }

        // ---- Refract at the cornea -----------------------------------------
        const glm::vec3 cornealNormal = SkinCornealNormal(n, a, corneaLane.y);
        glm::vec3 refracted{ 0.0f };
        // `-v`: the view points from the surface toward the eye, the incident
        // direction is its negation. Negated ONCE, here — see the contract on
        // SkinOcularRefract.
        if (!SkinOcularRefract(-v, cornealNormal, corneaLane.x, refracted))
        {
            result.Reason = SkinOcularFallbackReason::TotalInternalReflection;
            return result;
        }

        glm::vec3 refractedHit{ 0.0f };
        if (!SkinIrisPlaneHit(n, a, refracted, corneaLane.z, refractedHit))
        {
            result.Reason = SkinOcularFallbackReason::RayDoesNotReachIris;
            return result;
        }

        // ---- The iris point, on the quality ladder --------------------------
        //
        // Interpolated as a PERPENDICULAR OFFSET rather than as a 3D point,
        // because the two endpoints do not lie on the same plane: the painted
        // arm reads the iris at the SURFACE point's own lateral coordinate (the
        // `painted` arm of the optical reference, exactly) and the refracted arm
        // reads it on the iris plane. Interpolating the offsets keeps the result
        // on the iris plane for every value of the ladder, so crossing it is
        // continuous and never moves the iris off its own plane.
        const f32 refractionStrength = std::clamp(responseLane.w, 0.0f, 1.0f);
        const glm::vec3 paintedOffset = n - a * glm::dot(n, a);
        const glm::vec3 refractedOffset = refractedHit - a * glm::dot(refractedHit, a);
        const glm::vec3 offset = paintedOffset + (refractedOffset - paintedOffset) * refractionStrength;

        const f32 planeAxial =
            1.0f - std::clamp(corneaLane.z, kMinSkinIrisPlaneDepthRatio, kMaxSkinIrisPlaneDepthRatio);
        const glm::vec3 irisPoint = a * planeAxial + offset;

        const f32 irisRadius = irisLane.x;
        if (!std::isfinite(irisRadius) || !(irisRadius > 0.0f))
            return result;
        const f32 radial = glm::length(offset) / irisRadius;
        if (!std::isfinite(radial))
            return result;

        // ---- The iris response ----------------------------------------------
        //
        // THE ORDER HERE IS THE LAYER ORDER, and it is fixed rather than
        // depth-sorted: the refraction above decided WHICH iris point this is,
        // and the ring, the pupil and the tilt are all properties OF THAT POINT.
        // Reordering them would be a different eye; there is no arrangement of
        // them that can sort wrongly.
        const f32 ring = SkinIrisLimbalRing(radial, irisLane.z, responseLane.x);
        const f32 pupil = SkinIrisPupilMask(radial, irisLane.y, responseLane.y);

        const f32 master = std::clamp(ocularStrength, 0.0f, 1.0f);
        const f32 irisGain = ring * pupil;

        // THE IRIS TINT. Multiplied INSIDE the disc and faded out at the
        // limbus, so the sclera keeps whatever the material authored — an eye's
        // white is not a property of its iris colour.
        //
        // The three iris terms COMMUTE — ring, pupil and tint are all
        // properties of the same iris point — so they are combined into one
        // gain here and the master blends that once rather than three times.
        const glm::vec3 tint = glm::clamp(glm::vec3(tintLane), glm::vec3(0.0f), glm::vec3(1.0f));

        // THE DISC MASK FADES THE WHOLE IRIS RESPONSE, not just the tint, and
        // that is the fix for a hard edge rather than a refinement of one. The
        // limbus is reached by TWO different boundaries — the geometric cone
        // test on the surface normal, and this radial coordinate on the
        // REFRACTED iris point — and they do not coincide. So a term that is
        // still non-neutral when either boundary is crossed steps.
        //
        // Fading tint, ring and pupil together over [1 - band, 1] makes every
        // one of them reach exactly 1 at the iris edge, which is the value the
        // sclera outside it already has.
        const glm::vec3 discGain = tint * irisGain;
        const f32 discMask = SkinIrisDiscMask(radial, tintLane.w);
        const glm::vec3 faded = glm::vec3(1.0f) + (discGain - glm::vec3(1.0f)) * discMask;

        // Written as `1 + master * (gain - 1)` for the reason
        // SkinOralCavityWeight is: at master == 0 it is 1 EXACTLY for every
        // finite gain, which is what makes the neutral arm bit-identical.
        const glm::vec3 shadedAlbedo = albedo * (glm::vec3(1.0f) + master * (faded - glm::vec3(1.0f)));

        const glm::vec3 tilted =
            SkinIrisShadingNormal(n, a, irisPoint, irisRadius, responseLane.z * master);

        result.Albedo = shadedAlbedo;
        result.Normal = tilted;
        result.IrisRadial = radial;
        result.Refracted = refractionStrength > 0.0f;
        // `None` on the full-model path, and `NotAuthored` when the quality
        // ladder has been turned all the way down: the iris still has a pupil,
        // a ring and a tilt, but the refraction the feature is named for did
        // not apply, and a caller asking "did this pixel refract?" deserves the
        // distinction rather than a bool it has to interpret.
        result.Reason = result.Refracted ? SkinOcularFallbackReason::None
                                         : SkinOcularFallbackReason::NotAuthored;
        return result;
    }

    // -------------------------------------------------------------------------
    // The lanes
    // -------------------------------------------------------------------------

    namespace
    {
        // The two lengths every ratio below divides by, sanitized once so that
        // four lanes cannot disagree about which eye they are describing.
        // A profile that reached here unsanitized would divide by zero
        // otherwise, and an inf in a lane reaches the GPU.
        struct SkinOcularScale
        {
            f32 EyeRadiusMM;
            f32 IrisRadiusMM;
        };

        [[nodiscard]] SkinOcularScale ResolveScale(const SkinOcularParameters& ocular) noexcept
        {
            const SkinOcularParameters defaults{};
            const f32 eye = std::isfinite(ocular.EyeRadiusMM)
                                ? std::clamp(ocular.EyeRadiusMM, kMinSkinEyeRadiusMM, kMaxSkinEyeRadiusMM)
                                : defaults.EyeRadiusMM;
            const f32 iris = std::isfinite(ocular.IrisRadiusMM)
                                 ? std::clamp(ocular.IrisRadiusMM, kMinSkinIrisRadiusMM, kMaxSkinIrisRadiusMM)
                                 : defaults.IrisRadiusMM;
            // The iris cannot be wider than the eye it sits in. Clamped rather
            // than reported because SkinProfileParameters::Sanitize is this
            // engine's one validation gate and it has already had its say; this
            // is the arithmetic refusing to produce a limbus cosine of NaN.
            return SkinOcularScale{ eye, std::min(iris, eye) };
        }
    } // namespace

    glm::vec4 SkinOcularCorneaLane(const SkinProfileParameters& parameters) noexcept
    {
        const SkinOcularParameters& ocular = parameters.Ocular;
        const SkinOcularScale scale = ResolveScale(ocular);

        const f32 corneaRadius = std::isfinite(ocular.CorneaRadiusMM)
                                     ? std::clamp(ocular.CorneaRadiusMM, kMinSkinCorneaRadiusMM,
                                                  kMaxSkinCorneaRadiusMM)
                                     : SkinOcularParameters{}.CorneaRadiusMM;
        const f32 planeDepth = std::isfinite(ocular.IrisPlaneDepthMM)
                                   ? std::clamp(ocular.IrisPlaneDepthMM, kMinSkinIrisPlaneDepthMM,
                                                kMaxSkinIrisPlaneDepthMM)
                                   : SkinOcularParameters{}.IrisPlaneDepthMM;

        // sin(limbus) = irisRadius / eyeRadius, so cos is the complement. The
        // ratio cannot exceed 1 — ResolveScale caps the iris at the eye — so
        // the square root's argument cannot go negative.
        const f32 sinLimbus = std::clamp(scale.IrisRadiusMM / scale.EyeRadiusMM, 0.0f, 1.0f);
        const f32 cosLimbus = std::sqrt(std::max(0.0f, 1.0f - sinLimbus * sinLimbus));

        return glm::vec4(SkinCorneaEta(ocular.CorneaIor),
                         std::clamp(scale.EyeRadiusMM / corneaRadius, kMinSkinCorneaCurvatureRatio,
                                    kMaxSkinCorneaCurvatureRatio),
                         std::clamp(planeDepth / scale.EyeRadiusMM, kMinSkinIrisPlaneDepthRatio,
                                    kMaxSkinIrisPlaneDepthRatio),
                         cosLimbus);
    }

    glm::vec4 SkinOcularIrisLane(const SkinProfileParameters& parameters) noexcept
    {
        const SkinOcularParameters& ocular = parameters.Ocular;
        const SkinOcularScale scale = ResolveScale(ocular);

        const f32 pupilRadius = std::isfinite(ocular.PupilRadiusMM)
                                    ? std::clamp(ocular.PupilRadiusMM, kMinSkinPupilRadiusMM,
                                                 kMaxSkinPupilRadiusMM)
                                    : SkinOcularParameters{}.PupilRadiusMM;
        const f32 ringWidth = std::isfinite(ocular.LimbalRingWidthMM)
                                  ? std::clamp(ocular.LimbalRingWidthMM, kMinSkinLimbalRingWidthMM,
                                               kMaxSkinLimbalRingWidthMM)
                                  : SkinOcularParameters{}.LimbalRingWidthMM;

        return glm::vec4(std::clamp(scale.IrisRadiusMM / scale.EyeRadiusMM, 0.0f, 1.0f),
                         std::clamp(pupilRadius / scale.IrisRadiusMM, kMinSkinPupilRadialRatio,
                                    kMaxSkinPupilRadialRatio),
                         std::clamp(ringWidth / scale.IrisRadiusMM, kMinSkinLimbalRingWidthRatio,
                                    kMaxSkinLimbalRingWidthRatio),
                         std::isfinite(ocular.OcularStrength)
                             ? std::clamp(ocular.OcularStrength, 0.0f, 1.0f)
                             : 0.0f);
    }

    glm::vec4 SkinOcularTintLane(const SkinProfileParameters& parameters) noexcept
    {
        const SkinOcularParameters& ocular = parameters.Ocular;
        const SkinOcularParameters defaults{};
        const SkinOcularScale scale = ResolveScale(ocular);

        const auto channel = [](f32 value, f32 fallback) noexcept -> f32
        {
            return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
        };
        const f32 ringWidth = std::isfinite(ocular.LimbalRingWidthMM)
                                  ? std::clamp(ocular.LimbalRingWidthMM, kMinSkinLimbalRingWidthMM,
                                               kMaxSkinLimbalRingWidthMM)
                                  : defaults.LimbalRingWidthMM;

        return glm::vec4(channel(ocular.IrisColor.r, defaults.IrisColor.r),
                         channel(ocular.IrisColor.g, defaults.IrisColor.g),
                         channel(ocular.IrisColor.b, defaults.IrisColor.b),
                         // THE SAME BOUNDS THE IRIS LANE'S RING WIDTH USES, so the
                         // two components are the same number for every profile —
                         // which is what makes "the iris edge and the limbal ring
                         // are one boundary" true rather than nearly true. Clamping
                         // them differently would let a large authored width put the
                         // ring at 0.45 and the fade at 1.0, and the ring would then
                         // sit outside the disc it is supposed to be the rim of.
                         std::clamp(ringWidth / scale.IrisRadiusMM, kMinSkinLimbalRingWidthRatio,
                                    kMaxSkinLimbalRingWidthRatio));
    }

    glm::vec4 SkinOcularResponseLane(const SkinProfileParameters& parameters) noexcept
    {
        const SkinOcularParameters& ocular = parameters.Ocular;
        const SkinOcularParameters defaults{};

        const auto sane = [](f32 value, f32 lo, f32 hi, f32 fallback) noexcept -> f32
        {
            return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
        };

        return glm::vec4(sane(ocular.LimbalRingStrength, kMinSkinLimbalRingStrength,
                              kMaxSkinLimbalRingStrength, defaults.LimbalRingStrength),
                         sane(ocular.PupilDarkening, 0.0f, 1.0f, defaults.PupilDarkening),
                         sane(ocular.IrisConcavity, kMinSkinIrisConcavity, kMaxSkinIrisConcavity,
                              defaults.IrisConcavity),
                         sane(ocular.RefractionStrength, 0.0f, 1.0f, defaults.RefractionStrength));
    }

} // namespace OloEngine
