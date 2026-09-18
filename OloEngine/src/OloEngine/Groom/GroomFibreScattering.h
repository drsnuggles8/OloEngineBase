#pragma once

// =============================================================================
// GroomFibreScattering.h — the fibre BCSDF behind #1247, and the arithmetic the
// GPU strand shader has to agree with.
//
// WHAT A FIBRE SCATTERING MODEL IS. A hair or fur fibre is a dielectric
// cylinder with an absorbing interior, not a surface. Light reaching it takes
// one of a small number of paths through the cross-section, and each path
// leaves in its own direction with its own colour:
//
//   R        reflected off the cuticle without entering. Uncoloured — it is a
//            surface reflection, so it carries the LIGHT's colour, not the
//            hair's. This is why even black hair has a white sheen.
//   TT       refracted in, straight out the far side. Absorbed ONCE along the
//            chord, so it is the most saturated lobe and it fires when the
//            light is BEHIND the fibre. The whole visual character of pale
//            hair is TT.
//   TRT      in, reflected off the far wall, back out. Absorbed twice. The
//            secondary highlight, offset from R along the fibre, and the lobe
//            that makes blonde hair glow rather than merely being bright.
//   residual every path with three or more internal bounces, folded into one
//            isotropic term so the four attenuations sum to exactly one.
//
// THE MODEL IS CHIANG ET AL. 2016 ("A Practical and Controllable Hair and Fur
// Model for Production Path Rendering"), the same decomposition pbrt-v3 §"Hair
// Scattering" implements, with two properties that decided it over the cheaper
// closed-form fits:
//
//   * It has a matching evaluate / sample / pdf triple, so a stochastic
//     estimator can consume it without an independently written density — the
//     failure PBRClosureBSDF.h and LightSampling.glsl both name.
//   * Its attenuations are energy-conserving BY CONSTRUCTION (see
//     GroomFibreAttenuation), which is what lets criterion 4 be a test rather
//     than a claim.
//
// FAR FIELD, NOT NEAR FIELD, AND THAT IS FORCED BY #1246. The near-field model
// takes `h`, the offset of the ray across the fibre's width in [-1, 1]. A
// strand in this engine is rasterised as a ONE-PIXEL-WIDE ribbon whatever its
// true width (groom-strand-visibility.md rule 2), so the across-ribbon
// coordinate is a coordinate on the WIDENED quad and is not h. Integrating h
// out is therefore not an optimisation, it is the only honest reading of the
// geometry — and it buys a second thing for free: the far-field response
// depends on the azimuth DIFFERENCE alone, so it needs the strand TANGENT and
// no binormal. A ribbon has no meaningful binormal, so a model that needed one
// would have to invent it per frame.
//
// THE QUADRATURE IS THE APPROXIMATION, AND IT IS THE ONE THAT WAS MEASURED.
// GroomFibreEvaluateFar integrates over h with N uniform midpoint samples.
// Uniform rather than Gauss-Legendre on purpose: the near-field azimuthal term
// has caustic peaks whose position moves with h, so the integrand is not
// smooth and a high-order rule buys nothing over a stratified one. N is the
// single knob that trades cost against error, both sides of the CPU/GPU twin
// use the same nodes, and docs/analysis/groom-fibre-scattering-1247.md records
// the error-versus-N table that picked the shipped value.
//
// EVERYTHING HERE HAS A GLSL TWIN in OloEditor/assets/shaders/include/
// GroomFibreCommon.glsl, and GroomFibreGpuParityTest renders the twin over a
// grid of angles and compares texel for texel. Same contract as #1246's
// GroomStrandCommon.glsl: the numbers in the analysis are computed by THIS
// side, so a shader that scattered differently would make every measured
// number a measurement of something that is not on screen. Unlike #1246's
// integer hash this is floating point, so the parity test asks for a relative
// tolerance rather than exact equality — stated as a number in that test, not
// left to a reader to assume.
//
// CPU-only and context-free, so the comparison re-runs in CI.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <array>
#include <string_view>

