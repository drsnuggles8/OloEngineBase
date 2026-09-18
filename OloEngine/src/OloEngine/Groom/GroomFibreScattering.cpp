#include "OloEnginePCH.h"

#include "OloEngine/Groom/GroomFibreScattering.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kPi = 3.14159265358979323846f;
        constexpr f32 kTwoPi = 2.0f * kPi;
        constexpr f32 kInvTwoPi = 1.0f / kTwoPi;

        // sqrt(pi/8), the constant Chiang's azimuthal-roughness fit is scaled
        // by so that s is a logistic scale rather than a Gaussian sigma.
        constexpr f32 kSqrtPiOver8 = 0.626657069f;

        [[nodiscard]] constexpr f32 Sqr(f32 x) noexcept
        {
            return x * x;
        }

        [[nodiscard]] f32 SafeSqrt(f32 x) noexcept
        {
            return std::sqrt(std::max(0.0f, x));
        }

        [[nodiscard]] f32 SafeASin(f32 x) noexcept
        {
            return std::asin(std::clamp(x, -1.0f, 1.0f));
        }

        // The modified Bessel function of the first kind, order zero, as the
        // first ten terms of its series. GLSL has no factorial and no 64-bit
        // integer, so the coefficients 1 / (4^i (i!)^2) are written out as
        // constants and BOTH sides accumulate them in this order — the twin
        // contract is about values, and a Horner rearrangement would change
        // the rounding for no gain.
        constexpr std::array<f32, 10> kI0Coefficients{
            1.0f, 2.5e-1f, 1.5625e-2f, 4.34027778e-4f, 6.78168403e-6f,
            6.78168403e-8f, 4.70950280e-10f, 2.40280755e-12f, 9.38596699e-15f, 2.89690339e-17f
        };

        [[nodiscard]] f32 BesselI0(f32 x) noexcept
        {
            const f32 x2 = x * x;
            f32 x2i = 1.0f;
            f32 value = 0.0f;
            for (u32 i = 0; i < kI0Coefficients.size(); ++i)
            {
                value += x2i * kI0Coefficients[i];
                x2i *= x2;
            }
            return value;
        }

        // The series above overflows f32 well before the argument does, so the
        // large-x branch uses the asymptotic expansion instead. 12 is where the
        // two agree to better than a part in 10^6.
        [[nodiscard]] f32 LogBesselI0(f32 x) noexcept
        {
            if (x > 12.0f)
            {
                return x + (0.5f * (-std::log(kTwoPi) + std::log(1.0f / x) + (0.125f / x)));
            }
            return std::log(BesselI0(x));
        }

        // The longitudinal scattering function: a normalised distribution over
        // the outgoing longitudinal angle, centred on the specular cone. It
        // integrates to one against cos(theta) d(theta), which is half of why
        // the model conserves energy — the other half is the attenuations.
        //
        // The v <= 0.1 branch is not an optimisation. At small variance the
        // 1/sinh(1/v) normalisation is 2 e^(-1/v), which underflows f32 while
        // I0(a) overflows it, so the ratio is computed in log space instead.
        [[nodiscard]] f32 LongitudinalM(f32 cosThetaI, f32 cosThetaO, f32 sinThetaI, f32 sinThetaO, f32 v) noexcept
        {
            const f32 a = (cosThetaI * cosThetaO) / v;
            const f32 b = (sinThetaI * sinThetaO) / v;
            if (v <= 0.1f)
            {
                return std::exp(LogBesselI0(a) - b - (1.0f / v) + 0.6931f + std::log(1.0f / (2.0f * v)));
            }
            return (std::exp(-b) * BesselI0(a)) / (std::sinh(1.0f / v) * 2.0f * v);
        }

        [[nodiscard]] f32 Logistic(f32 x, f32 s) noexcept
        {
            const f32 e = std::exp(-std::abs(x) / s);
            return e / (s * Sqr(1.0f + e));
        }

        [[nodiscard]] f32 LogisticCDF(f32 x, f32 s) noexcept
        {
            return 1.0f / (1.0f + std::exp(-x / s));
        }

        // The logistic restricted to [-pi, pi] and renormalised there. Chiang's
        // choice over a Gaussian: it has a closed-form CDF, so the azimuthal
        // half of the sampling routine inverts analytically instead of
        // searching.
        [[nodiscard]] f32 TrimmedLogistic(f32 x, f32 s) noexcept
        {
            const f32 norm = LogisticCDF(kPi, s) - LogisticCDF(-kPi, s);
            return Logistic(x, s) / norm;
        }

        [[nodiscard]] f32 SampleTrimmedLogistic(f32 u, f32 s) noexcept
        {
            const f32 lo = LogisticCDF(-kPi, s);
            const f32 k = LogisticCDF(kPi, s) - lo;
            const f32 x = -s * std::log((1.0f / ((u * k) + lo)) - 1.0f);
            return std::clamp(x, -kPi, kPi);
        }

        // The exit azimuth of path p for a ray entering at gammaO and
        // refracting to gammaT. Pure geometry: p internal segments each turn
        // the ray by 2*gammaT, the entry turns it by -2*gammaO, and each
        // internal bounce adds a half turn.
        [[nodiscard]] constexpr f32 LobePhi(u32 p, f32 gammaO, f32 gammaT) noexcept
        {
            const f32 pf = static_cast<f32>(p);
            return (2.0f * pf * gammaT) - (2.0f * gammaO) + (pf * kPi);
        }

        // Wrap to [-pi, pi) WITHOUT a loop. The obvious `while (d > pi) d -=
        // 2pi` form is a data-dependent loop, which glsl-shaders.md §8b names
        // as a compile-time bomb on Mesa's AMD path — and the two sides of a
        // twin cannot differ in shape here without differing in rounding.
        [[nodiscard]] f32 WrapPhi(f32 phi) noexcept
        {
            return phi - (kTwoPi * std::floor((phi + kPi) * kInvTwoPi));
        }

        [[nodiscard]] f32 AzimuthalN(f32 phi, u32 p, f32 s, f32 gammaO, f32 gammaT) noexcept
        {
            return TrimmedLogistic(WrapPhi(phi - LobePhi(p, gammaO, gammaT)), s);
        }

        // THE QUADRATURE'S ROUGHNESS FLOOR — the measured fix for the spikes
        // the plain rule produces, and the reason the far field is not simply
        // an average of near-field samples.
        //
        // Each h-node contributes a logistic of scale `s` centred on that
        // node's exit azimuth, and adjacent nodes' exit azimuths are
        // |dPhi_p/dh| * dh apart. When that gap is wider than the logistic, the
        // sum is N separate spikes rather than one lobe: a coat rendered that
        // way bands as the strand tangent rotates, and the error against the
        // true far field is over 70 % RMS at every order a fragment shader can
        // afford. Table 1 of docs/analysis/groom-fibre-scattering-1247.md is
        // that measurement, and it is what rejected the plain rule.
        //
        // Widening each node's lobe to at least cover its own slice makes the
        // rule a piecewise reconstruction of the far field rather than a point
        // sample of it. What is left is a known azimuthal BLUR — a declared
        // approximation with a measured size, where the spikes were an
        // artefact with none.
        //
        // dPhi_p/dh = 2 p dGammaT/dh - 2 dGammaO/dh, both derivatives analytic.
        // Near |h| = 1 dGammaO/dh diverges, which is exactly where the fibre's
        // caustic is; a large widening there is the correct answer rather than
        // a failure of the bound.
        [[nodiscard]] f32 NodeWidenedScale(f32 s, u32 p, f32 h, f32 etaPrime, f32 nodeWidth) noexcept
        {
            const f32 safeEta = std::max(etaPrime, 1.0e-3f);
            const f32 dGammaO = 1.0f / std::max(SafeSqrt(1.0f - Sqr(h)), 1.0e-3f);
            const f32 dGammaT = (1.0f / safeEta) / std::max(SafeSqrt(1.0f - Sqr(h / safeEta)), 1.0e-3f);
            const f32 dPhi = std::abs((2.0f * static_cast<f32>(p) * dGammaT) - (2.0f * dGammaO));
            // A quarter of the slice width, because the trimmed logistic keeps
            // about half its mass inside +/- 1.1 s: a slice of width w is
            // covered by s ~ w/4. The constant is swept in the analysis rather
            // than assumed.
            return std::max(s, std::min(0.25f * dPhi * nodeWidth, kPi));
        }

        // Unpolarised Fresnel reflectance at a dielectric interface, entering
        // from vacuum into `eta`.
        [[nodiscard]] f32 FresnelDielectric(f32 cosThetaI, f32 eta) noexcept
        {
            cosThetaI = std::clamp(cosThetaI, -1.0f, 1.0f);
            f32 etaI = 1.0f;
            f32 etaT = eta;
            if (cosThetaI <= 0.0f)
            {
                etaI = eta;
                etaT = 1.0f;
                cosThetaI = -cosThetaI;
            }

            const f32 sinThetaI = SafeSqrt(1.0f - Sqr(cosThetaI));
            const f32 sinThetaT = (etaI / etaT) * sinThetaI;
            if (sinThetaT >= 1.0f)
            {
                return 1.0f; // total internal reflection
            }
            const f32 cosThetaT = SafeSqrt(1.0f - Sqr(sinThetaT));

            const f32 rParl = ((etaT * cosThetaI) - (etaI * cosThetaT)) / ((etaT * cosThetaI) + (etaI * cosThetaT));
            const f32 rPerp = ((etaI * cosThetaI) - (etaT * cosThetaT)) / ((etaI * cosThetaI) + (etaT * cosThetaT));
            return ((rParl * rParl) + (rPerp * rPerp)) * 0.5f;
        }

        // THE ENERGY-CONSERVATION MECHANISM, and the reason this model was
        // chosen over the closed-form fits.
        //
        // Each path's attenuation is built from the previous one's, so with no
        // absorption (T == 1) the four sum to EXACTLY one whatever the Fresnel
        // term is:
        //
        //   A0 = f                              reflected at the surface
        //   A1 = (1-f)^2 T                      in and straight out
        //   A2 = A1 T f                         one internal bounce
        //   A3 = A2 T f / (1 - T f)             the geometric tail of the rest
        //
        //   A0 + A1 + A2 + A3 = f + (1-f)^2 (1 + f + f^2 + ...) = 1  at T = 1.
        //
        // Folding the tail into a closed form rather than truncating is what
        // makes that identity exact instead of approximate, and it is why
        // criterion 4 can be a test. The 1 - T f denominator is guarded: at
        // grazing incidence f reaches 1 and, with no absorption, so does T.
        [[nodiscard]] std::array<glm::vec3, kGroomFibreLobeCount> Attenuations(f32 cosThetaO, f32 eta, f32 h,
                                                                               const glm::vec3& transmittance) noexcept
        {
            std::array<glm::vec3, kGroomFibreLobeCount> ap{};
            const f32 cosGammaO = SafeSqrt(1.0f - Sqr(h));
            const f32 f = FresnelDielectric(cosThetaO * cosGammaO, eta);

            ap[0] = glm::vec3(f);
            ap[1] = Sqr(1.0f - f) * transmittance;
            ap[2] = ap[1] * transmittance * f;

            const glm::vec3 denom = glm::max(glm::vec3(1.0f) - (transmittance * f), glm::vec3(1.0e-5f));
            ap[3] = (ap[2] * transmittance * f) / denom;
            return ap;
        }

        struct FibreGeometry
        {
            f32 CosThetaO = 1.0f;
            f32 CosThetaI = 1.0f;
            f32 GammaO = 0.0f;
            f32 GammaT = 0.0f;
            glm::vec3 Transmittance{ 1.0f };
        };

        [[nodiscard]] FibreGeometry MakeGeometry(const GroomFibreParams& params, f32 sinThetaO, f32 sinThetaI,
                                                 f32 h) noexcept
        {
            FibreGeometry geo;
            geo.CosThetaO = SafeSqrt(1.0f - Sqr(sinThetaO));
            geo.CosThetaI = SafeSqrt(1.0f - Sqr(sinThetaI));

            // The refracted longitudinal angle, and Bravais' "modified index"
            // — the effective index seen by the AZIMUTHAL problem once the
            // longitudinal tilt is projected out. Without it a fibre lit from
            // along its own length would refract as if lit head-on, and the
            // TT lobe would sit in the wrong place at exactly the grazing
            // angles hair is usually seen at.
            const f32 sinThetaT = sinThetaO / params.Eta;
            const f32 cosThetaT = SafeSqrt(1.0f - Sqr(sinThetaT));
            const f32 etaPrime = SafeSqrt(Sqr(params.Eta) - Sqr(sinThetaO)) / std::max(geo.CosThetaO, 1.0e-5f);

            const f32 sinGammaT = std::clamp(h / etaPrime, -1.0f, 1.0f);
            const f32 cosGammaT = SafeSqrt(1.0f - Sqr(sinGammaT));
            geo.GammaT = SafeASin(sinGammaT);
            geo.GammaO = SafeASin(h);

            // Beer-Lambert along the chord the refracted ray takes, in units of
            // the fibre DIAMETER — which is what makes sigma_a dimensionless
            // and a groom's absorption independent of how thick its strands
            // are. A thicker fibre of the same pigment is not darker in this
            // model, and that is deliberate: pigment concentration and fibre
            // width are separate authored quantities.
            geo.Transmittance = glm::exp(-params.SigmaA * ((2.0f * cosGammaT) / std::max(cosThetaT, 1.0e-5f)));
            return geo;
        }

        // The per-lobe tilt rotation. The cuticle scales lie like roof tiles,
        // so the surface a ray meets is tilted by alpha from the fibre axis —
        // and each path meets it a different number of times, which is why R
        // shifts one way by 2*alpha and TRT the other way by 4*alpha. Applied
        // as a rotation of the OUTGOING longitudinal angle rather than as an
        // offset to theta_h, so it stays exact at grazing angles.
        void ApplyTilt(const GroomFibreParams& params, u32 p, f32 sinThetaO, f32 cosThetaO, f32& outSin,
                       f32& outCos) noexcept
        {
            switch (p)
            {
                case 0:
                    outSin = (sinThetaO * params.Cos2kAlpha[1]) - (cosThetaO * params.Sin2kAlpha[1]);
                    outCos = (cosThetaO * params.Cos2kAlpha[1]) + (sinThetaO * params.Sin2kAlpha[1]);
                    break;
                case 1:
                    outSin = (sinThetaO * params.Cos2kAlpha[0]) + (cosThetaO * params.Sin2kAlpha[0]);
                    outCos = (cosThetaO * params.Cos2kAlpha[0]) - (sinThetaO * params.Sin2kAlpha[0]);
                    break;
                case 2:
                    outSin = (sinThetaO * params.Cos2kAlpha[2]) + (cosThetaO * params.Sin2kAlpha[2]);
                    outCos = (cosThetaO * params.Cos2kAlpha[2]) - (sinThetaO * params.Sin2kAlpha[2]);
                    break;
                default:
                    outSin = sinThetaO;
                    outCos = cosThetaO;
                    break;
            }
            outCos = std::abs(outCos);
        }

        // The quadrature node set. h_k is the MIDPOINT of the k-th of n equal
        // slices of [-1, 1], which is the stratified estimator's node and the
        // reason a single sample lands at h = 0 — the centre of the fibre,
        // exactly the degenerate near-field case every model reduces to.
        [[nodiscard]] f32 QuadratureNode(u32 k, u32 n) noexcept
        {
            return -1.0f + ((2.0f * (static_cast<f32>(k) + 0.5f)) / static_cast<f32>(n));
        }

        [[nodiscard]] f32 SanitizeScalar(f32 value, f32 fallback, f32 lo, f32 hi) noexcept
        {
            if (!std::isfinite(value))
            {
                return fallback;
            }
            return std::clamp(value, lo, hi);
        }

        [[nodiscard]] glm::vec3 SanitizeVec3(const glm::vec3& value, const glm::vec3& fallback, f32 lo,
                                             f32 hi) noexcept
        {
            glm::vec3 out = value;
            for (int c = 0; c < 3; ++c)
            {
                out[c] = SanitizeScalar(value[c], fallback[c], lo, hi);
            }
            return out;
        }

        // A deterministic 32-bit hash used only by GroomFibreSampledFurnace, so
        // its number is reproducible run to run. Not the strand pass's hash:
        // that one has a GPU twin and must not be changed by anything here.
        [[nodiscard]] u32 FurnaceHash(u32 x) noexcept
        {
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            x ^= x >> 16;
            return x;
        }

        [[nodiscard]] f32 FurnaceUniform(u32 index, u32 dimension, u32 seed) noexcept
        {
            const u32 h = FurnaceHash((index * 0x9e3779b9u) ^ (dimension * 0x85ebca6bu) ^ seed);
            return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Parameter construction
    // -------------------------------------------------------------------------

    GroomFibreAuthoring SanitizeGroomFibreAuthoring(const GroomFibreAuthoring& authored) noexcept
    {
        const GroomFibreAuthoring defaults{};
        GroomFibreAuthoring out = authored;

        if (!IsValidGroomFibrePigmentMode(static_cast<i32>(authored.PigmentMode)))
        {
            // REJECT, not clamp: the mode is a discriminated value, so
            // saturating an out-of-range one silently picks a DIFFERENT valid
            // mode and the coat renders a colour nobody authored.
            out.PigmentMode = defaults.PigmentMode;
        }

        out.Eumelanin = SanitizeScalar(authored.Eumelanin, defaults.Eumelanin, 0.0f, GroomFibreLimits::MaxConcentration);
        out.Pheomelanin =
            SanitizeScalar(authored.Pheomelanin, defaults.Pheomelanin, 0.0f, GroomFibreLimits::MaxConcentration);
        out.BaseColor = SanitizeVec3(authored.BaseColor, defaults.BaseColor, 0.0f, 1.0f);
        out.Absorption =
            SanitizeVec3(authored.Absorption, defaults.Absorption, 0.0f, GroomFibreLimits::MaxAbsorption);
        out.LongitudinalRoughness =
            SanitizeScalar(authored.LongitudinalRoughness, defaults.LongitudinalRoughness,
                           GroomFibreLimits::MinRoughness, GroomFibreLimits::MaxRoughness);
        out.AzimuthalRoughness = SanitizeScalar(authored.AzimuthalRoughness, defaults.AzimuthalRoughness,
                                                GroomFibreLimits::MinRoughness, GroomFibreLimits::MaxRoughness);
        out.TiltDegrees =
            SanitizeScalar(authored.TiltDegrees, defaults.TiltDegrees, 0.0f, GroomFibreLimits::MaxTiltDegrees);
        out.IndexOfRefraction = SanitizeScalar(authored.IndexOfRefraction, defaults.IndexOfRefraction,
                                               GroomFibreLimits::MinIOR, GroomFibreLimits::MaxIOR);
        out.Intensity = SanitizeScalar(authored.Intensity, defaults.Intensity, 0.0f, GroomFibreLimits::MaxIntensity);
        out.HSamples = std::clamp(authored.HSamples, GroomFibreLimits::MinHSamples, GroomFibreLimits::MaxHSamples);
        return out;
    }

    glm::vec3 GroomFibreSigmaAFromMelanin(f32 eumelanin, f32 pheomelanin) noexcept
    {
        const f32 eu = std::max(0.0f, std::isfinite(eumelanin) ? eumelanin : 0.0f);
        const f32 pheo = std::max(0.0f, std::isfinite(pheomelanin) ? pheomelanin : 0.0f);
        return (eu * kGroomEumelaninAbsorption) + (pheo * kGroomPheomelaninAbsorption);
    }

    glm::vec3 GroomFibreSigmaAFromColor(const glm::vec3& color, f32 azimuthalRoughness) noexcept
    {
        const f32 bn = std::clamp(std::isfinite(azimuthalRoughness) ? azimuthalRoughness : 0.3f, 0.0f, 1.0f);
        // Chiang et al. 2016, eq. 9: a polynomial in beta_N whose square-root
        // relationship to the reflectance is what folds multiple scattering
        // into the absorption.
        const f32 denom = 5.969f - (0.215f * bn) + (2.532f * Sqr(bn)) - (10.73f * bn * Sqr(bn)) +
                          (5.574f * Sqr(Sqr(bn))) + (0.245f * Sqr(Sqr(bn)) * bn);

        glm::vec3 sigma(0.0f);
        for (int c = 0; c < 3; ++c)
        {
            // A zero channel would take log(0) to -inf and then square it to
            // +inf, which is a legal authored colour (pure black) reaching a
            // NaN in the shader. Floored at the darkest colour the model can
            // still represent.
            const f32 channel = std::clamp(std::isfinite(color[c]) ? color[c] : 0.0f, 1.0e-4f, 1.0f);
            sigma[c] = Sqr(std::log(channel) / denom);
        }
        return glm::min(sigma, glm::vec3(GroomFibreLimits::MaxAbsorption));
    }

    glm::vec3 GroomFibreSigmaAForAlbedo(const glm::vec3& color, f32 eta, u32 hSamples) noexcept
    {
        const u32 n = std::clamp(hSamples, GroomFibreLimits::MinHSamples, GroomFibreLimits::MaxHSamples);
        const f32 safeEta = std::clamp(std::isfinite(eta) ? eta : kGroomFibreDefaultIOR, GroomFibreLimits::MinIOR,
                                       GroomFibreLimits::MaxIOR);

        // The head-on albedo for a scalar absorption: the mean over the
        // fibre's width of the four attenuations. cos(theta_o) is 1 head on, so
        // this is GroomFibreAmbientResponse's arithmetic with the angle fixed —
        // written once here rather than called through, because the caller
        // needs it per CHANNEL and the lobe set is per colour.
        const auto albedo = [&](f32 sigma) noexcept
        {
            f32 total = 0.0f;
            for (u32 k = 0; k < n; ++k)
            {
                const f32 h = QuadratureNode(k, n);
                const f32 sinGammaT = std::clamp(h / safeEta, -1.0f, 1.0f);
                const f32 cosGammaT = SafeSqrt(1.0f - Sqr(sinGammaT));
                const glm::vec3 transmittance = glm::vec3(std::exp(-sigma * 2.0f * cosGammaT));
                const std::array<glm::vec3, kGroomFibreLobeCount> ap = Attenuations(1.0f, safeEta, h, transmittance);
                for (u32 p = 0; p < kGroomFibreLobeCount; ++p)
                {
                    total += ap[p].r;
                }
            }
            return total / static_cast<f32>(n);
        };

        glm::vec3 sigma(0.0f);
        for (int channel = 0; channel < 3; ++channel)
        {
            const f32 target = std::clamp(std::isfinite(color[channel]) ? color[channel] : 0.0f, 0.0f, 1.0f);

            // The floor: with everything absorbed, what is left is the surface
            // reflection, which no absorption can remove. Saturate rather than
            // bisect towards something unreachable.
            if (target <= albedo(GroomFibreLimits::MaxAbsorption))
            {
                sigma[channel] = GroomFibreLimits::MaxAbsorption;
                continue;
            }
            if (target >= 1.0f)
            {
                sigma[channel] = 0.0f;
                continue;
            }

            // Monotone decreasing in sigma, so plain bisection. 20 iterations
            // over [0, 32] resolves sigma to 3e-5, far finer than the 8-bit
            // colour that asked for it.
            f32 lo = 0.0f;
            f32 hi = GroomFibreLimits::MaxAbsorption;
            for (int iteration = 0; iteration < 20; ++iteration)
            {
                const f32 mid = 0.5f * (lo + hi);
                if (albedo(mid) > target)
                {
                    lo = mid;
                }
                else
                {
                    hi = mid;
                }
            }
            sigma[channel] = 0.5f * (lo + hi);
        }
        return sigma;
    }

    GroomFibreParams MakeGroomFibreParams(const GroomFibreAuthoring& authoredIn) noexcept
    {
        const GroomFibreAuthoring authored = SanitizeGroomFibreAuthoring(authoredIn);

        GroomFibreParams params;
        switch (authored.PigmentMode)
        {
            case GroomFibrePigmentMode::Melanin:
                params.SigmaA = GroomFibreSigmaAFromMelanin(authored.Eumelanin, authored.Pheomelanin);
                break;
            case GroomFibrePigmentMode::BaseColor:
                // Inverted against THIS renderer's albedo, not through
                // Chiang's assembly fit — see GroomFibreSigmaAForAlbedo for
                // why, and for what the fit would have done instead.
                params.SigmaA =
                    GroomFibreSigmaAForAlbedo(authored.BaseColor, authored.IndexOfRefraction, authored.HSamples);
                break;
            case GroomFibrePigmentMode::Absorption:
            case GroomFibrePigmentMode::Count:
                params.SigmaA = authored.Absorption;
                break;
        }
        params.SigmaA = glm::clamp(params.SigmaA, glm::vec3(0.0f), glm::vec3(GroomFibreLimits::MaxAbsorption));

        // Chiang et al. 2016, eq. 7: the fit from an artist-facing [0,1]
        // roughness to the longitudinal variance. The beta^20 term is what
        // makes the top of the slider reach a genuinely diffuse fibre instead
        // of merely a blurry highlight.
        const f32 bm = authored.LongitudinalRoughness;
        const f32 bn = authored.AzimuthalRoughness;
        params.V[0] = Sqr(0.726f * bm + (0.812f * Sqr(bm)) + (3.7f * std::pow(bm, 20.0f)));
        // Marschner's MEASURED lobe-width ratios: TT is half the width of R and
        // TRT is twice it, so the variances are a quarter and four times.
        params.V[1] = 0.25f * params.V[0];
        params.V[2] = 4.0f * params.V[0];
        params.V[3] = params.V[2];

        params.S = kSqrtPiOver8 * ((0.265f * bn) + (1.194f * Sqr(bn)) + (5.372f * std::pow(bn, 22.0f)));

        const f32 alpha = glm::radians(authored.TiltDegrees);
        params.Sin2kAlpha[0] = std::sin(alpha);
        params.Cos2kAlpha[0] = SafeSqrt(1.0f - Sqr(params.Sin2kAlpha[0]));
        for (u32 i = 1; i < 3; ++i)
        {
            params.Sin2kAlpha[i] = 2.0f * params.Cos2kAlpha[i - 1] * params.Sin2kAlpha[i - 1];
            params.Cos2kAlpha[i] = Sqr(params.Cos2kAlpha[i - 1]) - Sqr(params.Sin2kAlpha[i - 1]);
        }

        params.Eta = authored.IndexOfRefraction;
        params.Intensity = authored.Intensity;
        params.HSamples = authored.HSamples;
        return params;
    }

    // -------------------------------------------------------------------------
    // Evaluation
    // -------------------------------------------------------------------------

    // THE NEAR FIELD IS A PLAIN POINT EVALUATION, with the model's own
    // azimuthal scale and no node widening. That is not an omission: the
    // sampling routine and the pdf both call through here, and widening the
    // density they invert would stop the triple matching itself. The widening
    // belongs to the QUADRATURE, which is why it lives in GroomFibreEvaluateFar
    // and nowhere else.
    GroomFibreLobeSet GroomFibreEvaluateNear(const GroomFibreParams& params, f32 sinThetaO, f32 sinThetaI, f32 phi,
                                             f32 h) noexcept
    {
        GroomFibreLobeSet out;

        sinThetaO = std::clamp(sinThetaO, -1.0f, 1.0f);
        sinThetaI = std::clamp(sinThetaI, -1.0f, 1.0f);
        h = std::clamp(h, -1.0f, 1.0f);

        const FibreGeometry geo = MakeGeometry(params, sinThetaO, sinThetaI, h);
        const std::array<glm::vec3, kGroomFibreLobeCount> ap =
            Attenuations(geo.CosThetaO, params.Eta, h, geo.Transmittance);

        for (u32 p = 0; p + 1 < kGroomFibreLobeCount; ++p)
        {
            f32 sinThetaOp = sinThetaO;
            f32 cosThetaOp = geo.CosThetaO;
            ApplyTilt(params, p, sinThetaO, geo.CosThetaO, sinThetaOp, cosThetaOp);

            const f32 m = LongitudinalM(geo.CosThetaI, cosThetaOp, sinThetaI, sinThetaOp, params.V[p]);
            const f32 n = AzimuthalN(phi, p, params.S, geo.GammaO, geo.GammaT);
            out.Lobe[p] = ap[p] * (m * n);
        }

        // The residual is isotropic in azimuth by construction: it is the sum
        // of every path with three or more internal bounces, and those have no
        // shared exit direction left to speak of.
        const u32 last = kGroomFibreLobeCount - 1;
        const f32 mLast = LongitudinalM(geo.CosThetaI, geo.CosThetaO, sinThetaI, sinThetaO, params.V[last]);
        out.Lobe[last] = ap[last] * (mLast * kInvTwoPi);

        return out;
    }

    GroomFibreLobeSet GroomFibreEvaluateFar(const GroomFibreParams& params, f32 sinThetaO, f32 sinThetaI, f32 phi,
                                            u32 hSamples, GroomFibreQuadrature rule) noexcept
    {
        const u32 n = std::clamp(hSamples, GroomFibreLimits::MinHSamples, GroomFibreLimits::ReferenceHSamples);

        const f32 nodeWidth = (rule == GroomFibreQuadrature::NodeWidened) ? (2.0f / static_cast<f32>(n)) : 0.0f;

        sinThetaO = std::clamp(sinThetaO, -1.0f, 1.0f);
        sinThetaI = std::clamp(sinThetaI, -1.0f, 1.0f);

        // THE LONGITUDINAL TERM IS HOISTED OUT OF THE QUADRATURE, and that is
        // not a micro-optimisation: M depends on the two longitudinal angles
        // and the lobe's variance, and on NOTHING that varies with h. Left
        // inside the loop it costs N Bessel evaluations per lobe instead of
        // one, which on the GPU is the difference between a four-tap rule
        // being affordable and not. Written as a separate function from
        // EvaluateNearImpl rather than shared with it, because the near-field
        // form must stay a plain point evaluation for the sampling triple.
        const f32 cosThetaO = SafeSqrt(1.0f - Sqr(sinThetaO));
        const f32 cosThetaI = SafeSqrt(1.0f - Sqr(sinThetaI));

        std::array<f32, kGroomFibreLobeCount> longitudinal{};
        for (u32 p = 0; p < kGroomFibreLobeCount; ++p)
        {
            f32 sinThetaOp = sinThetaO;
            f32 cosThetaOp = cosThetaO;
            ApplyTilt(params, p, sinThetaO, cosThetaO, sinThetaOp, cosThetaOp);
            longitudinal[p] = LongitudinalM(cosThetaI, cosThetaOp, sinThetaI, sinThetaOp, params.V[p]);
        }

        // etaPrime is a function of sinThetaO alone, so it too leaves the loop.
        const f32 etaPrime = SafeSqrt(Sqr(params.Eta) - Sqr(sinThetaO)) / std::max(cosThetaO, 1.0e-5f);
        const f32 sinThetaT = sinThetaO / params.Eta;
        const f32 cosThetaT = SafeSqrt(1.0f - Sqr(sinThetaT));

        std::array<glm::vec3, kGroomFibreLobeCount> azimuthal{};
        for (u32 k = 0; k < n; ++k)
        {
            const f32 h = QuadratureNode(k, n);
            const f32 gammaO = SafeASin(h);
            const f32 sinGammaT = std::clamp(h / etaPrime, -1.0f, 1.0f);
            const f32 cosGammaT = SafeSqrt(1.0f - Sqr(sinGammaT));
            const f32 gammaT = SafeASin(sinGammaT);

            const glm::vec3 transmittance =
                glm::exp(-params.SigmaA * ((2.0f * cosGammaT) / std::max(cosThetaT, 1.0e-5f)));
            const std::array<glm::vec3, kGroomFibreLobeCount> ap =
                Attenuations(cosThetaO, params.Eta, h, transmittance);

            for (u32 p = 0; p + 1 < kGroomFibreLobeCount; ++p)
            {
                const f32 s = (nodeWidth > 0.0f) ? NodeWidenedScale(params.S, p, h, etaPrime, nodeWidth) : params.S;
                azimuthal[p] += ap[p] * AzimuthalN(phi, p, s, gammaO, gammaT);
            }
            azimuthal[kGroomFibreLobeCount - 1] += ap[kGroomFibreLobeCount - 1] * kInvTwoPi;
        }

        const f32 weight = 1.0f / static_cast<f32>(n);
        GroomFibreLobeSet out;
        for (u32 p = 0; p < kGroomFibreLobeCount; ++p)
        {
            out.Lobe[p] = azimuthal[p] * (longitudinal[p] * weight);
        }
        return out;
    }

    GroomFibreLobeSet GroomFibreEvaluateFar(const GroomFibreParams& params, f32 sinThetaO, f32 sinThetaI,
                                            f32 phi) noexcept
    {
        return GroomFibreEvaluateFar(params, sinThetaO, sinThetaI, phi, params.HSamples,
                                     GroomFibreQuadrature::NodeWidened);
    }

    GroomFibreLobeSet GroomFibreEvaluateReference(const GroomFibreParams& params, f32 sinThetaO, f32 sinThetaI,
                                                  f32 phi) noexcept
    {
        return GroomFibreEvaluateFar(params, sinThetaO, sinThetaI, phi, GroomFibreLimits::ReferenceHSamples,
                                     GroomFibreQuadrature::PointSampled);
    }

    GroomFibreLobeSet GroomFibreEvaluateFar(const GroomFibreParams& params, const glm::vec3& tangent,
                                            const glm::vec3& wo, const glm::vec3& wi) noexcept
    {
        const f32 tangentLength = glm::length(tangent);
        if (!(tangentLength > 1.0e-8f))
        {
            return GroomFibreLobeSet{};
        }
        const glm::vec3 t = tangent / tangentLength;

        const f32 sinThetaO = std::clamp(glm::dot(t, wo), -1.0f, 1.0f);
        const f32 sinThetaI = std::clamp(glm::dot(t, wi), -1.0f, 1.0f);

        // The azimuth DIFFERENCE, measured in the plane perpendicular to the
        // fibre. Only the difference is needed, which is what frees the model
        // from wanting a binormal the ribbon does not have.
        const glm::vec3 perpO = wo - (t * sinThetaO);
        const glm::vec3 perpI = wi - (t * sinThetaI);
        const f32 lenO = glm::length(perpO);
        const f32 lenI = glm::length(perpI);

        // A direction exactly along the fibre has no azimuth at all. Returning
        // phi = 0 is the continuous limit and keeps a NaN out of the frame; the
        // longitudinal term is already ~0 there, so the choice is unobservable.
        const f32 phi = (lenO > 1.0e-6f && lenI > 1.0e-6f)
                            ? std::acos(std::clamp(glm::dot(perpO, perpI) / (lenO * lenI), -1.0f, 1.0f))
                            : 0.0f;

        return GroomFibreEvaluateFar(params, sinThetaO, sinThetaI, phi, params.HSamples);
    }

    GroomFibreLobeSet GroomFibreAmbientResponse(const GroomFibreParams& params, f32 sinThetaO) noexcept
    {
        sinThetaO = std::clamp(sinThetaO, -1.0f, 1.0f);
        const u32 n = std::clamp(params.HSamples, GroomFibreLimits::MinHSamples, GroomFibreLimits::ReferenceHSamples);

        const f32 cosThetaO = SafeSqrt(1.0f - Sqr(sinThetaO));
        const f32 etaPrime = SafeSqrt(Sqr(params.Eta) - Sqr(sinThetaO)) / std::max(cosThetaO, 1.0e-5f);
        const f32 sinThetaT = sinThetaO / params.Eta;
        const f32 cosThetaT = SafeSqrt(1.0f - Sqr(sinThetaT));

        GroomFibreLobeSet out;
        for (u32 k = 0; k < n; ++k)
        {
            const f32 h = QuadratureNode(k, n);
            const f32 sinGammaT = std::clamp(h / etaPrime, -1.0f, 1.0f);
            const f32 cosGammaT = SafeSqrt(1.0f - Sqr(sinGammaT));
            const glm::vec3 transmittance =
                glm::exp(-params.SigmaA * ((2.0f * cosGammaT) / std::max(cosThetaT, 1.0e-5f)));
            const std::array<glm::vec3, kGroomFibreLobeCount> ap =
                Attenuations(cosThetaO, params.Eta, h, transmittance);
            for (u32 p = 0; p < kGroomFibreLobeCount; ++p)
            {
                out.Lobe[p] += ap[p];
            }
        }

        // The mean attenuation, times cos(theta_o). See the header for why the
        // cosine belongs here and what the measured band around it is.
        const f32 weight = cosThetaO / static_cast<f32>(n);
        for (u32 p = 0; p < kGroomFibreLobeCount; ++p)
        {
            out.Lobe[p] *= weight;
        }
        return out;
    }

    f32 GroomFibreCosineWeight(const glm::vec3& tangent, const glm::vec3& wi) noexcept
    {
        const f32 tangentLength = glm::length(tangent);
        if (!(tangentLength > 1.0e-8f))
        {
            return 0.0f;
        }
        const f32 sinTheta = std::clamp(glm::dot(tangent / tangentLength, wi), -1.0f, 1.0f);
        return SafeSqrt(1.0f - Sqr(sinTheta));
    }

    // -------------------------------------------------------------------------
    // Stochastic triple
    // -------------------------------------------------------------------------

    namespace
    {
        // The per-lobe selection probabilities: each attenuation's luminance
        // over their total. Sampling a lobe in proportion to how much energy it
        // carries is what keeps the estimator's variance finite when one lobe
        // dominates, which for a dark fibre is almost always R.
        [[nodiscard]] std::array<f32, kGroomFibreLobeCount> LobeSelectionPdf(const GroomFibreParams& params,
                                                                             f32 sinThetaO, f32 h) noexcept
        {
            const FibreGeometry geo = MakeGeometry(params, sinThetaO, sinThetaO, h);
            const std::array<glm::vec3, kGroomFibreLobeCount> ap =
                Attenuations(geo.CosThetaO, params.Eta, h, geo.Transmittance);

            std::array<f32, kGroomFibreLobeCount> pdf{};
            f32 total = 0.0f;
            for (u32 p = 0; p < kGroomFibreLobeCount; ++p)
            {
                // Rec. 709 luminance, the same weighting the rest of the
                // engine's importance sampling uses.
                pdf[p] = (0.2126f * ap[p].r) + (0.7152f * ap[p].g) + (0.0722f * ap[p].b);
                total += pdf[p];
            }
            if (total <= 0.0f)
            {
                pdf.fill(1.0f / static_cast<f32>(kGroomFibreLobeCount));
                return pdf;
            }
            for (u32 p = 0; p < kGroomFibreLobeCount; ++p)
            {
                pdf[p] /= total;
            }
            return pdf;
        }
    } // namespace

    GroomFibreSampleResult GroomFibreSample(const GroomFibreParams& params, const glm::vec3& wo, f32 h,
                                            const glm::vec4& u) noexcept
    {
        GroomFibreSampleResult result;

        h = std::clamp(h, -1.0f, 1.0f);
        const f32 sinThetaO = std::clamp(wo.x, -1.0f, 1.0f);
        const f32 cosThetaO = SafeSqrt(1.0f - Sqr(sinThetaO));
        const f32 phiO = std::atan2(wo.z, wo.y);

        const std::array<f32, kGroomFibreLobeCount> selection = LobeSelectionPdf(params, sinThetaO, h);

        u32 p = kGroomFibreLobeCount - 1;
        f32 uLobe = std::clamp(u.x, 0.0f, 1.0f - 1.0e-6f);
        for (u32 candidate = 0; candidate + 1 < kGroomFibreLobeCount; ++candidate)
        {
            if (uLobe < selection[candidate])
            {
                p = candidate;
                break;
            }
            uLobe -= selection[candidate];
        }

        f32 sinThetaOp = sinThetaO;
        f32 cosThetaOp = cosThetaO;
        ApplyTilt(params, p, sinThetaO, cosThetaO, sinThetaOp, cosThetaOp);

        // Inverting the longitudinal lobe. The u -> cosTheta map is the exact
        // inverse of the v-parameterised distribution M integrates, so the
        // longitudinal half of the density cancels exactly rather than
        // approximately.
        const f32 uM = std::max(std::clamp(u.y, 0.0f, 1.0f), 1.0e-5f);
        const f32 v = params.V[p];
        const f32 cosTheta = 1.0f + (v * std::log(uM + ((1.0f - uM) * std::exp(-2.0f / v))));
        const f32 sinTheta = SafeSqrt(1.0f - Sqr(cosTheta));
        const f32 cosPhiJitter = std::cos(kTwoPi * std::clamp(u.z, 0.0f, 1.0f));

        const f32 sinThetaI = std::clamp((-cosTheta * sinThetaOp) + (sinTheta * cosPhiJitter * cosThetaOp), -1.0f, 1.0f);
        const f32 cosThetaI = SafeSqrt(1.0f - Sqr(sinThetaI));

        const FibreGeometry geo = MakeGeometry(params, sinThetaO, sinThetaI, h);

        const f32 uN = std::clamp(u.w, 1.0e-6f, 1.0f - 1.0e-6f);
        const f32 dphi = (p + 1 < kGroomFibreLobeCount)
                             ? LobePhi(p, geo.GammaO, geo.GammaT) + SampleTrimmedLogistic(uN, params.S)
                             : kTwoPi * uN;

        const f32 phiI = phiO + dphi;
        result.Wi = glm::vec3(sinThetaI, cosThetaI * std::cos(phiI), cosThetaI * std::sin(phiI));
        result.Lobe = static_cast<GroomFibreLobe>(p);
        result.Pdf = GroomFibrePdf(params, wo, result.Wi, h);
        result.Value = GroomFibreEvaluateNear(params, sinThetaO, sinThetaI, dphi, h);
        result.Valid = result.Pdf > 0.0f;
        return result;
    }

    f32 GroomFibrePdf(const GroomFibreParams& params, const glm::vec3& wo, const glm::vec3& wi, f32 h) noexcept
    {
        h = std::clamp(h, -1.0f, 1.0f);
        const f32 sinThetaO = std::clamp(wo.x, -1.0f, 1.0f);
        const f32 cosThetaO = SafeSqrt(1.0f - Sqr(sinThetaO));
        const f32 sinThetaI = std::clamp(wi.x, -1.0f, 1.0f);
        const f32 cosThetaI = SafeSqrt(1.0f - Sqr(sinThetaI));

        const f32 phi = std::atan2(wi.z, wi.y) - std::atan2(wo.z, wo.y);

        const FibreGeometry geo = MakeGeometry(params, sinThetaO, sinThetaI, h);
        const std::array<f32, kGroomFibreLobeCount> selection = LobeSelectionPdf(params, sinThetaO, h);

        f32 pdf = 0.0f;
        for (u32 p = 0; p + 1 < kGroomFibreLobeCount; ++p)
        {
            f32 sinThetaOp = sinThetaO;
            f32 cosThetaOp = cosThetaO;
            ApplyTilt(params, p, sinThetaO, cosThetaO, sinThetaOp, cosThetaOp);
            pdf += LongitudinalM(cosThetaI, cosThetaOp, sinThetaI, sinThetaOp, params.V[p]) * selection[p] *
                   AzimuthalN(phi, p, params.S, geo.GammaO, geo.GammaT);
        }

        const u32 last = kGroomFibreLobeCount - 1;
        pdf += LongitudinalM(cosThetaI, cosThetaO, sinThetaI, sinThetaO, params.V[last]) * selection[last] * kInvTwoPi;
        return pdf;
    }

    // -------------------------------------------------------------------------
    // Energy
    // -------------------------------------------------------------------------

    glm::vec3 GroomFibreWhiteFurnace(const GroomFibreParams& params, f32 sinThetaO, u32 thetaSteps,
                                     u32 phiSteps) noexcept
    {
        thetaSteps = std::max(thetaSteps, 1u);
        phiSteps = std::max(phiSteps, 1u);

        // dOmega = cos(theta) dTheta dPhi, with theta measured from the plane
        // perpendicular to the fibre — the fibre convention, not the surface
        // one. Writing the measure out is the point: using sin(theta) here is
        // the single most common way to "discover" that a correct fibre model
        // does not conserve energy.
        const f32 dTheta = kPi / static_cast<f32>(thetaSteps);
        const f32 dPhi = kTwoPi / static_cast<f32>(phiSteps);

        glm::vec3 total(0.0f);
        for (u32 ti = 0; ti < thetaSteps; ++ti)
        {
            const f32 theta = (-0.5f * kPi) + (dTheta * (static_cast<f32>(ti) + 0.5f));
            const f32 sinThetaI = std::sin(theta);
            const f32 cosThetaI = std::cos(theta);
            for (u32 pi = 0; pi < phiSteps; ++pi)
            {
                const f32 phi = dPhi * (static_cast<f32>(pi) + 0.5f);
                const GroomFibreLobeSet lobes = GroomFibreEvaluateFar(params, sinThetaO, sinThetaI, phi);
                total += lobes.Sum() * (cosThetaI * dTheta * dPhi);
            }
        }
        return total;
    }

    glm::vec3 GroomFibreSampledFurnace(const GroomFibreParams& params, f32 sinThetaO, f32 h, u32 sampleCount,
                                       u32 seed) noexcept
    {
        sampleCount = std::max(sampleCount, 1u);

        const f32 cosThetaO = SafeSqrt(1.0f - Sqr(sinThetaO));
        const glm::vec3 wo(sinThetaO, cosThetaO, 0.0f);

        glm::vec3 total(0.0f);
        for (u32 i = 0; i < sampleCount; ++i)
        {
            const glm::vec4 u(FurnaceUniform(i, 0, seed), FurnaceUniform(i, 1, seed), FurnaceUniform(i, 2, seed),
                              FurnaceUniform(i, 3, seed));
            const GroomFibreSampleResult sample = GroomFibreSample(params, wo, h, u);
            if (!sample.Valid)
            {
                continue;
            }
            total += sample.Value.Sum() / sample.Pdf;
        }
        return total / static_cast<f32>(sampleCount);
    }
} // namespace OloEngine
