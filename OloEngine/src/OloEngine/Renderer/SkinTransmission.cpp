#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkinTransmission.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/SkinDiffusion.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    f32 SkinThicknessMM(f32 thicknessMetres, f32 mapSample, f32 thicknessScale) noexcept
    {
        // Every input is tested, not asserted. The thickness factor comes from a
        // material (already clamped non-negative by Material::SetThicknessFactor),
        // the map sample from a texture fetch, and the scale from a sanitized
        // profile — but a corrupt asset pack can put a NaN in a texture, and the
        // one thing this function must never return is a non-finite thickness
        // that becomes a non-finite optical depth and then a NaN pixel.
        if (!std::isfinite(thicknessMetres) || !std::isfinite(mapSample) || !std::isfinite(thicknessScale))
            return kSkinThicknessMissing;
        if (thicknessMetres <= 0.0f || mapSample <= 0.0f || thicknessScale <= 0.0f)
            return kSkinThicknessMissing;

        const f32 millimetres = thicknessMetres * std::clamp(mapSample, 0.0f, 1.0f) * thicknessScale;
        if (!std::isfinite(millimetres))
            return kSkinThicknessMissing;

        return std::clamp(millimetres, 0.0f, kMaxSkinThicknessMM);
    }

    f32 SkinThicknessBaseMM(f32 thicknessMetres, f32 thicknessScale) noexcept
    {
        // The full chain with the map sample held at 1, which is exactly what
        // "no map" means — expressed by CALLING the full chain rather than by
        // repeating its arithmetic, so the two cannot drift into different units.
        return SkinThicknessMM(thicknessMetres, 1.0f, thicknessScale);
    }

    glm::vec3 SkinTransmittance(f32 thicknessMM, const SkinProfileParameters& parameters) noexcept
    {
        // A missing or degenerate thickness transmits NOTHING. This is the
        // conservative fallback the issue's third criterion asks for, and the
        // sign of the inequality is the whole point: the OTHER reading of a zero
        // thickness is "infinitely thin, therefore exp(0) = 1, therefore fully
        // transparent", which is the uniformly emissive head. Returning black
        // makes an unauthored head look UNFINISHED rather than WRONG, and the
        // caller counts the substitution.
        if (!std::isfinite(thicknessMM) || thicknessMM <= 0.0f)
            return glm::vec3(0.0f);

        // THE SAME `d` THE DIFFUSION KERNEL IS BUILT FROM. Not a second opinion
        // about the mean free path — literally SkinBurleyScalingMM, so an author
        // who widens the scattering radius gets a deeper-transmitting surface
        // and a wider blur from one edit, and the two features cannot disagree
        // about how far light travels in this skin.
        const glm::vec3 d = SkinBurleyScalingMM(parameters);

        const f32 clampedThickness = std::min(thicknessMM, kMaxSkinThicknessMM);

        glm::vec3 transmittance(0.0f);
        for (int channel = 0; channel < 3; ++channel)
        {
            // d is guaranteed strictly positive and finite by SkinBurleyScalingMM's
            // contract (ScatterRadiusMM has a positive floor and the shape fit is
            // bounded below on [0,1]), so this division is safe. The guard is
            // still here because "guaranteed by another header's contract" is
            // exactly the kind of guarantee that survives until someone edits
            // that header, and the cost is one compare.
            const f32 scaling = d[channel];
            if (!std::isfinite(scaling) || scaling <= 0.0f)
            {
                transmittance[channel] = 0.0f;
                continue;
            }

            const f32 opticalDepth = clampedThickness / scaling;
            const f32 survival = std::exp(-opticalDepth);
            // ScatterColor is the transport albedo: the fraction that leaves the
            // surface again rather than being absorbed. Multiplying it in here
            // is what makes red reach the near face when blue does not.
            const f32 value = parameters.ScatterColor[channel] * survival;
            transmittance[channel] = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
        }

        return transmittance;
    }

    f32 SkinTransmissionLobe(const glm::vec3& normal, const glm::vec3& view, const glm::vec3& lightDir,
                             const SkinTransmissionParameters& parameters) noexcept
    {
        // Direction vectors arrive normalised from every production caller, and
        // a non-finite one would poison the dot products into a NaN weight that
        // multiplies the whole term. Tested rather than trusted.
        if (!Math::IsFinite(normal) || !Math::IsFinite(view) || !Math::IsFinite(lightDir))
            return 0.0f;

        // COUPLING INTO THE FAR FACE, and the exact zero that premise 1 of the
        // energy argument rests on: this is 0 over the whole near hemisphere,
        // so the reflected diffuse lobe (which carries +dot(N, L)) and this term
        // are never both non-zero for the same light. No wrap, no half-Lambert.
        const f32 backFacing = std::clamp(-glm::dot(normal, lightDir), 0.0f, 1.0f);
        if (backFacing <= 0.0f)
            return 0.0f;

        // The viewer looking TOWARD the light through the surface. `lightDir`
        // points from the surface toward the light, so -lightDir is the
        // direction the transmitted light continues travelling in.
        const f32 forward = std::clamp(glm::dot(view, -lightDir), 0.0f, 1.0f);

        const f32 power = std::clamp(parameters.Power, kMinSkinTransmissionPower, kMaxSkinTransmissionPower);
        const f32 anisotropy =
            std::clamp(parameters.Anisotropy, kMinSkinTransmissionAnisotropy, kMaxSkinTransmissionAnisotropy);

        // A CONVEX COMBINATION, so the result is in [0,1] for every input — the
        // arithmetic, not the parameter values, is what bounds it (premise 3).
        // std::pow of a value in [0,1] by an exponent >= 1 stays in [0,1].
        const f32 directional = std::pow(forward, power);
        const f32 shape = ((1.0f - anisotropy) * 1.0f) + (anisotropy * directional);

        const f32 lobe = backFacing * shape;
        return std::isfinite(lobe) ? std::clamp(lobe, 0.0f, 1.0f) : 0.0f;
    }

    glm::vec4 SkinTransmissionScatterLane(const SkinProfileParameters& parameters) noexcept
    {
        const f32 strength =
            std::clamp(parameters.Transmission.Strength, kMinSkinTransmissionStrength, kMaxSkinTransmissionStrength);
        const f32 anisotropy = std::clamp(parameters.Transmission.Anisotropy, kMinSkinTransmissionAnisotropy,
                                          kMaxSkinTransmissionAnisotropy);
        return glm::vec4(glm::clamp(parameters.ScatterColor, glm::vec3(0.0f), glm::vec3(1.0f)) * strength, anisotropy);
    }

    glm::vec4 SkinTransmissionScalingLane(const SkinProfileParameters& parameters) noexcept
    {
        const glm::vec3 scaling = glm::max(SkinBurleyScalingMM(parameters), glm::vec3(kMinSkinTransmissionScalingMM));
        const f32 power = std::clamp(parameters.Transmission.Power, kMinSkinTransmissionPower, kMaxSkinTransmissionPower);
        return glm::vec4(scaling, power);
    }

    glm::vec3 EvaluateSkinTransmission(const glm::vec3& normal, const glm::vec3& view, const glm::vec3& lightDir,
                                       const glm::vec3& radiance, const glm::vec3& albedo, f32 visibility,
                                       f32 thicknessMM, const SkinProfileParameters& parameters) noexcept
    {
        // The transport VERSION branch, here as well as in the shader — the same
        // arrangement oloApplySkinProfile uses, and for the same reason: a
        // profile authored against an older transport must not acquire this term
        // because a renderer setting was switched on. A version this function has
        // no arm for transmits NOTHING rather than guessing.
        // BOTH TRANSMITTING VERSIONS (issue #1243 appended the second). The
        // versions are CUMULATIVE — version 3 is "everything version 2 does,
        // plus the layered specular" — so testing only for version 2 here would
        // make a head stop transmitting through its ears the moment its author
        // turned the specular lobes on.
        if (parameters.EvaluationModel != SkinEvaluationModel::ThicknessTransmission &&
            parameters.EvaluationModel != SkinEvaluationModel::LayeredSpecular &&
            parameters.EvaluationModel != SkinEvaluationModel::OralSurface &&
            parameters.EvaluationModel != SkinEvaluationModel::OcularSurface)
            return glm::vec3(0.0f);

        // The inputs a caller could hand in non-finite. The lanes below are
        // derived from a sanitized profile and need no such test.
        if (!std::isfinite(visibility) || !std::isfinite(thicknessMM))
            return glm::vec3(0.0f);
        if (!Math::IsFinite(radiance) || !Math::IsFinite(albedo) || !Math::IsFinite(normal) ||
            !Math::IsFinite(view) || !Math::IsFinite(lightDir))
            return glm::vec3(0.0f);

        const glm::vec3 result = EvaluateSkinTransmissionLanes(
            normal, view, lightDir, glm::max(radiance, glm::vec3(0.0f)),
            glm::clamp(albedo, glm::vec3(0.0f), glm::vec3(1.0f)), std::clamp(visibility, 0.0f, 1.0f), thicknessMM,
            SkinTransmissionScatterLane(parameters), SkinTransmissionScalingLane(parameters));

        return Math::IsFinite(result) ? glm::max(result, glm::vec3(0.0f)) : glm::vec3(0.0f);
    }

    glm::vec3 EvaluateSkinTransmissionLanes(const glm::vec3& normal, const glm::vec3& view, const glm::vec3& lightDir,
                                            const glm::vec3& radiance, const glm::vec3& albedo, f32 visibility,
                                            f32 thicknessMM, const glm::vec4& scatterLane,
                                            const glm::vec4& scalingLane) noexcept
    {
        // THIS FUNCTION AND oloSkinTransmissionDirect IN include/SkinTransmission.glsl
        // ARE THE SAME EXPRESSION IN THE SAME ORDER, operation for operation.
        // That is what makes SkinTransmissionParityEvidenceTest a test of the
        // arithmetic rather than of two algebraically-equal rearrangements: a
        // reassociation here would show up as a last-bit disagreement there,
        // which is a far cheaper way to find a transcription slip than a
        // screenshot is.

        // A missing thickness transmits nothing — the conservative fallback,
        // and the same guard the shader opens with.
        if (thicknessMM <= 0.0f)
            return glm::vec3(0.0f);

        // Premise 1 of the energy argument: exactly zero over the whole near
        // hemisphere, so this term and the reflected diffuse lobe are never both
        // non-zero for one light. No wrap, no half-Lambert, no epsilon.
        const f32 backFacing = std::clamp(-glm::dot(normal, lightDir), 0.0f, 1.0f);
        if (backFacing <= 0.0f)
            return glm::vec3(0.0f);

        // The viewer looking TOWARD the light through the surface. `lightDir`
        // points from the surface toward the light, so -lightDir is the
        // direction transmitted light continues travelling in.
        const f32 forward = std::clamp(glm::dot(view, -lightDir), 0.0f, 1.0f);

        // A convex combination of two values in [0,1], so bounded by 1 for every
        // input (premise 3) — bounded by the arithmetic, not by the parameters.
        const f32 directional = std::pow(forward, scalingLane.w);
        const f32 lobe = backFacing * (((1.0f - scatterLane.w) * 1.0f) + (scatterLane.w * directional));

        // Beer-Lambert through the authored thickness, per channel, against the
        // SAME Burley scaling the diffusion kernel is built from.
        const glm::vec3 opticalDepth = glm::vec3(std::min(thicknessMM, kMaxSkinThicknessMM)) /
                                       glm::max(glm::vec3(scalingLane), glm::vec3(kMinSkinTransmissionScalingMM));
        const glm::vec3 transmittance = glm::vec3(scatterLane) * glm::exp(-opticalDepth);

        return radiance * transmittance * albedo * lobe * visibility;
    }

    glm::vec3 SkinTransmissionEnergyBound(f32 thicknessMM, const glm::vec3& albedo,
                                          const SkinProfileParameters& parameters) noexcept
    {
        const glm::vec3 clampedAlbedo = glm::clamp(albedo, glm::vec3(0.0f), glm::vec3(1.0f));

        // THE DIFFUSE SIDE. A Lambertian diffuse lobe delivers at most `albedo`
        // times the incident radiance, at dot(N, L) = 1. Diffusion preserves
        // that total (premise 2: unit-sum kernel, `blur(aux) - aux`), so this is
        // the bound both before and after the diffusion pass — which is why this
        // function does not need the kernel.
        const glm::vec3 diffuseBound = clampedAlbedo;

        // THE TRANSMITTED SIDE. The lobe is at most 1 (premise 3) and visibility
        // at most 1, so the term is at most transmittance * albedo * strength.
        const f32 strength = std::clamp(parameters.Transmission.Strength, kMinSkinTransmissionStrength,
                                        kMaxSkinTransmissionStrength);
        const glm::vec3 transmittedBound =
            ((parameters.EvaluationModel == SkinEvaluationModel::ThicknessTransmission) ||
             (parameters.EvaluationModel == SkinEvaluationModel::LayeredSpecular) ||
             (parameters.EvaluationModel == SkinEvaluationModel::OralSurface) ||
             (parameters.EvaluationModel == SkinEvaluationModel::OcularSurface))
                ? SkinTransmittance(thicknessMM, parameters) * clampedAlbedo * strength
                : glm::vec3(0.0f);

        // THE MAX, NOT THE SUM, and premise 1 is what licenses it: the two terms
        // are driven by saturate(+dot(N, L)) and saturate(-dot(N, L)), which are
        // never both positive, so no single light can pay into both. Summing
        // them here would report a bound twice as loose as the truth and would
        // make the contract test pass for a wrapped lobe — the exact defect the
        // absence of a Wrap parameter exists to prevent.
        return glm::max(diffuseBound, transmittedBound);
    }

} // namespace OloEngine