namespace OloEngine
{
    // The scattering paths, in the order every array here is indexed.
    // R/TT/TRT are Marschner's names and are kept; Residual is everything from
    // TRRT onwards, which has no established name because no model before
    // Chiang's gave it one.
    enum class GroomFibreLobe : u8
    {
        R = 0,
        TT = 1,
        TRT = 2,
        Residual = 3,

        Count
    };

    inline constexpr u32 kGroomFibreLobeCount = static_cast<u32>(GroomFibreLobe::Count);

    [[nodiscard]] constexpr std::string_view ToString(GroomFibreLobe lobe) noexcept
    {
        switch (lobe)
        {
            case GroomFibreLobe::R:
                return "R";
            case GroomFibreLobe::TT:
                return "TT";
            case GroomFibreLobe::TRT:
                return "TRT";
            case GroomFibreLobe::Residual:
                return "Residual";
            case GroomFibreLobe::Count:
                break;
        }
        return "R";
    }

    // How the authored colour of a fibre becomes an absorption coefficient.
    // Append, never renumber — the number reaches scene YAML and save games.
    enum class GroomFibrePigmentMode : u8
    {
        // MEASURED PIGMENT. Eumelanin and pheomelanin concentrations against
        // the absorption spectra measured by d'Eon et al. 2011, which are the
        // numbers pbrt and every production hair shader use. This is the mode
        // with a physical meaning: "dark hair" is a eumelanin concentration,
        // not a colour swatch, and the hue difference between black, brown and
        // red hair is the RATIO of the two pigments.
        Melanin = 0,

        // AUTHORED REFLECTANCE. A colour the artist picks, inverted through
        // Chiang's fit to the absorption that REPRODUCES that colour once
        // multiple scattering has had its say. See GroomFibreSigmaAFromColor
        // for why that inversion is the multiple-scattering approximation this
        // slice declares.
        BaseColor = 1,

        // RAW ABSORPTION, in the model's own units (per fibre diameter). The
        // escape hatch for matching a reference render or a measured fibre;
        // not an authoring mode.
        Absorption = 2,

        Count
    };

