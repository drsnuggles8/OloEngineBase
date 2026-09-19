// OLO_TEST_LAYER: L1
// =============================================================================
// SkinOcularSurfaceTest.cpp — the corneal refraction, the iris response, and
// the agreement with an independent optical ground truth. Issue #1244.
//
// THE CASE THAT MATTERS MOST is TheShippedModelAgreesWithATwoSurfaceRayTrace.
// Everything else here is a property, and a property can hold while the eye is
// wrong: a refraction that bends the wrong way still moves the iris, still
// moves it more at oblique angles, and still returns to zero head-on. What
// separates a correct eye from a plausible one is a NUMBER, compared against
// something that was not derived from the same code.
//
// So this file carries its own two-surface trace — air -> corneal stroma 1.376
// -> aqueous humour 1.336 -> the iris plane, through clinical radii — written
// from the geometry and not from Renderer/SkinOcularSurface.h. It is the same
// ground truth as experiments/eye-cornea-reference/compare_refraction.py, which
// is where the approximation was CHOSEN; having it here as well is what turns
// that choice into a regression test. An experiment that ran once and a shader
// that drifted afterwards would look identical from the outside.
//
// THE TRACE IS ITSELF CHECKED FIRST, in TheGroundTruthReproducesTheLiterature,
// against the entrance-pupil magnification of 1.13 that Gullstrand and
// Bennett & Rabbetts publish. A ground truth nobody validated is a fourth
// opinion, and this file would then be comparing two wrong answers.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/SkinDiffusion.h"
#include "OloEngine/Renderer/SkinLayeredSpecular.h"
#include "OloEngine/Renderer/SkinOcularSurface.h"
#include "OloEngine/Renderer/SkinOralSurface.h"
#include "OloEngine/Renderer/SkinTransmission.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <cmath>
#include <tuple>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

        // ---------------------------------------------------------------------
        // The independent ground truth. EYE-LOCAL: origin at the globe centre,
        // +Z forward, millimetres. Every constant is clinical and every derived
        // one is derived here, so changing a radius cannot leave a stale number
        // further down — the same discipline the experiment file uses.
        // ---------------------------------------------------------------------
        struct SchematicEye
        {
            f32 Globe = 12.0f;
            f32 CorneaAnterior = 7.8f;
            f32 CorneaPosterior = 6.8f;
            f32 CorneaThickness = 0.55f;
            f32 LimbusRadius = 5.85f;
            f32 AcdFromApex = 3.6f;
            f32 PupilRadius = 2.0f;

            f32 NCornea = 1.376f;
            f32 NAqueous = 1.336f;

            [[nodiscard]] f32 LimbusZ() const
            {
                return std::sqrt(Globe * Globe - LimbusRadius * LimbusRadius);
            }
            [[nodiscard]] f32 ApexZ() const
            {
                const f32 sagitta =
                    CorneaAnterior - std::sqrt(CorneaAnterior * CorneaAnterior - LimbusRadius * LimbusRadius);
                return LimbusZ() + sagitta;
            }
            [[nodiscard]] f32 AnteriorCentreZ() const
            {
                return ApexZ() - CorneaAnterior;
            }
            [[nodiscard]] f32 PosteriorCentreZ() const
            {
                return (ApexZ() - CorneaThickness) - CorneaPosterior;
            }
            [[nodiscard]] f32 IrisPlaneZ() const
            {
                return ApexZ() - AcdFromApex;
            }
        };

        // Nearest forward intersection with a sphere centred on the axis.
        // Returns false on a miss; the hit is left untouched, never zeroed —
        // for the reason the production code gives about a zero direction.
        [[nodiscard]] bool HitSphere(const glm::vec3& origin, const glm::vec3& dir, f32 centreZ, f32 radius,
                                     glm::vec3& hit)
        {
            const glm::vec3 oc = origin - glm::vec3(0.0f, 0.0f, centreZ);
            const f32 b = 2.0f * glm::dot(oc, dir);
            const f32 c = glm::dot(oc, oc) - radius * radius;
            const f32 disc = b * b - 4.0f * c;
            if (!(disc > 0.0f))
                return false;
            const f32 root = std::sqrt(disc);
            const f32 t0 = (-b - root) * 0.5f;
            const f32 t1 = (-b + root) * 0.5f;
            const f32 t = (t0 > 1.0e-6f) ? t0 : t1;
            if (!(t > 1.0e-6f))
                return false;
            hit = origin + dir * t;
            return true;
        }

        // Snell, written out here as well. DELIBERATELY NOT CALLING
        // SkinOcularRefract: a ground truth that shares an implementation with
        // the thing it is checking cannot detect a bug in that implementation,
        // which is the entire failure mode this file exists to catch.
        [[nodiscard]] bool RefractHere(const glm::vec3& incident, const glm::vec3& normal, f32 eta,
                                       glm::vec3& out)
        {
            const f32 cosI = -glm::dot(normal, incident);
            const f32 k = 1.0f - eta * eta * (1.0f - cosI * cosI);
            if (!(k >= 0.0f))
                return false;
            out = glm::normalize(eta * incident + (eta * cosI - std::sqrt(k)) * normal);
            return true;
        }

        // The two-surface trace: where a ray entering at lateral height
        // `height`, travelling at `angleDeg` off the axis, lands on the iris
        // plane. Returns the signed lateral coordinate in millimetres.
        [[nodiscard]] bool TraceTwoSurface(const SchematicEye& eye, f32 height, f32 angleDeg, f32& landingMM)
        {
            const f32 a = glm::radians(angleDeg);
            const glm::vec3 dir = glm::normalize(glm::vec3(-std::sin(a), 0.0f, -std::cos(a)));
            const glm::vec3 basis(std::cos(a), 0.0f, -std::sin(a));
            const glm::vec3 start =
                glm::vec3(0.0f, 0.0f, eye.ApexZ()) - dir * 40.0f + basis * height;

            glm::vec3 p1{};
            if (!HitSphere(start, dir, eye.AnteriorCentreZ(), eye.CorneaAnterior, p1))
                return false;
            const glm::vec3 n1 = glm::normalize(p1 - glm::vec3(0.0f, 0.0f, eye.AnteriorCentreZ()));
            glm::vec3 d1{};
            if (!RefractHere(dir, n1, 1.0f / eye.NCornea, d1))
                return false;

            glm::vec3 p2{};
            if (!HitSphere(p1, d1, eye.PosteriorCentreZ(), eye.CorneaPosterior, p2))
                return false;
            const glm::vec3 n2 = glm::normalize(p2 - glm::vec3(0.0f, 0.0f, eye.PosteriorCentreZ()));
            glm::vec3 d2{};
            if (!RefractHere(d1, n2, eye.NCornea / eye.NAqueous, d2))
                return false;

            if (!(d2.z < -1.0e-6f))
                return false;
            const f32 t = (eye.IrisPlaneZ() - p2.z) / d2.z;
            landingMM = p2.x + d2.x * t;
            return true;
        }

        // ---------------------------------------------------------------------
        // The shipped model, driven the way the shader drives it: a SPHERE mesh
        // (so the entry point and the normal are the globe's) at the same
        // incoming ray, reported in the same millimetres.
        // ---------------------------------------------------------------------

        // The profile the shipped defaults describe, with the eye switched on.
        [[nodiscard]] SkinProfileParameters ShippedEye()
        {
            SkinProfileParameters p{};
            p.EvaluationModel = SkinEvaluationModel::OcularSurface;
            p.Ocular.OcularStrength = 1.0f;
            return p;
        }

        // Where the shipped model says a ray entering the GLOBE at lateral
        // height `height`, travelling at `angleDeg` off the axis, lands — in
        // millimetres on the iris plane, so it is directly comparable with
        // TraceTwoSurface above.
        [[nodiscard]] bool TraceShipped(const SkinProfileParameters& profile, f32 height, f32 angleDeg,
                                        f32& landingMM)
        {
            const SchematicEye eye{};
            const glm::vec3 axis(0.0f, 0.0f, 1.0f);
            const f32 a = glm::radians(angleDeg);
            const glm::vec3 dir = glm::normalize(glm::vec3(-std::sin(a), 0.0f, -std::cos(a)));
            const glm::vec3 basis(std::cos(a), 0.0f, -std::sin(a));
            const glm::vec3 start = glm::vec3(0.0f, 0.0f, eye.ApexZ()) - dir * 40.0f + basis * height;

            // The mesh the engine actually has: the globe sphere.
            glm::vec3 surface{};
            if (!HitSphere(start, dir, 0.0f, eye.Globe, surface))
                return false;
            const glm::vec3 n = glm::normalize(surface);
            const glm::vec3 view = -dir;

            const glm::vec4 corneaLane = SkinOcularCorneaLane(profile);
            const glm::vec4 irisLane = SkinOcularIrisLane(profile);

            // Outside the limbus there is no iris to land on.
            if (!(glm::dot(n, axis) >= corneaLane.w))
                return false;

            const glm::vec3 cornealNormal = SkinCornealNormal(n, axis, corneaLane.y);
            glm::vec3 refracted{};
            if (!SkinOcularRefract(-view, cornealNormal, corneaLane.x, refracted))
                return false;
            glm::vec3 hit{};
            if (!SkinIrisPlaneHit(n, axis, refracted, corneaLane.z, hit))
                return false;

            // The march is in EYE RADII; the ground truth is in millimetres.
            landingMM = hit.x * profile.Ocular.EyeRadiusMM;
            (void)irisLane;
            return true;
        }

        // The shipped model on a mesh that HAS corneal geometry: the ray enters
        // on the corneal sphere and the mesh supplies the corneal normal, so
        // the curvature ratio is 1 and the authored iris depth is the clinical
        // one. This is the configuration an eye ASSET would ship in, and it is
        // the configuration in which the index question has an answer — see
        // TheAqueousIndexBeatsTheStromalOne for why that distinction is
        // load-bearing rather than pedantic.
        [[nodiscard]] bool TraceCornealMesh(f32 ior, f32 height, f32 angleDeg, f32& landingMM)
        {
            const SchematicEye eye{};
            const glm::vec3 axis(0.0f, 0.0f, 1.0f);
            const f32 a = glm::radians(angleDeg);
            const glm::vec3 dir = glm::normalize(glm::vec3(-std::sin(a), 0.0f, -std::cos(a)));
            const glm::vec3 basis(std::cos(a), 0.0f, -std::sin(a));
            const glm::vec3 start = glm::vec3(0.0f, 0.0f, eye.ApexZ()) - dir * 40.0f + basis * height;

            glm::vec3 surface{};
            if (!HitSphere(start, dir, eye.AnteriorCentreZ(), eye.CorneaAnterior, surface))
                return false;
            // The mesh's own normal IS the corneal normal here, so no bend.
            const glm::vec3 n = glm::normalize(surface - glm::vec3(0.0f, 0.0f, eye.AnteriorCentreZ()));

            glm::vec3 refracted{};
            if (!SkinOcularRefract(dir, n, SkinCorneaEta(ior), refracted))
                return false;
            if (!(glm::dot(refracted, axis) < -1.0e-6f))
                return false;
            const f32 t = (eye.IrisPlaneZ() - surface.z) / glm::dot(refracted, axis);
            landingMM = surface.x + refracted.x * t;
            return true;
        }

    } // namespace

    // =========================================================================
    // The ground truth, checked before anything is checked against it
    // =========================================================================

    TEST(SkinOcularSurfaceTest, TheGroundTruthReproducesTheLiterature)
    {
        const SchematicEye eye{};

        // The entrance pupil: the incoming ray height that lands exactly on the
        // real pupil rim. Bisected, because the relationship is monotone in
        // height and there is no closed form through two spherical surfaces.
        f32 lo = 0.0f;
        f32 hi = eye.LimbusRadius * 0.98f;
        for (i32 i = 0; i < 80; ++i)
        {
            const f32 mid = 0.5f * (lo + hi);
            f32 landing = 0.0f;
            ASSERT_TRUE(TraceTwoSurface(eye, mid, 0.0f, landing))
                << "the ground-truth trace lost a ray it must not lose";
            if (landing < eye.PupilRadius)
                lo = mid;
            else
                hi = mid;
        }
        const f32 entranceRadius = 0.5f * (lo + hi);
        const f32 magnification = entranceRadius / eye.PupilRadius;

        // 1.13 is the published figure. The 0.02 band is the spherical-cornea
        // approximation this schematic makes against the real prolate one — it
        // is stated in the experiment's LIMITS section and it is the reason
        // this is a band and not an equality.
        EXPECT_NEAR(magnification, 1.13f, 0.02f)
            << "the two-surface trace no longer reproduces the literature's entrance-pupil "
               "magnification, so it cannot be used as a ground truth for anything below";
    }

    // =========================================================================
    // The headline: the shipped approximation against that ground truth
    // =========================================================================

    TEST(SkinOcularSurfaceTest, TheShippedModelAgreesWithATwoSurfaceRayTrace)
    {
        const SchematicEye eye{};
        const SkinProfileParameters profile = ShippedEye();

        // Worst-case error over the same sweep the experiment uses, as a
        // fraction of the iris radius — the unit an iris lookup is in.
        // RMS AND MAX, BOTH, and the claim is made on the RMS. The two say
        // different things and only one of them is about the model: the MAX is
        // set by a handful of rays at extreme grazing, where a sphere mesh's
        // missing corneal bulge costs the most, and it therefore measures the
        // FIXTURE's geometry rather than the refraction's accuracy. The RMS is
        // what the experiment reports and what an image actually looks like.
        f64 sumSqRefracted = 0.0;
        f64 sumSqPainted = 0.0;
        f32 worstRefracted = 0.0f;
        f32 worstPainted = 0.0f;
        i32 samples = 0;

        for (const f32 angle : { 0.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f })
        {
            for (i32 i = -20; i <= 20; ++i)
            {
                const f32 height = static_cast<f32>(i) * 0.16f;

                f32 truth = 0.0f;
                if (!TraceTwoSurface(eye, height, angle, truth))
                    continue;
                if (std::abs(truth) > eye.LimbusRadius)
                    continue;

                f32 shipped = 0.0f;
                if (!TraceShipped(profile, height, angle, shipped))
                    continue;

                // The PAINTED arm: the surface point's own lateral coordinate,
                // which is what an iris drawn onto the mesh shows. Computed
                // here so the comparison below is against a number rather than
                // against an adjective.
                const f32 a = glm::radians(angle);
                const glm::vec3 dir = glm::normalize(glm::vec3(-std::sin(a), 0.0f, -std::cos(a)));
                const glm::vec3 basis(std::cos(a), 0.0f, -std::sin(a));
                const glm::vec3 start = glm::vec3(0.0f, 0.0f, eye.ApexZ()) - dir * 40.0f + basis * height;
                glm::vec3 surface{};
                if (!HitSphere(start, dir, 0.0f, eye.Globe, surface))
                    continue;

                const f32 errRefracted = std::abs(shipped - truth);
                const f32 errPainted = std::abs(surface.x - truth);
                sumSqRefracted += static_cast<f64>(errRefracted) * errRefracted;
                sumSqPainted += static_cast<f64>(errPainted) * errPainted;
                worstRefracted = std::max(worstRefracted, errRefracted);
                worstPainted = std::max(worstPainted, errPainted);
                ++samples;
            }
        }

        ASSERT_GT(samples, 200) << "the sweep stopped reaching the iris; the fixture is no longer an eye";

        const auto rms = [samples](f64 sumSq)
        {
            return static_cast<f32>(std::sqrt(sumSq / static_cast<f64>(samples)));
        };
        const f32 refractedFraction = rms(sumSqRefracted) / eye.LimbusRadius;
        const f32 paintedFraction = rms(sumSqPainted) / eye.LimbusRadius;

        // THE MEASURED NUMBERS, over exactly this sweep: the shipped sphere-mesh
        // model lands at 3.9% RMS of the iris radius and the painted arm at
        // 16.7%. The bounds are those figures with headroom, so an ordinary
        // floating-point difference does not fail the test while a model that
        // stopped refracting does.
        EXPECT_LT(refractedFraction, 0.06f)
            << "the corneal refraction no longer tracks a two-surface trace: worst error "
            << refractedFraction * 100.0f << "% of the iris radius";

        // AND THE RATIO, which is the statement the feature is FOR. An absolute
        // bound alone would still pass if the ground truth drifted to meet a
        // broken model; this says the refracted arm is several times better
        // than doing nothing, which no single-sided drift can fake.
        EXPECT_GT(paintedFraction / refractedFraction, 3.0f)
            << "refracting is no longer materially better than painting the iris on "
            << "(painted " << paintedFraction * 100.0f << "%, refracted " << refractedFraction * 100.0f << "%)";

        // THE MAX, recorded with a loose bound rather than asserted tightly.
        // 18.6% is where it sits, and it is a property of the SPHERE FIXTURE
        // rather than of the model: the worst rays are the ones at extreme
        // grazing, where a globe with no corneal bulge puts the entry point
        // 1.1 mm behind where a real eye's is. A tight bound here would go red
        // for a geometry limitation this feature already documents.
        EXPECT_LT(worstRefracted / eye.LimbusRadius, 0.25f)
            << "even the worst single ray has moved: " << (worstRefracted / eye.LimbusRadius) * 100.0f << "%";
        EXPECT_LT(worstRefracted, worstPainted)
            << "the worst refracted ray is worse than the worst painted one";
    }

    TEST(SkinOcularSurfaceTest, TheAqueousIndexBeatsTheStromalOne)
    {
        // The trap this feature has: 1.376 is the number an author looks up for
        // "cornea", and it is the wrong one — the ray ends in the aqueous, not
        // in the stroma.
        //
        // ASKED ON A MESH WITH CORNEAL GEOMETRY, and that qualification is the
        // whole point of this test rather than a caveat on it.
        //
        // On the SPHERE PRIMITIVE the engine currently uses, the ordering does
        // not hold — measured, over this same sweep: aqueous 3.6% RMS against
        // stromal 3.1%. A higher index refracts harder, and refracting harder
        // happens to compensate for the entry point a sphere puts 1.1 mm too
        // far back. That is two errors cancelling, not a better model, and
        // tuning the index until a sphere looked right would be FITTING THE
        // INDEX TO A GEOMETRY BUG — after which an eye asset with a real cornea
        // would arrive and be wrong for a reason nobody could find.
        //
        // So the index is chosen where the index question is actually posed: a
        // mesh whose normal and entry point are the cornea's, where the only
        // remaining approximation IS the index. There the ordering is stark —
        // 0.10% RMS against 1.04%, a factor of ten — and it is the
        // configuration SkinOcularParameters::CorneaRadiusMM == EyeRadiusMM
        // exists to name.
        const SchematicEye eye{};

        f64 sumSqAqueous = 0.0;
        f64 sumSqStromal = 0.0;
        i32 samples = 0;
        for (const f32 angle : { 0.0f, 20.0f, 40.0f, 60.0f })
        {
            for (i32 i = -20; i <= 20; ++i)
            {
                const f32 height = static_cast<f32>(i) * 0.16f;
                f32 truth = 0.0f;
                if (!TraceTwoSurface(eye, height, angle, truth) || std::abs(truth) > eye.LimbusRadius)
                    continue;
                f32 a = 0.0f;
                f32 st = 0.0f;
                if (!TraceCornealMesh(eye.NAqueous, height, angle, a))
                    continue;
                if (!TraceCornealMesh(eye.NCornea, height, angle, st))
                    continue;
                sumSqAqueous += static_cast<f64>(a - truth) * (a - truth);
                sumSqStromal += static_cast<f64>(st - truth) * (st - truth);
                ++samples;
            }
        }
        ASSERT_GT(samples, 100) << "the index sweep stopped reaching the iris";

        const f32 aqueousRms = static_cast<f32>(std::sqrt(sumSqAqueous / samples)) / eye.LimbusRadius;
        const f32 stromalRms = static_cast<f32>(std::sqrt(sumSqStromal / samples)) / eye.LimbusRadius;

        EXPECT_LT(aqueousRms, stromalRms)
            << "the aqueous index is supposed to be the better one on corneal geometry; if this "
               "flipped, the default in SkinOcularParameters::CorneaIor is now wrong "
               "(aqueous "
            << aqueousRms * 100.0f << "%, stromal " << stromalRms * 100.0f << "%)";
        // AND BY A LOT, not by a rounding error — the decision is only worth
        // documenting if the margin is real. A factor of four leaves headroom
        // under the measured ten.
        EXPECT_GT(stromalRms / aqueousRms, 4.0f)
            << "the two indices now land within a factor of four of each other, so the choice "
               "between them is no longer a physical decision";
        // And the aqueous arm is genuinely accurate on this geometry, which is
        // what makes the 3.9% of the sphere-mesh fixture attributable to the
        // MESH rather than to the model.
        EXPECT_LT(aqueousRms, 0.005f)
            << "on corneal geometry the single-surface model should be sub-percent";
    }

    // =========================================================================
    // The lanes are DERIVED
    // =========================================================================

    TEST(SkinOcularSurfaceTest, EveryLaneComponentIsDerivedFromAClinicalLength)
    {
        const SkinProfileParameters profile = ShippedEye();
        const SkinOcularParameters& o = profile.Ocular;

        const glm::vec4 cornea = SkinOcularCorneaLane(profile);
        EXPECT_NEAR(cornea.x, 1.0f / o.CorneaIor, 1.0e-6f) << "eta is not 1 / CorneaIor";
        EXPECT_NEAR(cornea.y, o.EyeRadiusMM / o.CorneaRadiusMM, 1.0e-6f) << "curvature ratio is not the radius ratio";
        EXPECT_NEAR(cornea.z, o.IrisPlaneDepthMM / o.EyeRadiusMM, 1.0e-6f) << "iris depth is not in eye radii";
        const f32 sinLimbus = o.IrisRadiusMM / o.EyeRadiusMM;
        EXPECT_NEAR(cornea.w, std::sqrt(1.0f - sinLimbus * sinLimbus), 1.0e-6f) << "limbus cosine is not derived";

        const glm::vec4 iris = SkinOcularIrisLane(profile);
        EXPECT_NEAR(iris.x, o.IrisRadiusMM / o.EyeRadiusMM, 1.0e-6f);
        EXPECT_NEAR(iris.y, o.PupilRadiusMM / o.IrisRadiusMM, 1.0e-6f);
        EXPECT_NEAR(iris.z, o.LimbalRingWidthMM / o.IrisRadiusMM, 1.0e-6f);
        EXPECT_NEAR(iris.w, o.OcularStrength, 1.0e-6f);

        const glm::vec4 response = SkinOcularResponseLane(profile);
        const glm::vec4 tint = SkinOcularTintLane(profile);
        EXPECT_NEAR(response.x, o.LimbalRingStrength, 1.0e-6f);
        EXPECT_NEAR(response.y, o.PupilDarkening, 1.0e-6f);
        EXPECT_NEAR(response.z, o.IrisConcavity, 1.0e-6f);
        EXPECT_NEAR(response.w, o.RefractionStrength, 1.0e-6f);
    }

    TEST(SkinOcularSurfaceTest, TheDefaultProfileIsTheSchematicEye)
    {
        // The shipped defaults are supposed to BE the eye the optical reference
        // traces. Asserted rather than assumed, because every number in this
        // file's ground truth is written twice — once here and once in the
        // experiment — and a default that drifted from them would make every
        // comparison above a comparison between two different eyes.
        const SchematicEye eye{};
        const SkinOcularParameters o{};
        EXPECT_FLOAT_EQ(o.EyeRadiusMM, eye.Globe);
        EXPECT_FLOAT_EQ(o.CorneaRadiusMM, eye.CorneaAnterior);
        EXPECT_FLOAT_EQ(o.IrisRadiusMM, eye.LimbusRadius);
        EXPECT_FLOAT_EQ(o.PupilRadiusMM, eye.PupilRadius);
        EXPECT_FLOAT_EQ(o.CorneaIor, eye.NAqueous);

        // And the derived iris depth: the clinical ACD is measured from the
        // CORNEAL apex, and a sphere mesh's apex is further back. The default
        // is that difference, which is arithmetic rather than a fitted number.
        const f32 derived = eye.Globe - eye.IrisPlaneZ();
        EXPECT_NEAR(o.IrisPlaneDepthMM, derived, 0.01f)
            << "IrisPlaneDepthMM is no longer the sphere apex's distance to the schematic iris plane";
    }

    // =========================================================================
    // Neutral identities — every one of them EXACT
    // =========================================================================

    TEST(SkinOcularSurfaceTest, AZeroMasterReturnsItsInputsUntouched)
    {
        SkinProfileParameters profile{};
        profile.EvaluationModel = SkinEvaluationModel::OcularSurface;
        // OcularStrength stays at its default 0.
        const glm::vec3 albedo(0.31f, 0.42f, 0.53f);
        const glm::vec3 normal = glm::normalize(glm::vec3(0.2f, 0.1f, 0.97f));
        const glm::vec3 view = glm::normalize(glm::vec3(0.05f, 0.0f, 1.0f));

        const SkinOcularResult r =
            ApplySkinOcularSurface(albedo, normal, view, glm::vec3(0.0f, 0.0f, 1.0f),
                                   SkinOcularCorneaLane(profile), SkinOcularIrisLane(profile),
                                   SkinOcularResponseLane(profile), SkinOcularTintLane(profile));

        // BIT-IDENTICAL, not close. This is the arm every A/B in the evidence
        // matrix is measured against, and "almost unchanged" would make the
        // control a third variant rather than a baseline.
        EXPECT_EQ(r.Albedo, albedo);
        EXPECT_EQ(r.Normal, normal);
        EXPECT_LT(r.IrisRadial, 0.0f) << "a neutral profile must report no iris, not iris radius zero";
        EXPECT_FALSE(r.Refracted);
        EXPECT_EQ(r.Reason, SkinOcularFallbackReason::NotAuthored)
            << "a profile with no eye should say so, not name a failure it did not hit";
    }

    TEST(SkinOcularSurfaceTest, ScleraIsLeftToTheSkinTransport)
    {
        const SkinProfileParameters profile = ShippedEye();
        const glm::vec3 axis(0.0f, 0.0f, 1.0f);
        const glm::vec3 albedo(0.8f, 0.8f, 0.78f);
        // Well outside the limbus: 60 degrees off the optical axis, where the
        // clinical limbus is at 29.2.
        const glm::vec3 normal = glm::normalize(glm::vec3(std::sin(glm::radians(60.0f)), 0.0f,
                                                          std::cos(glm::radians(60.0f))));

        const SkinOcularResult r =
            ApplySkinOcularSurface(albedo, normal, normal, axis, SkinOcularCorneaLane(profile),
                                   SkinOcularIrisLane(profile), SkinOcularResponseLane(profile), SkinOcularTintLane(profile));

        EXPECT_EQ(r.Albedo, albedo) << "an eye's white must stay a skin term, untouched";
        EXPECT_EQ(r.Normal, normal);
        EXPECT_LT(r.IrisRadial, 0.0f);
        EXPECT_EQ(r.Reason, SkinOcularFallbackReason::OutsideLimbus);
    }

    TEST(SkinOcularSurfaceTest, ACurvatureRatioOfOneIsABitExactIdentity)
    {
        // How a mesh that already carries real corneal geometry opts out of the
        // bend. Exactness matters: such a mesh reaches the 0.1% row of the
        // reference table only if its interpolated normal is used AS IT IS.
        const glm::vec3 normal = glm::normalize(glm::vec3(0.31f, -0.2f, 0.9f));
        EXPECT_EQ(SkinCornealNormal(normal, glm::vec3(0.0f, 0.0f, 1.0f), 1.0f), normal);
    }

    TEST(SkinOcularSurfaceTest, AZeroRingAndAZeroConcavityAreBitExactIdentities)
    {
        for (const f32 radial : { 0.0f, 0.3f, 0.7f, 0.99f, 1.5f })
            EXPECT_EQ(SkinIrisLimbalRing(radial, 0.12f, 0.0f), 1.0f) << "radial " << radial;

        const glm::vec3 normal = glm::normalize(glm::vec3(0.1f, 0.2f, 0.97f));
        EXPECT_EQ(SkinIrisShadingNormal(normal, glm::vec3(0.0f, 0.0f, 1.0f),
                                        glm::vec3(0.2f, 0.0f, 0.8f), 0.4875f, 0.0f),
                  normal);
    }

    // =========================================================================
    // The iris response
    // =========================================================================

    TEST(SkinOcularSurfaceTest, TheLimbalRingDarkensOnlyABandJustInsideTheIrisEdge)
    {
        constexpr f32 kBand = 0.12f;
        constexpr f32 kStrength = 0.7f;

        // Well inside the iris, nothing happens — the ring is a RING.
        EXPECT_FLOAT_EQ(SkinIrisLimbalRing(0.0f, kBand, kStrength), 1.0f);
        EXPECT_FLOAT_EQ(SkinIrisLimbalRing(1.0f - 2.0f * kBand, kBand, kStrength), 1.0f);

        // It reaches full strength ONE BAND INSIDE the edge, not at it — which
        // is what leaves room for SkinIrisDiscMask to fade the whole response
        // out over [1 - band, 1] without fighting the ring.
        EXPECT_NEAR(SkinIrisLimbalRing(1.0f - kBand, kBand, kStrength), 1.0f - kStrength, 1.0e-6f);

        // And monotone in between, which is what stops the "ring" being a band
        // with a bright line through it.
        f32 previous = 1.0f;
        for (i32 i = 0; i <= 40; ++i)
        {
            const f32 radial = static_cast<f32>(i) / 40.0f;
            const f32 value = SkinIrisLimbalRing(radial, kBand, kStrength);
            EXPECT_LE(value, previous + 1.0e-6f) << "ring not monotone at radial " << radial;
            previous = value;
        }
    }

    TEST(SkinOcularSurfaceTest, AZeroWidthRingNeverDividesByAZeroSpan)
    {
        // THE NaN THIS BOUND EXISTS FOR. `smoothstep(a, a, x)` divides by its
        // span and is UNDEFINED in GLSL; the CPU's SmoothStep guards it and the
        // shader's builtin does not, so a zero band would be a NaN albedo on
        // the GPU and a clean frame on the CPU — a parity divergence reachable
        // from an authored profile whose ring width is 0 and whose strength is
        // not.
        //
        // Two halves: the FLOOR makes it unreachable through a lane, and the
        // function is finite even if something reaches it anyway.
        EXPECT_GT(kMinSkinLimbalRingWidthRatio, 0.0f)
            << "the ring width's floor is back at zero, which is a NaN on the GPU";

        SkinProfileParameters zeroWidth{};
        zeroWidth.EvaluationModel = SkinEvaluationModel::OcularSurface;
        zeroWidth.Ocular.OcularStrength = 1.0f;
        zeroWidth.Ocular.LimbalRingWidthMM = 0.0f;
        zeroWidth.Ocular.LimbalRingStrength = 0.9f;
        (void)zeroWidth.Sanitize();
        EXPECT_GE(SkinOcularIrisLane(zeroWidth).z, kMinSkinLimbalRingWidthRatio)
            << "a zero authored ring width reached the lane as a zero span";
        EXPECT_GE(SkinOcularTintLane(zeroWidth).w, kMinSkinIrisEdgeBand)
            << "a zero authored ring width reached the tint lane's band as a zero span";

        for (const f32 radial : { 0.0f, 0.5f, 0.98f, 1.0f, 1.2f })
        {
            EXPECT_TRUE(std::isfinite(SkinIrisLimbalRing(radial, 0.0f, 0.9f)))
                << "a zero band produced a non-finite ring at radial " << radial;
            EXPECT_TRUE(std::isfinite(SkinIrisDiscMask(radial, 0.0f)))
                << "a zero band produced a non-finite disc mask at radial " << radial;
        }
    }

    TEST(SkinOcularSurfaceTest, TheIrisResponseReachesExactlyOneAtTheLimbus)
    {
        // THE CONTINUITY CLAIM, and it is the one a still frame shows loudest
        // if it breaks: the limbus is reached by TWO boundaries — the geometric
        // cone test on the surface normal and the radial coordinate on the
        // REFRACTED iris point — which do not coincide. A term still
        // non-neutral when either is crossed leaves a hard ring one pixel wide
        // all the way around the iris.
        //
        // So every iris term must reach exactly 1 at radial 1, where the sclera
        // beyond it already is.
        SkinProfileParameters profile = ShippedEye();
        profile.Ocular.LimbalRingStrength = 0.9f;
        profile.Ocular.IrisColor = { 0.25f, 0.4f, 0.55f };
        profile.Ocular.PupilDarkening = 1.0f;
        ASSERT_TRUE(profile.Sanitize());

        const glm::vec4 iris = SkinOcularIrisLane(profile);
        const glm::vec4 response = SkinOcularResponseLane(profile);
        const glm::vec4 tint = SkinOcularTintLane(profile);

        // The disc mask is what carries the fade, so it is the thing that must
        // be zero at the edge.
        EXPECT_NEAR(SkinIrisDiscMask(1.0f, tint.w), 0.0f, 1.0e-6f)
            << "the iris disc does not close at radial 1, so the tint and the ring run onto the sclera";
        EXPECT_NEAR(SkinIrisDiscMask(1.5f, tint.w), 0.0f, 1.0e-6f) << "the disc mask reopens past the edge";

        // And the combined gain the shader applies is continuous through it: no
        // step bigger than a smooth curve's own slope over a fine sweep.
        // SWEPT OVER THE LIMBUS REGION ONLY — [0.6, 1.6] — and NOT over the
        // whole disc, because the whole disc contains the PUPIL MARGIN and that
        // is a deliberately narrow band. The pupil's soft edge spans 12% of the
        // pupil radius (kSkinPupilEdgeBandFraction), so it steps about 0.06 per
        // 0.005 of disc coordinate, which is correct there and would mask the
        // thing this test is actually about. The pupil's own shape is pinned by
        // ThePupilIsDarkInsideAndAbsentOutside; this is the LIMBUS.
        //
        // 0.6 is comfortably outside the pupil (0.342 of the iris radius) and
        // inside the ring's inner edge (1 - 2*band = 0.76).
        f32 previous = -1.0f;
        f32 worstStep = 0.0f;
        f32 worstAt = 0.0f;
        for (i32 i = 0; i <= 200; ++i)
        {
            const f32 radial = 0.6f + static_cast<f32>(i) / 200.0f; // 0.6 .. 1.6
            const f32 ring = SkinIrisLimbalRing(radial, iris.z, response.x);
            const f32 pupil = SkinIrisPupilMask(radial, iris.y, response.y);
            const f32 mask = SkinIrisDiscMask(radial, tint.w);
            // Red channel of the combined gain, which is what the shader forms.
            const f32 gain = 1.0f + (tint.x * ring * pupil - 1.0f) * mask;
            if (previous >= 0.0f && std::abs(gain - previous) > worstStep)
            {
                worstStep = std::abs(gain - previous);
                worstAt = radial;
            }
            previous = gain;
        }
        // The ring and the edge fade each span a band of 0.12, so a smoothstep's
        // peak slope over them is about 1.5 / 0.12 = 12.5 per unit radial, or
        // 0.0625 per 0.005 sample. A HARD limbus step would be the ring's full
        // 0.9 strength in one sample. 0.08 separates those by an order of
        // magnitude without being a number tuned until the test passed.
        EXPECT_LT(worstStep, 0.08f)
            << "the iris gain steps by " << worstStep << " at radial " << worstAt
            << " — that is a hard ring, not a shaded edge";

        // At and beyond the edge the gain is exactly neutral.
        const f32 atEdge = 1.0f + (tint.x * SkinIrisLimbalRing(1.0f, iris.z, response.x) *
                                       SkinIrisPupilMask(1.0f, iris.y, response.y) -
                                   1.0f) *
                                      SkinIrisDiscMask(1.0f, tint.w);
        EXPECT_NEAR(atEdge, 1.0f, 1.0e-6f) << "the iris response is not neutral at the limbus";
    }

    TEST(SkinOcularSurfaceTest, ThePupilIsDarkInsideAndAbsentOutside)
    {
        constexpr f32 kPupil = 0.34f;

        EXPECT_NEAR(SkinIrisPupilMask(0.0f, kPupil, 1.0f), 0.0f, 1.0e-6f) << "the pupil centre must be black";
        EXPECT_NEAR(SkinIrisPupilMask(1.0f, kPupil, 1.0f), 1.0f, 1.0e-6f) << "the iris edge must be untouched";

        // The soft edge is a BAND, not a step: two samples either side of the
        // pupil radius must differ, and the value at the radius itself must sit
        // between them. A hard step would put all three at 0 or 1.
        const f32 inner = SkinIrisPupilMask(kPupil * 0.9f, kPupil, 1.0f);
        const f32 at = SkinIrisPupilMask(kPupil, kPupil, 1.0f);
        const f32 outer = SkinIrisPupilMask(kPupil * 1.1f, kPupil, 1.0f);
        EXPECT_LT(inner, at);
        EXPECT_LT(at, outer);

        // Zero darkening is exact, like every other neutral in this feature.
        EXPECT_EQ(SkinIrisPupilMask(0.0f, kPupil, 0.0f), 1.0f);
    }

    TEST(SkinOcularSurfaceTest, TheIrisDishTiltVanishesAtTheIrisEdge)
    {
        // The continuity claim. A cone would tilt just as hard at the iris edge
        // as at the pupil, stepping the shading normal across the limbus and
        // drawing a hard ring at exactly the radius the limbal ring is already
        // drawing attention to. The paraboloid ramp is what prevents it, and
        // this is the assertion that keeps the ramp there.
        const glm::vec3 axis(0.0f, 0.0f, 1.0f);
        const glm::vec3 normal = glm::normalize(glm::vec3(0.3f, 0.0f, 0.95f));
        constexpr f32 kIrisRadius = 0.4875f;
        constexpr f32 kConcavity = 0.35f;

        const glm::vec3 atEdge =
            SkinIrisShadingNormal(normal, axis, glm::vec3(kIrisRadius, 0.0f, 0.8f), kIrisRadius, kConcavity);
        EXPECT_NEAR(glm::length(atEdge - normal), 0.0f, 1.0e-5f)
            << "the dish tilt no longer vanishes at the iris edge; the limbus will show a hard ring";

        // And it is largest near the pupil, which is the direction that makes
        // it a dish rather than a dome.
        const glm::vec3 nearPupil =
            SkinIrisShadingNormal(normal, axis, glm::vec3(kIrisRadius * 0.1f, 0.0f, 0.8f), kIrisRadius, kConcavity);
        EXPECT_GT(glm::length(nearPupil - normal), glm::length(atEdge - normal));
    }

    // =========================================================================
    // Criterion 3, as a number: what a head-on capture cannot show
    // =========================================================================

    TEST(SkinOcularSurfaceTest, RefractionChangesNothingHeadOnAndEverythingObliquely)
    {
        // The reason issue #1244's third acceptance criterion is about MOTION,
        // stated as a test. On the optical axis the refracted and painted iris
        // points coincide by symmetry, so a front-on screenshot is exactly the
        // capture that cannot tell a refracting eye from a painted one. Every
        // pixel of difference the feature is worth is off-axis.
        const SkinProfileParameters profile = ShippedEye();
        const glm::vec3 axis(0.0f, 0.0f, 1.0f);
        const glm::vec4 cornea = SkinOcularCorneaLane(profile);
        const glm::vec4 iris = SkinOcularIrisLane(profile);
        const glm::vec4 tint = SkinOcularTintLane(profile);
        const glm::vec3 albedo(0.5f);

        SkinProfileParameters painted = profile;
        painted.Ocular.RefractionStrength = 0.0f;
        ASSERT_TRUE(painted.Sanitize());
        const glm::vec4 paintedResponse = SkinOcularResponseLane(painted);
        const glm::vec4 refractedResponse = SkinOcularResponseLane(profile);

        // Head on, at the apex: identical.
        {
            const glm::vec3 n = axis;
            const SkinOcularResult a =
                ApplySkinOcularSurface(albedo, n, axis, axis, cornea, iris, paintedResponse, tint);
            const SkinOcularResult b =
                ApplySkinOcularSurface(albedo, n, axis, axis, cornea, iris, refractedResponse, tint);
            EXPECT_NEAR(a.IrisRadial, b.IrisRadial, 1.0e-5f)
                << "at the corneal apex the two models must agree; if they do not, the refraction "
                   "has a bias that a head-on capture would show";
        }

        // Off axis: the refracted iris point is measurably closer to the
        // optical axis than the painted one. That direction is the corneal
        // MAGNIFICATION — the same effect that makes the entrance pupil 1.13x
        // the real one — so the sign here is a physical claim, not a
        // convention.
        f32 largestGap = 0.0f;
        for (const f32 degrees : { 5.0f, 10.0f, 15.0f, 20.0f, 25.0f })
        {
            const f32 t = glm::radians(degrees);
            const glm::vec3 n = glm::normalize(glm::vec3(std::sin(t), 0.0f, std::cos(t)));
            const glm::vec3 view = axis;

            const SkinOcularResult a = ApplySkinOcularSurface(albedo, n, view, axis, cornea, iris, paintedResponse, tint);
            const SkinOcularResult b =
                ApplySkinOcularSurface(albedo, n, view, axis, cornea, iris, refractedResponse, tint);
            ASSERT_GE(a.IrisRadial, 0.0f) << "painted arm lost the iris at " << degrees << " degrees";
            ASSERT_GE(b.IrisRadial, 0.0f) << "refracted arm lost the iris at " << degrees << " degrees";
            EXPECT_EQ(b.Reason, SkinOcularFallbackReason::None)
                << "a pixel that refracted still names a failure reason — the success path is "
                   "assigning one, which is what made every refracting pixel claim to be sclera";

            EXPECT_LT(b.IrisRadial, a.IrisRadial)
                << "the cornea must MAGNIFY the iris, so a refracted sample lands nearer the axis "
                   "than a painted one (at "
                << degrees << " degrees)";
            largestGap = std::max(largestGap, a.IrisRadial - b.IrisRadial);
        }

        // And the difference is a real fraction of the iris, not a rounding
        // error dressed as an effect. The experiment measures up to 19.7% of
        // the iris radius at a 30-degree gaze; 3% is a floor with room under it.
        EXPECT_GT(largestGap, 0.03f)
            << "the refraction moves the iris by less than 3% of its radius, which no viewer "
               "would see — the feature has stopped doing anything";
    }

    TEST(SkinOcularSurfaceTest, TheQualityLadderIsContinuousAndEndsAtThePaintedArm)
    {
        // Criterion 4's fallback, as a property. RefractionStrength is the knob
        // a path that cannot afford the full model turns down, and a ladder
        // that popped between its rungs would be worse than no ladder.
        const SkinProfileParameters base = ShippedEye();
        const glm::vec3 axis(0.0f, 0.0f, 1.0f);
        const glm::vec4 cornea = SkinOcularCorneaLane(base);
        const glm::vec4 iris = SkinOcularIrisLane(base);
        const glm::vec4 tint = SkinOcularTintLane(base);
        const glm::vec3 albedo(0.5f);
        const f32 t = glm::radians(18.0f);
        const glm::vec3 n = glm::normalize(glm::vec3(std::sin(t), 0.0f, std::cos(t)));

        f32 previous = -1.0f;
        for (i32 i = 0; i <= 10; ++i)
        {
            SkinProfileParameters p = base;
            p.Ocular.RefractionStrength = static_cast<f32>(i) / 10.0f;
            ASSERT_TRUE(p.Sanitize());
            const SkinOcularResult r =
                ApplySkinOcularSurface(albedo, n, axis, axis, cornea, iris, SkinOcularResponseLane(p), tint);
            ASSERT_GE(r.IrisRadial, 0.0f);
            if (previous >= 0.0f)
            {
                EXPECT_LE(r.IrisRadial, previous + 1.0e-6f) << "the ladder is not monotone at rung " << i;
                EXPECT_LT(previous - r.IrisRadial, 0.05f) << "the ladder pops between rungs " << i - 1
                                                          << " and " << i;
            }
            previous = r.IrisRadial;
        }
    }

    // =========================================================================
    // Layer order — issue #1244's second criterion, as code order
    // =========================================================================

    TEST(SkinOcularSurfaceTest, ThePupilIsPlacedAtTheRefractedPointNotThePaintedOne)
    {
        // THE LAYER-SORTING CLAIM, made checkable. "Cornea in front of iris"
        // means the pupil you SEE has been moved by the cornea. If the pupil
        // were drawn at the surface coordinate and the refraction applied to
        // something else, the eye would still look like an eye in a still frame
        // and the layers would be in the wrong order.
        //
        // MEASURED AS AN ALBEDO RATIO AND NOT AS A BAND OF DISAGREEING
        // VERDICTS, and the first version of this test did the latter and could
        // not pass. The refraction shifts the disc coordinate by about 7.6% —
        // the reciprocal of the corneal magnification — while the pupil's soft
        // edge spans 12% of the pupil radius, so there is NO angle at which one
        // arm is fully inside the pupil and the other fully outside. Looking
        // for one finds nothing and proves nothing.
        //
        // What there IS, right at the pupil margin, is a large difference in
        // how much pupil each arm sees: a shift of a fifth of the soft edge
        // lands the two arms on very different parts of a smoothstep. Measured,
        // at 10 degrees off axis: the painted arm keeps 94% of the albedo and
        // the refracted arm keeps 9%. That factor of ten is the claim.
        SkinProfileParameters profile = ShippedEye();
        profile.Ocular.PupilDarkening = 1.0f;
        profile.Ocular.LimbalRingStrength = 0.0f; // isolate the pupil
        ASSERT_TRUE(profile.Sanitize());

        const glm::vec3 axis(0.0f, 0.0f, 1.0f);
        const glm::vec4 cornea = SkinOcularCorneaLane(profile);
        const glm::vec4 iris = SkinOcularIrisLane(profile);
        const glm::vec4 tint = SkinOcularTintLane(profile);
        const glm::vec3 albedo(0.6f, 0.5f, 0.4f);

        SkinProfileParameters painted = profile;
        painted.Ocular.RefractionStrength = 0.0f;
        ASSERT_TRUE(painted.Sanitize());
        const glm::vec4 paintedResponse = SkinOcularResponseLane(painted);
        const glm::vec4 refractedResponse = SkinOcularResponseLane(profile);

        f32 bestRatio = 1.0f;
        f32 bestPaintedRadial = -1.0f;
        for (i32 i = 1; i <= 72; ++i)
        {
            const f32 t = glm::radians(static_cast<f32>(i) * 0.4f);
            const glm::vec3 n = glm::normalize(glm::vec3(std::sin(t), 0.0f, std::cos(t)));
            const SkinOcularResult p =
                ApplySkinOcularSurface(albedo, n, axis, axis, cornea, iris, paintedResponse, tint);
            const SkinOcularResult r =
                ApplySkinOcularSurface(albedo, n, axis, axis, cornea, iris, refractedResponse, tint);
            if (p.IrisRadial < 0.0f || r.IrisRadial < 0.0f)
                continue;
            // THE SIGN IS A PHYSICAL CLAIM: the cornea magnifies, so the
            // refracted sample lands NEARER the axis — deeper into the pupil —
            // and is therefore DARKER. A model that bent the other way would
            // make this ratio less than one at every angle.
            if (r.Albedo.r > 1.0e-6f)
            {
                const f32 ratio = p.Albedo.r / r.Albedo.r;
                if (ratio > bestRatio)
                {
                    bestRatio = ratio;
                    bestPaintedRadial = p.IrisRadial;
                }
            }
        }

        EXPECT_GT(bestRatio, 3.0f)
            << "the painted and refracted arms never disagree about the pupil by more than a "
               "factor of "
            << bestRatio
            << ". The pupil is being drawn at the surface coordinate, so the cornea is behind "
               "the iris rather than in front of it.";

        // AND IT HAPPENS AT THE PUPIL MARGIN, which is what makes the ratio
        // above a statement about the PUPIL rather than about some other term
        // that happened to differ. `iris.y` is the pupil radius in disc units.
        ASSERT_GE(bestPaintedRadial, 0.0f);
        EXPECT_NEAR(bestPaintedRadial, iris.y, iris.y * 0.25f)
            << "the largest disagreement is at disc coordinate " << bestPaintedRadial
            << ", nowhere near the pupil margin at " << iris.y
            << " — so whatever moved, it was not the pupil";
    }

    // =========================================================================
    // Loudness: a failure is reported, never painted over
    // =========================================================================

    TEST(SkinOcularSurfaceTest, AnInvertedIndexIsReportedRatherThanSilentlyPainted)
    {
        // eta above 1 means the lane carries an INVERTED conversion — light
        // entering a denser medium cannot total-internal-reflect. The point of
        // this case is that the failure has a NAME: a painted-looking eye with
        // no diagnostic is the silent fallback this repo's rules forbid.
        const SkinProfileParameters profile = ShippedEye();
        glm::vec4 cornea = SkinOcularCorneaLane(profile);
        cornea.x = 2.0f; // an eta nobody can author, only mis-derive

        const glm::vec3 axis(0.0f, 0.0f, 1.0f);
        const f32 t = glm::radians(25.0f);
        const glm::vec3 n = glm::normalize(glm::vec3(std::sin(t), 0.0f, std::cos(t)));
        const glm::vec3 albedo(0.5f);

        const SkinOcularResult r = ApplySkinOcularSurface(albedo, n, axis, axis, cornea,
                                                          SkinOcularIrisLane(profile),
                                                          SkinOcularResponseLane(profile), SkinOcularTintLane(profile));
        EXPECT_EQ(r.Reason, SkinOcularFallbackReason::TotalInternalReflection);
        EXPECT_FALSE(r.Refracted);
        EXPECT_EQ(r.Albedo, albedo) << "a failed refraction must leave the pixel alone, not half-shade it";
    }

    TEST(SkinOcularSurfaceTest, TheOcularBlockNeverIntroducesANonFiniteValue)
    {
        // THE CONTRACT IS "NEVER INTRODUCES", NOT "NEVER RETURNS", and the
        // difference is worth stating because the first version of this test
        // asserted the stronger thing and was wrong to.
        //
        // If a caller hands in a NaN albedo, returning it unchanged is CORRECT:
        // the NaN came from the albedo map or from a material somebody
        // authored, and a refraction that quietly replaced it would hide a bug
        // that belongs to whoever produced it. What must never happen is a
        // finite input turning into a non-finite output — a degenerate normal,
        // a zero axis or a corrupt lane producing a NaN that then spreads
        // through the whole frame's lighting.
        const SkinProfileParameters profile = ShippedEye();
        const glm::vec4 cornea = SkinOcularCorneaLane(profile);
        const glm::vec4 iris = SkinOcularIrisLane(profile);
        const glm::vec4 response = SkinOcularResponseLane(profile);
        const glm::vec4 tint = SkinOcularTintLane(profile);

        const glm::vec3 axis(0.0f, 0.0f, 1.0f);
        const glm::vec3 good = glm::normalize(glm::vec3(0.1f, 0.0f, 0.99f));
        const glm::vec3 nan3(kNaN, 0.0f, 1.0f);
        const glm::vec3 zero(0.0f);
        const glm::vec3 albedo(0.5f, 0.4f, 0.3f);

        // Every corrupt DIRECTION, with a finite albedo. None may produce a
        // non-finite anything.
        for (const auto& [normal, view, ax] : { std::tuple{ nan3, good, axis },
                                                std::tuple{ good, nan3, axis },
                                                std::tuple{ good, good, nan3 },
                                                std::tuple{ zero, good, axis },
                                                std::tuple{ good, zero, axis },
                                                std::tuple{ good, good, zero } })
        {
            const SkinOcularResult r = ApplySkinOcularSurface(albedo, normal, view, ax, cornea, iris, response, tint);
            EXPECT_TRUE(std::isfinite(r.Albedo.r) && std::isfinite(r.Albedo.g) && std::isfinite(r.Albedo.b))
                << "a corrupt direction produced a non-finite albedo from a finite one";
            EXPECT_EQ(r.Albedo, albedo) << "a corrupt direction must leave the pixel alone, not half-shade it";
        }

        // A corrupt LANE, with everything else finite — the case a stale or
        // mis-derived profile slot produces.
        for (i32 component = 0; component < 4; ++component)
        {
            glm::vec4 corruptCornea = cornea;
            corruptCornea[component] = kNaN;
            const SkinOcularResult r =
                ApplySkinOcularSurface(albedo, good, good, axis, corruptCornea, iris, response, tint);
            EXPECT_TRUE(std::isfinite(r.Albedo.r) && std::isfinite(r.Albedo.g) && std::isfinite(r.Albedo.b))
                << "cornea lane component " << component << " being NaN produced a non-finite albedo";
            EXPECT_TRUE(std::isfinite(r.Normal.x) && std::isfinite(r.Normal.y) && std::isfinite(r.Normal.z))
                << "cornea lane component " << component << " being NaN produced a non-finite normal";
        }

        // A NaN albedo passes through UNCHANGED, which is the contract's other
        // half stated as an assertion rather than left implicit.
        {
            const SkinOcularResult r =
                ApplySkinOcularSurface(glm::vec3(kNaN), good, good, axis, cornea, iris, response, tint);
            EXPECT_TRUE(std::isnan(r.Albedo.r)) << "a NaN albedo was silently replaced rather than passed on";
        }

        // And the lanes themselves, which are what actually reach the GPU. A
        // profile authored with NaNs must not be able to pack one.
        SkinProfileParameters corrupt{};
        corrupt.EvaluationModel = SkinEvaluationModel::OcularSurface;
        corrupt.Ocular.EyeRadiusMM = kNaN;
        corrupt.Ocular.IrisRadiusMM = kNaN;
        corrupt.Ocular.CorneaIor = kNaN;
        corrupt.Ocular.IrisPlaneDepthMM = kNaN;
        corrupt.Ocular.PupilRadiusMM = kNaN;
        corrupt.Ocular.OcularStrength = kNaN;
        for (const glm::vec4& lane :
             { SkinOcularCorneaLane(corrupt), SkinOcularIrisLane(corrupt), SkinOcularResponseLane(corrupt),
               SkinOcularTintLane(corrupt) })
        {
            for (i32 i = 0; i < 4; ++i)
                EXPECT_TRUE(std::isfinite(lane[i])) << "a NaN-authored profile packed a non-finite lane";
        }
    }

    // =========================================================================
    // Sanitize
    // =========================================================================

    TEST(SkinOcularSurfaceTest, SanitizeEnforcesTheThreeCrossFieldRules)
    {
        {
            SkinProfileParameters p{};
            p.Ocular.IrisRadiusMM = 30.0f; // wider than any globe
            EXPECT_FALSE(p.Sanitize());
            EXPECT_LE(p.Ocular.IrisRadiusMM, p.Ocular.EyeRadiusMM)
                << "an iris wider than the globe puts the limbus past the equator";
        }
        {
            SkinProfileParameters p{};
            p.Ocular.PupilRadiusMM = 4.9f; // legal on its own, not against the iris
            p.Ocular.IrisRadiusMM = 2.0f;
            EXPECT_FALSE(p.Sanitize());
            EXPECT_LE(p.Ocular.PupilRadiusMM, p.Ocular.IrisRadiusMM) << "a pupil wider than the iris leaves no iris";
        }
        {
            SkinProfileParameters p{};
            p.Ocular.CorneaRadiusMM = 19.0f;
            p.Ocular.EyeRadiusMM = 12.0f;
            EXPECT_FALSE(p.Sanitize());
            EXPECT_LE(p.Ocular.CorneaRadiusMM, p.Ocular.EyeRadiusMM)
                << "a cornea flatter than the globe is not an eye";
        }

        // And the limbus cosine survives all three, which is what the rules are
        // actually protecting: sqrt(1 - sin^2) with sin > 1 is a NaN in a lane.
        SkinProfileParameters worst{};
        worst.EvaluationModel = SkinEvaluationModel::OcularSurface;
        worst.Ocular.IrisRadiusMM = 99.0f;
        worst.Ocular.EyeRadiusMM = 5.0f;
        (void)worst.Sanitize();
        EXPECT_TRUE(std::isfinite(SkinOcularCorneaLane(worst).w));
    }

    TEST(SkinOcularSurfaceTest, TheDefaultProfileIsItselfValid)
    {
        SkinProfileParameters p{};
        p.EvaluationModel = SkinEvaluationModel::OcularSurface;
        EXPECT_TRUE(p.Sanitize()) << "the shipped ocular defaults fall outside their own bounds";
    }

    // =========================================================================
    // The version is cumulative
    // =========================================================================

    TEST(SkinOcularSurfaceTest, VersionFiveStillDoesEverythingVersionFourDid)
    {
        // The trap SkinDiffusion.cpp, SkinTransmission.cpp and PBRCommon.glsl
        // have each recorded once: a new version that is not added to the
        // CUMULATIVE lists silently turns an older term off. Version 5 matters
        // most of all for the coat, because the coat IS the tear line.
        EXPECT_TRUE(SkinEvaluatesOcularSurface(SkinEvaluationModel::OcularSurface));
        EXPECT_FALSE(SkinEvaluatesOcularSurface(SkinEvaluationModel::OralSurface));

        EXPECT_TRUE(SkinEvaluatesOralSurface(SkinEvaluationModel::OcularSurface))
            << "version 5 lost the wet coat — which is the TEAR LINE, so the eye is now dry";
        EXPECT_TRUE(SkinEvaluatesLayeredSpecular(SkinEvaluationModel::OcularSurface))
            << "version 5 lost the layered specular";

        SkinProfileParameters p{};
        p.EvaluationModel = SkinEvaluationModel::OcularSurface;
        EXPECT_FALSE(BuildSkinDiffusionKernel(p, SkinDiffusionQuality::Medium).IsIdentity())
            << "version 5 lost the screen-space diffusion — an eye's sclera would read as chalk";

        p.Transmission.Strength = 1.0f;
        EXPECT_GT(glm::length(SkinTransmissionScatterLane(p)), 0.0f)
            << "version 5 lost the thin-region transmission";
    }

} // namespace OloEngine::Tests