    [[nodiscard]] inline constexpr bool IsValidGroomFibrePigmentMode(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomFibrePigmentMode::Count);
    }

    // Which contribution the strand pass renders. Full is the material; the
    // rest are the separated diagnostic contributions acceptance criterion 3
    // asks for, and they are how the dark-versus-pale difference is debugged —
    // a pale coat that looks wrong is almost always a TT that is too dim or a
    // TRT that is too bright, and the sum cannot tell you which.
    enum class GroomFibreDebugMode : u8
    {
        Full = 0,
        LobeR = 1,
        LobeTT = 2,
        LobeTRT = 3,
        LobeResidual = 4,
        // The world-space strand tangent, remapped to [0,1]. The tangent frame
        // is the one input every lobe shares, so a coat that shades wrong
        // everywhere is checked here first.
        Tangent = 5,

        Count
    };

    [[nodiscard]] inline constexpr bool IsValidGroomFibreDebugMode(i32 value) noexcept
    {
        return value >= 0 && value < static_cast<i32>(GroomFibreDebugMode::Count);
    }

    // The MEASURED absorption spectra of the two hair pigments, per unit
    // concentration, in the model's per-fibre-diameter units. d'Eon et al.
    // 2011, "An Energy-Conserving Hair Reflectance Model", table 1 — the same
    // constants pbrt-v3 ships in SigmaAFromConcentration.
    //
    // The shape of the eumelanin spectrum is the whole reason dark hair reads
    // dark rather than grey: it absorbs blue three times as hard as red, so a
    // high concentration does not merely darken, it also drags the residual
    // transmission towards red. Pheomelanin is flatter and weaker, which is
    // why red hair stays saturated at concentrations that would make a
    // eumelanin fibre black.
    inline constexpr glm::vec3 kGroomEumelaninAbsorption{ 0.419f, 0.697f, 1.37f };
    inline constexpr glm::vec3 kGroomPheomelaninAbsorption{ 0.187f, 0.4f, 1.05f };

    // Marschner's measured fibre constants, used as the authoring defaults.
    // The index of refraction of keratin is 1.55; the cuticle scales tilt by
    // 2-3 degrees, which is what separates the R and TRT highlights on a real
    // head of hair and the reason a zero tilt looks like plastic.
    inline constexpr f32 kGroomFibreDefaultIOR = 1.55f;
    inline constexpr f32 kGroomFibreDefaultTiltDegrees = 2.0f;

    // Bounds every authored value is validated into. They are not taste: each
    // one is a value at or past which the model stops being defined.
    namespace GroomFibreLimits
    {
        // eta == 1 is no interface at all: the Bravais index collapses, every
        // refraction angle is the incidence angle and TT/TRT lose their
        // meaning. The upper bound is far past any keratin.
        inline constexpr f32 MinIOR = 1.01f;
        inline constexpr f32 MaxIOR = 3.0f;

        // Roughness 0 is a delta lobe the quadrature cannot resolve and the
        // shader would render as aliased sparkle; 1 is fully diffuse.
        inline constexpr f32 MinRoughness = 0.01f;
        inline constexpr f32 MaxRoughness = 1.0f;

        inline constexpr f32 MaxTiltDegrees = 15.0f;

        inline constexpr f32 MaxConcentration = 8.0f;
        // Absorption is dimensionless (per fibre diameter); 32 already
        // transmits less than e^-64 and is black in every channel.
        inline constexpr f32 MaxAbsorption = 32.0f;

        inline constexpr f32 MaxIntensity = 64.0f;

        // The h-quadrature order. 1 is the degenerate centre-of-fibre sample
        // that every near-field model reduces to; the upper bound is what the
        // shader's loop is unrolled to and what the analysis measured against.
        inline constexpr u32 MinHSamples = 1;
        inline constexpr u32 MaxHSamples = 32;
        // The reference order the analysis treats as ground truth.
        inline constexpr u32 ReferenceHSamples = 256;
    } // namespace GroomFibreLimits

    // ------------------------------------------------------------------------
    // Authoring
    // ------------------------------------------------------------------------
    // What a human sets. Deliberately NOT the component: the renderer, the
    // tests and the analysis all need these numbers without dragging in the
    // ECS, and GroomFibreComponent converts into this in one place
    // (MakeGroomFibreAuthoring, Scene/Components.h).
    struct GroomFibreAuthoring
    {
        GroomFibrePigmentMode PigmentMode = GroomFibrePigmentMode::Melanin;

        /// Melanin mode. 1.3 / 0.0 is dark brown; 0.1 / 0.05 is a pale blonde;
        /// 0.35 / 1.4 is red. The numbers are concentrations against the
        /// spectra above, so they are comparable with published hair data
        /// rather than being an arbitrary 0-1 slider.
        f32 Eumelanin = 1.3f;
        f32 Pheomelanin = 0.0f;

        /// BaseColor mode. The reflectance the coat should END UP with.
        glm::vec3 BaseColor{ 0.42f, 0.26f, 0.14f };

        /// Absorption mode. sigma_a directly.
        glm::vec3 Absorption{ 0.84f, 1.39f, 2.74f };

        /// Longitudinal roughness (Marschner's beta_M): how far the highlight
        /// smears ALONG the fibre. Drives the width of every M lobe.
        f32 LongitudinalRoughness = 0.3f;

        /// Azimuthal roughness (beta_N): how far each lobe smears AROUND the
        /// fibre. Drives the azimuthal logistic's width, and — through the
        /// colour inversion — how much multiple scattering the BaseColor mode
        /// assumes.
        f32 AzimuthalRoughness = 0.3f;

        /// Cuticle scale tilt in degrees. Shifts R one way and TRT the other,
        /// which is what separates the two highlights.
        f32 TiltDegrees = kGroomFibreDefaultTiltDegrees;

        f32 IndexOfRefraction = kGroomFibreDefaultIOR;

        /// A plain multiplier on the whole response. Not part of the physics —
        /// an exposure lever for matching a coat into a scene — so it is
        /// applied AFTER the energy check rather than inside it.
        f32 Intensity = 1.0f;

        /// Quadrature order. See the file header.
        u32 HSamples = 4;

        [[nodiscard]] bool operator==(const GroomFibreAuthoring&) const = default;
    };

    // ------------------------------------------------------------------------
    // Derived parameters
    // ------------------------------------------------------------------------
    // Everything the evaluation needs, precomputed once per groom. This is the
    // struct the UBO mirrors: deriving on the GPU instead would put a sin, a
    // pow and a logarithm per fragment in the way of parity, and #1246's twin
    // contract is that the two sides agree on VALUES, not on the recipe.
    struct GroomFibreParams
    {
        /// Absorption per fibre diameter, whatever authoring mode produced it.
        glm::vec3 SigmaA{ 0.0f };

        /// Longitudinal variances, one per lobe. v[TT] = v[R]/4 and
        /// v[TRT] = 4 v[R] are Marschner's measured lobe-width ratios; the
        /// residual reuses the TRT width because it has no measured one.
        std::array<f32, kGroomFibreLobeCount> V{ 0.0f, 0.0f, 0.0f, 0.0f };

        /// Azimuthal logistic scale.
        f32 S = 0.0f;

        /// sin/cos of 2^k * tilt, k = 0,1,2 — the per-lobe tilt rotation,
        /// built by angle doubling so the three share one sin/cos.
        std::array<f32, 3> Sin2kAlpha{ 0.0f, 0.0f, 0.0f };
        std::array<f32, 3> Cos2kAlpha{ 1.0f, 1.0f, 1.0f };

        f32 Eta = kGroomFibreDefaultIOR;
        f32 Intensity = 1.0f;
        u32 HSamples = 4;

        [[nodiscard]] bool operator==(const GroomFibreParams&) const = default;
    };

    /// The four lobes, kept apart. Callers that want the material sum call
    /// Sum(); the strand pass keeps them separate so a debug mode can render
    /// one, which is acceptance criterion 3's "separated diagnostic
    /// contributions".
    struct GroomFibreLobeSet
    {
        std::array<glm::vec3, kGroomFibreLobeCount> Lobe{ glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f),
                                                          glm::vec3(0.0f) };

        [[nodiscard]] glm::vec3 Sum() const noexcept
        {
            return Lobe[0] + Lobe[1] + Lobe[2] + Lobe[3];
        }

        [[nodiscard]] const glm::vec3& operator[](GroomFibreLobe lobe) const noexcept
        {
            return Lobe[static_cast<u32>(lobe)];
        }
        [[nodiscard]] glm::vec3& operator[](GroomFibreLobe lobe) noexcept
        {
            return Lobe[static_cast<u32>(lobe)];
        }
    };

    // ------------------------------------------------------------------------
    // Parameter construction
    // ------------------------------------------------------------------------

    /// Clamps every field into GroomFibreLimits and replaces every non-finite
    /// one with its default. Authoring reaches here from scene YAML, save
    /// games and the MCP write path, so this is the one gate all three pass.
    [[nodiscard]] GroomFibreAuthoring SanitizeGroomFibreAuthoring(const GroomFibreAuthoring& authored) noexcept;

    /// Measured pigment concentrations -> sigma_a.
    [[nodiscard]] glm::vec3 GroomFibreSigmaAFromMelanin(f32 eumelanin, f32 pheomelanin) noexcept;

    /// Chiang et al. 2016 eq. 9: the sigma_a whose MULTIPLY-SCATTERED
    /// appearance is `color`, given the azimuthal roughness.
    ///
    /// EXPOSED, BUT NOT WHAT THE BaseColor MODE USES, and the difference is
    /// the point. That fit answers "what absorption makes a fibre ASSEMBLY,
    /// at the density the fit was made against, look like this colour" — it
    /// has inter-fibre multiple scattering baked into it. Nothing in this
    /// slice transports light between fibres (#1248 owns that), so using it
    /// directly makes a coat authored at 0.1 render at 0.76: the absorption it
    /// asks for is low precisely because it expects the neighbours to do the
    /// darkening, and there are no neighbours.
    ///
    /// It is kept callable because it is the right answer once #1248 lands,
    /// because it is the published constant, and because
    /// GroomFibreColourInversionTest measures the gap rather than describing
    /// it. Today it serves as the starting guess for the inversion below.
    [[nodiscard]] glm::vec3 GroomFibreSigmaAFromColor(const glm::vec3& color, f32 azimuthalRoughness) noexcept;

    /// The absorption that makes a SINGLE FIBRE's head-on albedo equal
    /// `color` — the inversion the BaseColor mode actually uses, so an
    /// authored colour is the colour that renders.
    ///
    /// Solved by bisection against the model's own albedo rather than by a
    /// fit, because the albedo is exactly what GroomFibreAmbientResponse
    /// computes and a closed form would be a second, drifting description of
    /// it. Twenty iterations over a monotone function; it runs once per groom,
    /// not per fragment.
    ///
    /// A FIBRE CANNOT BE DARKER THAN ITS OWN SURFACE REFLECTION. R never
    /// enters the fibre, so no absorption removes it, and a target below that
    /// floor (around 0.04 at these indices) saturates at the maximum
    /// absorption instead of being chased forever. That is a fact about hair —
    /// it is why black hair still has a white sheen — rather than a limitation
    /// of the solver.
    [[nodiscard]] glm::vec3 GroomFibreSigmaAForAlbedo(const glm::vec3& color, f32 eta, u32 hSamples) noexcept;

    [[nodiscard]] GroomFibreParams MakeGroomFibreParams(const GroomFibreAuthoring& authored) noexcept;

    // ------------------------------------------------------------------------
    // Evaluation
    // ------------------------------------------------------------------------

    /// The NEAR-FIELD lobes at one offset `h` across the fibre, h in [-1, 1].
    /// Angles are longitudinal: sinTheta is the component of the direction
    /// along the fibre TANGENT, so theta is measured from the plane
    /// perpendicular to the fibre (the fibre-scattering convention, not the
    /// surface one). `phi` is the azimuth DIFFERENCE between the two
    /// directions about the tangent, in radians.
    ///
    /// The value is a BCSDF: the far-field integral of it against the
    /// longitudinal cosine is the fibre's albedo, which is what
    /// GroomFibreWhiteFurnace measures.
    [[nodiscard]] GroomFibreLobeSet GroomFibreEvaluateNear(const GroomFibreParams& params, f32 sinThetaO,
                                                           f32 sinThetaI, f32 phi, f32 h) noexcept;

    /// The two h-quadrature rules the comparison weighed. Both are kept
    /// callable because the rejected one is the SHAPE of the failure: keeping
    /// it lets GroomFibreScatteringTest re-run the rejection every build
    /// instead of leaving it as a number in a document nothing defends.
    enum class GroomFibreQuadrature : u8
    {
        /// Each node's azimuthal lobe is widened to cover its own slice of h.
        /// The shipped rule — see NodeWidenedScale in the .cpp.
        NodeWidened = 0,

        /// The plain rule: N point samples of the near field, averaged. It is
        /// the obvious thing to write and it is REJECTED: at every order a
        /// fragment shader can afford it reproduces the far-field lobe as N
        /// separate spikes, over 70 % RMS away from the truth, and a coat
        /// rendered with it bands as the strand tangent turns. Correct only in
        /// the limit, which is not a limit this runs at.
        PointSampled = 1,

        Count
    };

    /// The FAR-FIELD lobes: the near field integrated over h with
    /// `params.HSamples` uniform midpoint nodes and the shipped rule. This is
    /// what the renderer evaluates and what the GLSL twin computes.
    [[nodiscard]] GroomFibreLobeSet GroomFibreEvaluateFar(const GroomFibreParams& params, f32 sinThetaO, f32 sinThetaI,
                                                          f32 phi) noexcept;

    /// The same, with an explicit quadrature order and rule — what the
    /// analysis sweeps to pick the shipped ones.
    [[nodiscard]] GroomFibreLobeSet GroomFibreEvaluateFar(
        const GroomFibreParams& params, f32 sinThetaO, f32 sinThetaI, f32 phi, u32 hSamples,
        GroomFibreQuadrature rule = GroomFibreQuadrature::NodeWidened) noexcept;

    /// The converged far field: the point-sampled rule at
    /// GroomFibreLimits::ReferenceHSamples, which is the integral both rules
    /// are approximations OF. Named rather than spelled out at each call site
    /// so a test cannot accidentally measure one approximation against another.
    [[nodiscard]] GroomFibreLobeSet GroomFibreEvaluateReference(const GroomFibreParams& params, f32 sinThetaO,
                                                                f32 sinThetaI, f32 phi) noexcept;

    /// The renderer's form: a strand tangent and two world directions.
    /// `wo` points at the eye and `wi` at the light, both unit, and neither
    /// has to be perpendicular to anything. Degenerate azimuths (a direction
    /// exactly along the tangent) fall back to phi = 0 rather than
    /// normalising a zero vector.
    [[nodiscard]] GroomFibreLobeSet GroomFibreEvaluateFar(const GroomFibreParams& params, const glm::vec3& tangent,
                                                          const glm::vec3& wo, const glm::vec3& wi) noexcept;

    /// THE ENVIRONMENT TERM, per lobe: the fibre's response to a unit uniform
    /// environment, which the caller multiplies by the environment's average
    /// radiance (an irradiance-map sample over pi).
    ///
    /// It is each path's mean attenuation over the fibre's width — that path's
    /// ALBEDO, because the longitudinal and azimuthal factors are each
    /// normalised to one, so integrating the BCSDF over the sphere leaves
    /// exactly the sum of these — times cos(theta_o). The cosine is not a
    /// fudge: the true integral carries one more factor of cos(theta_i) than
    /// the albedo does, and the scattering is concentrated on the cone where
    /// theta_i mirrors theta_o, so cos(theta_o) is that factor's mean value.
    ///
    /// It costs nothing beyond the quadrature the evaluation already runs.
    ///
    /// THE APPROXIMATION THIS DECLARES is that the environment is uniform over
    /// the directions the fibre scatters from. Measured against the true
    /// spherical integral it lands in [0.64, 1.11] of the truth across dark,
    /// pale, coloured, smooth and rough fibres at four view angles. The
    /// alternative measured first — a K-point rule around the fibre's normal
    /// circle, which is the obvious "sample the environment where the fibre
    /// scatters" idea — landed in [0.00, 11.25] and is REJECTED: at a smooth
    /// fibre's grazing angles that circle misses the scattering cone entirely
    /// and the coat's ambient goes to exactly zero. Table 5 of
    /// docs/analysis/groom-fibre-scattering-1247.md is the measurement.
    ///
    /// It uses the SAME attenuations the direct lighting uses, which is
    /// acceptance criterion 4's "scene lights and environment lighting use
    /// consistent material parameters" as an identity rather than a promise.
    [[nodiscard]] GroomFibreLobeSet GroomFibreAmbientResponse(const GroomFibreParams& params, f32 sinThetaO) noexcept;

    /// The longitudinal cosine that weights the fibre's rendering integral.
    /// Written as a named function because it is NOT the surface cosine and
    /// getting that wrong is invisible at normal incidence.
    [[nodiscard]] f32 GroomFibreCosineWeight(const glm::vec3& tangent, const glm::vec3& wi) noexcept;

    // ------------------------------------------------------------------------
    // Stochastic triple
    // ------------------------------------------------------------------------
    // Evaluate / sample / pdf, matching by construction: the sampling picks a
    // lobe with probability equal to its attenuation and then inverts that
    // lobe's own M and N, and the pdf sums the same per-lobe densities with
    // the same weights. Nothing on the GPU consumes these today — the raster
    // pass evaluates a known light direction — so they exist for two reasons
    // that are not speculative: they are how energy conservation is PROVEN
    // (GroomFibreWhiteFurnace integrates the evaluation against this density),
    // and #1248's dense-coat transport is a stochastic estimator that will
    // need exactly this triple rather than a second, subtly different one.

    struct GroomFibreSampleResult
    {
        bool Valid = false;
        /// The sampled direction, in the same tangent frame the evaluation
        /// uses: x along the tangent, y and z spanning the normal plane.
        glm::vec3 Wi{ 0.0f };
        GroomFibreLobeSet Value;
        f32 Pdf = 0.0f;
        GroomFibreLobe Lobe = GroomFibreLobe::R;
    };

    /// `wo` and the returned `Wi` are in the fibre's LOCAL frame: x along the
    /// tangent. `u` are four independent uniforms in [0, 1).
    [[nodiscard]] GroomFibreSampleResult GroomFibreSample(const GroomFibreParams& params, const glm::vec3& wo, f32 h,
                                                          const glm::vec4& u) noexcept;

    /// The density GroomFibreSample draws from, for an arbitrary direction.
    [[nodiscard]] f32 GroomFibrePdf(const GroomFibreParams& params, const glm::vec3& wo, const glm::vec3& wi,
                                    f32 h) noexcept;

    // ------------------------------------------------------------------------
    // Energy
    // ------------------------------------------------------------------------

    /// The fibre's albedo at `sinThetaO`, measured by integrating the
    /// evaluation over the whole sphere. The BCSDF is normalised so that this
    /// integral is the fraction of incident energy the fibre returns — there
    /// is deliberately NO cosine in it, because the longitudinal falloff is
    /// already inside the M lobes and the fibre's projected width is
    /// GroomFibreCosineWeight, which belongs to the GEOMETRY and not to the
    /// scattering function. Weighting this integral again is the classic way
    /// to conclude a correct hair model loses energy. With no
    /// absorption this is 1 to within the quadrature's error, which is the
    /// energy-conservation statement acceptance criterion 4 asks for; with
    /// absorption it is the fraction of energy the fibre returns.
    ///
    /// `thetaSteps` x `phiSteps` deterministic quadrature rather than Monte
    /// Carlo, so the number a test asserts on is the same number every run.
    [[nodiscard]] glm::vec3 GroomFibreWhiteFurnace(const GroomFibreParams& params, f32 sinThetaO, u32 thetaSteps,
                                                   u32 phiSteps) noexcept;

    /// The same integral evaluated through the SAMPLING routine instead of the
    /// quadrature: draws `sampleCount` directions and averages value/pdf. Two
    /// independent roads to one number — they agree only if evaluate, sample
    /// and pdf all agree, which is what makes this the triple's consistency
    /// check rather than a second energy check.
    [[nodiscard]] glm::vec3 GroomFibreSampledFurnace(const GroomFibreParams& params, f32 sinThetaO, f32 h,
                                                     u32 sampleCount, u32 seed) noexcept;
} // namespace OloEngine
