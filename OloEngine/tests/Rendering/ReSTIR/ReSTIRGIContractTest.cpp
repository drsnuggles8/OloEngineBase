// OLO_TEST_LAYER: plumbing
// =============================================================================
// ReSTIRGIContractTest.cpp — the PIECES of the ReSTIR GI estimator, pinned on a
// machine with no GPU. Issue #1169.
//
// Read docs/design/restir-gi-reconnection-shift.md first; the section numbers
// below cite it, and §7 is the list this file implements.
//
// WHAT IT PINS, AND WHY EACH ONE NEEDS A TEST RATHER THAN A COMMENT:
//
//   * THE JACOBIAN, by the measure IDENTITY over randomised geometry — never by
//     a hand-picked expected value, which would be derived from the same
//     understanding that wrote the code. The identity catches the two mistakes
//     the term invites (cosines at the shading points instead of at the vertex,
//     and the two squared distances swapped) because both break it while both
//     produce a finite, plausible number.
//   * THE ENVIRONMENT ARM, as a LIMIT rather than an assertion: J -> 1 as the
//     vertex recedes. That is what makes "distant samples shift with J = 1" a
//     derived fact instead of a special case somebody chose.
//   * THE DOMAIN GATE and the AGE CAP, which have no DI counterpart and are the
//     two guards a reviewer is most likely to read as paranoia.
//   * THE DDGI HAND-OFF, as an exclusivity property over EVERY combination of
//     inputs. #979's non-goal ("the two must not double-count") is a property of
//     how three passes compose, so it is asserted against the pure function that
//     owns it rather than inspected in an image, where a factor of two in the
//     ambient reads as "the new tier is a bit strong".
//
// WHAT IT CANNOT PIN: that the assembled estimator converges to the right
// integral. Every piece here can pass while the wiring is wrong — a Jacobian
// applied to the wrong term, a density converted at the wrong point. That is
// ReSTIRGIOracleTest's job, and neither file substitutes for the other.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ReSTIR/ReSTIRGITechnique.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirGI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"

#include <glm/glm.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#include <string>

namespace OloEngine::Tests
{
    using namespace OloEngine::ReSTIR;

    namespace
    {
        // The shader tree, relative to whatever directory the test binary was
        // started from. The test binary runs from the repo root (CLAUDE.md), and
        // the walk upward is what keeps a run from a build directory working
        // rather than silently skipping the scan.
        [[nodiscard]] std::filesystem::path ResolveShaderPath(std::string_view relative)
        {
            std::filesystem::path candidate = std::filesystem::current_path();
            for (int depth = 0; depth < 6; ++depth)
            {
                const auto probe = candidate / "OloEditor" / "assets" / "shaders" / relative;
                if (std::filesystem::exists(probe))
                    return probe;
                candidate = candidate.parent_path();
            }
            return std::filesystem::path("OloEditor/assets/shaders") / relative;
        }

        [[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream file(path);
            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        [[nodiscard]] u32 ScanDefine(const std::string& source, const char* name)
        {
            const std::regex pattern(std::string("#define\\s+") + name + "\\s+(\\d+)u?");
            std::smatch match;
            if (!std::regex_search(source, match, pattern))
                return ~0u;
            return static_cast<u32>(std::stoul(match[1].str()));
        }

        [[nodiscard]] u32 ScanConstUint(const std::string& source, const char* name)
        {
            const std::regex pattern(std::string("const\\s+uint\\s+") + name + "\\s*=\\s*(\\d+)u?\\s*;");
            std::smatch match;
            if (!std::regex_search(source, match, pattern))
                return ~0u;
            return static_cast<u32>(std::stoul(match[1].str()));
        }

        // A sample vertex and two shading points that see it from genuinely
        // different angles and distances — the configuration a spatial reuse
        // actually faces, rather than a symmetric one where every arm agrees.
        struct ShiftFixture
        {
            GISample Sample{};
            glm::vec3 DestPoint{ 0.0f };
            glm::vec3 SourcePoint{ 0.0f };
        };

        [[nodiscard]] ShiftFixture MakeRandomShift(std::mt19937& rng)
        {
            std::uniform_real_distribution<f32> coordinate(-4.0f, 4.0f);
            std::uniform_real_distribution<f32> normalAxis(-1.0f, 1.0f);

            ShiftFixture fixture;
            fixture.Sample.Kind = GISampleKind::SurfaceHit;
            fixture.Sample.Position = glm::vec3(coordinate(rng), coordinate(rng), coordinate(rng));
            // Deliberately NOT axis-aligned: an axis-aligned normal makes several
            // wrong forms of the Jacobian agree with the right one, which is how
            // a test like this passes while proving nothing.
            glm::vec3 n(normalAxis(rng), normalAxis(rng), normalAxis(rng));
            if (glm::dot(n, n) < 1.0e-4f)
                n = glm::vec3(0.3f, 0.6f, 0.74f);
            fixture.Sample.Normal = glm::normalize(n);
            fixture.Sample.Radiance = glm::vec3(1.0f, 0.8f, 0.6f);

            do
            {
                fixture.DestPoint = glm::vec3(coordinate(rng), coordinate(rng), coordinate(rng));
                fixture.SourcePoint = glm::vec3(coordinate(rng), coordinate(rng), coordinate(rng));
            } while (glm::length(fixture.DestPoint - fixture.Sample.Position) < 0.25f ||
                     glm::length(fixture.SourcePoint - fixture.Sample.Position) < 0.25f);
            return fixture;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The Jacobian, by identity
    // -------------------------------------------------------------------------

    // THE ONE CHECK THAT CATCHES A WRONG JACOBIAN. Converting an area density at
    // the SOURCE and then shifting must equal converting it at the DESTINATION
    // directly:
    //
    //     AreaToSolidAngle(p, dest) == AreaToSolidAngle(p, source) / J
    //
    // An expected-value assertion on J would prove nothing: the term is a smooth
    // geometric factor and almost any wrong version of it produces a plausible
    // number. This identity is broken by taking the cosines at the shading points
    // and by swapping the two squared distances, which are the two mistakes the
    // expression invites.
    TEST(ReSTIRGIContract, ShiftJacobianIsTheAreaToSolidAngleMeasureChange)
    {
        std::mt19937 rng(0x1169u);
        constexpr f32 kAreaPdf = 0.37f;
        u32 evaluated = 0;
        for (u32 i = 0; i < 512; ++i)
        {
            const ShiftFixture fixture = MakeRandomShift(rng);
            const f32 j = GIShiftJacobian(fixture.Sample, fixture.DestPoint, fixture.SourcePoint);
            if (!(j > 0.0f))
                continue; // a degenerate configuration; the guard is its own test below

            const f32 atSource = GIAreaPdfToSolidAnglePdf(kAreaPdf, fixture.Sample, fixture.SourcePoint);
            const f32 atDest = GIAreaPdfToSolidAnglePdf(kAreaPdf, fixture.Sample, fixture.DestPoint);
            if (!(atSource > 0.0f) || !(atDest > 0.0f))
                continue;

            ++evaluated;
            EXPECT_NEAR(atDest, atSource / j, std::max(atDest, atSource) * 1.0e-3f)
                << "configuration " << i << ": J=" << j << " atSource=" << atSource << " atDest=" << atDest;
        }
        // The loop has to have done work, or a Jacobian that returned 0 for
        // everything would pass by never asserting anything.
        EXPECT_GT(evaluated, 400u) << "too many configurations were rejected for this test to mean anything";
    }

    // Swapping the endpoints gives the reciprocal. Cheap, and it fails for any
    // form of the expression that is not symmetric in the way a change of measure
    // has to be.
    TEST(ReSTIRGIContract, ShiftJacobianReversesToItsReciprocal)
    {
        std::mt19937 rng(0x51F7u);
        for (u32 i = 0; i < 256; ++i)
        {
            const ShiftFixture fixture = MakeRandomShift(rng);
            const f32 forward = GIShiftJacobian(fixture.Sample, fixture.DestPoint, fixture.SourcePoint);
            const f32 backward = GIShiftJacobian(fixture.Sample, fixture.SourcePoint, fixture.DestPoint);
            if (!(forward > 0.0f) || !(backward > 0.0f))
                continue;
            EXPECT_NEAR(forward * backward, 1.0f, 1.0e-3f) << "configuration " << i;
        }
    }

    // -------------------------------------------------------------------------
    // The environment arm, as a LIMIT (design note §4.3)
    // -------------------------------------------------------------------------

    // An ENVIRONMENT sample shifts with J exactly 1, and the reason is a limit
    // rather than a convention: as the vertex recedes along the sample direction,
    // both distances grow at the same rate and both vertex cosines go to 1.
    //
    // This asserts BOTH halves — that a receding SURFACE sample's Jacobian
    // converges to 1, and that the stored-as-a-direction arm returns exactly 1 —
    // because only the pair of them makes the special case a derived fact. The
    // first half alone would leave "it returns 1" unconnected to why.
    TEST(ReSTIRGIContract, AReceedingVertexConvergesToTheEnvironmentArmsExactOne)
    {
        const glm::vec3 dest(0.4f, -0.3f, 0.0f);
        const glm::vec3 source(-0.6f, 0.5f, 0.0f);
        const glm::vec3 direction = glm::normalize(glm::vec3(0.3f, 0.5f, 0.81f));

        f32 previousError = 1.0e9f;
        for (const f32 distance : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        {
            GISample receding{};
            receding.Kind = GISampleKind::SurfaceHit;
            receding.Position = direction * distance;
            // The vertex faces back along the ray, which is what a distant
            // surface sampled from either shading point looks like.
            receding.Normal = -direction;

            const f32 j = GIShiftJacobian(receding, dest, source);
            ASSERT_GT(j, 0.0f) << "distance " << distance;
            const f32 error = std::abs(j - 1.0f);
            EXPECT_LT(error, previousError) << "J did not move toward 1 at distance " << distance;
            previousError = error;
        }
        EXPECT_LT(previousError, 1.0e-3f) << "J did not converge to 1 as the vertex receded";

        GISample environment{};
        environment.Kind = GISampleKind::Environment;
        environment.Position = direction;
        EXPECT_FLOAT_EQ(GIShiftJacobian(environment, dest, source), 1.0f);
        // And in the other direction too — a delta in the measure cannot depend
        // on which end you look from.
        EXPECT_FLOAT_EQ(GIShiftJacobian(environment, source, dest), 1.0f);
    }

    // -------------------------------------------------------------------------
    // The guards that reject rather than scale (design note §3, §6.3)
    // -------------------------------------------------------------------------

    TEST(ReSTIRGIContract, ADegenerateShiftIsRejectedWithZeroRatherThanPassedThroughWithOne)
    {
        GISample sample{};
        sample.Kind = GISampleKind::SurfaceHit;
        sample.Position = glm::vec3(0.0f, 0.0f, 2.0f);
        sample.Normal = glm::vec3(0.0f, 0.0f, -1.0f);

        // The SOURCE saw the vertex edge-on: its cosine is the Jacobian's
        // DENOMINATOR, and scaling by it would produce a firefly that temporal
        // reuse then keeps alive for as long as the M cap allows.
        const glm::vec3 edgeOn(5.0f, 0.0f, 2.0f);
        EXPECT_FLOAT_EQ(GIShiftJacobian(sample, glm::vec3(0.0f), edgeOn), 0.0f);

        // A shading point coincident with the vertex.
        EXPECT_FLOAT_EQ(GIShiftJacobian(sample, sample.Position, glm::vec3(0.0f)), 0.0f);

        // ZERO, NOT ONE. One would mean "no change"; zero means "reject". The two
        // read the same in an image and differently in the estimator.
        EXPECT_FLOAT_EQ(GIShiftJacobian(GISample{}, glm::vec3(1.0f), glm::vec3(2.0f)), 0.0f);
    }

    // The NEAR-FIELD gate, which has no DI counterpart: dDestSq is in the
    // Jacobian's denominator and a GI vertex can be centimetres from the
    // destination, so a reuse that close is rejected rather than scaled.
    TEST(ReSTIRGIContract, TheDomainGateRejectsANearFieldOrBelowHorizonReconnection)
    {
        const glm::vec3 dest(0.0f);
        const glm::vec3 normal(0.0f, 0.0f, 1.0f);
        constexpr f32 kMinimum = 0.05f;

        GISample close{};
        close.Kind = GISampleKind::SurfaceHit;
        close.Position = glm::vec3(0.0f, 0.0f, 0.01f); // a centimetre away
        close.Normal = glm::vec3(0.0f, 0.0f, -1.0f);
        EXPECT_FALSE(ReconnectionInDomain(close, dest, normal, kMinimum));

        // `distant`, not `far`: <windows.h> still defines `far` as a macro, so a
        // local named that turns into a parse error whose message names neither.
        GISample distant = close;
        distant.Position = glm::vec3(0.0f, 0.0f, 1.0f);
        EXPECT_TRUE(ReconnectionInDomain(distant, dest, normal, kMinimum));

        // BELOW THE HORIZON. A vertex behind the destination's surface cannot be
        // reconnected to at all: no path goes through it, and the target function
        // would be zero anyway — but a gate that let it through would put a zero
        // in a denominator somewhere downstream rather than rejecting here.
        GISample behind = distant;
        behind.Position = glm::vec3(0.0f, 0.0f, -1.0f);
        EXPECT_FALSE(ReconnectionInDomain(behind, dest, normal, kMinimum));

        // An ENVIRONMENT sample has no vertex, so the only question is the
        // hemisphere — the minimum distance has nothing to apply to.
        GISample sky{};
        sky.Kind = GISampleKind::Environment;
        sky.Position = glm::vec3(0.0f, 0.0f, 1.0f);
        EXPECT_TRUE(ReconnectionInDomain(sky, dest, normal, kMinimum));
        sky.Position = glm::vec3(0.0f, 0.0f, -1.0f);
        EXPECT_FALSE(ReconnectionInDomain(sky, dest, normal, kMinimum));
    }

    // -------------------------------------------------------------------------
    // The age cap — the second staleness bound (design note §10)
    // -------------------------------------------------------------------------

    TEST(ReSTIRGIContract, SampleAgeSaturatesRatherThanWrapping)
    {
        // An age that WRAPPED would make the oldest samples look the freshest,
        // which is the one failure an age cap exists to prevent, arriving through
        // the cap itself.
        EXPECT_EQ(AdvanceSampleAge(0u), 1u);
        EXPECT_EQ(AdvanceSampleAge(kMaxSampleAgeFrames - 1u), kMaxSampleAgeFrames);
        EXPECT_EQ(AdvanceSampleAge(kMaxSampleAgeFrames), kMaxSampleAgeFrames);
        EXPECT_EQ(AdvanceSampleAge(kMaxSampleAgeFrames + 100u), kMaxSampleAgeFrames);

        EXPECT_TRUE(SampleAgeAcceptable(4u, 8u));
        EXPECT_TRUE(SampleAgeAcceptable(8u, 8u));
        EXPECT_FALSE(SampleAgeAcceptable(9u, 8u));
        // A cap of zero would mean "drop everything", which is temporal reuse
        // switched off by another name while still reporting itself as running.
        EXPECT_TRUE(SampleAgeAcceptable(1u, 0u));
    }

    TEST(ReSTIRGIContract, TheIdentityLaneCarriesKindAndAgeThroughAFloat)
    {
        for (const u32 age : { 0u, 1u, 63u, 64u, 1000u, kMaxSampleAgeFrames })
        {
            for (const auto kind : { GISampleKind::None, GISampleKind::SurfaceHit, GISampleKind::Environment })
            {
                const f32 packed = PackGIIdentity(kind, age);
                GISampleKind decodedKind{};
                u32 decodedAge = 0;
                UnpackGIIdentity(packed, decodedKind, decodedAge);
                EXPECT_EQ(decodedKind, kind) << "age " << age;
                EXPECT_EQ(decodedAge, age) << "kind " << std::to_underlying(kind);
            }
        }

        // An out-of-range kind decodes to None rather than to whatever the bits
        // happen to name: a reservoir read at the wrong layout must come back
        // EMPTY, never as a plausible sample of some other family.
        GISampleKind kind{};
        u32 age = 0;
        UnpackGIIdentity(PackReservoirIdentity(5u, 3u), kind, age);
        EXPECT_EQ(kind, GISampleKind::None);
    }

    TEST(ReSTIRGIContract, SanitizeEmptiesACorruptReservoirRatherThanClampingIt)
    {
        GIReservoir good{};
        good.Sample.Kind = GISampleKind::SurfaceHit;
        good.Sample.Position = glm::vec3(1.0f, 2.0f, 3.0f);
        good.Sample.Normal = glm::vec3(0.0f, 0.0f, 1.0f);
        good.Sample.Radiance = glm::vec3(0.5f);
        good.TargetPdf = 0.25f;
        good.WeightSum = 1.5f;
        good.M = 8.0f;
        good.W = 0.75f;
        EXPECT_EQ(SanitizeGIReservoir(good), good);

        // A CLAMP WOULD KEEP M, and M is a claim about how many candidates were
        // seen — a false one suppresses every future candidate at that pixel, so a
        // transient corruption becomes permanent.
        GIReservoir nanWeight = good;
        nanWeight.WeightSum = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_TRUE(SanitizeGIReservoir(nanWeight).IsEmpty());

        GIReservoir infPosition = good;
        infPosition.Sample.Position.y = std::numeric_limits<f32>::infinity();
        EXPECT_TRUE(SanitizeGIReservoir(infPosition).IsEmpty());

        GIReservoir negativeM = good;
        negativeM.M = -1.0f;
        EXPECT_TRUE(SanitizeGIReservoir(negativeM).IsEmpty());

        GIReservoir impossibleAge = good;
        impossibleAge.Sample.Age = kMaxSampleAgeFrames + 1u;
        EXPECT_TRUE(SanitizeGIReservoir(impossibleAge).IsEmpty());
    }

    // -------------------------------------------------------------------------
    // THE DDGI HAND-OFF — #979's non-goal, as a property (design note §5)
    // -------------------------------------------------------------------------

    // THE TEST THIS WHOLE SEAM EXISTS FOR. Over EVERY combination of inputs:
    //
    //   * exactly one mechanism owns the indirect diffuse term at the PRIMARY
    //     vertex — never both (a factor of two in the ambient, which reads as
    //     "the new tier is a bit strong") and never neither (a room with no
    //     indirect light at all, which reads as "the tier is broken");
    //   * SSGI never composites while ReSTIR GI owns the term, because SSGI is a
    //     THIRD estimate of the same integral;
    //   * the probe cache is never read at the SECONDARY vertex unless ReSTIR GI
    //     is live, because there is no secondary vertex otherwise — reporting one
    //     would be a claim about a path that was never traced.
    TEST(ReSTIRGIContract, ExactlyOneMechanismOwnsTheIndirectDiffuseTerm)
    {
        for (const bool active : { false, true })
        {
            for (const bool ssgi : { false, true })
            {
                for (const bool tail : { false, true })
                {
                    const IndirectDiffuseInputs inputs{
                        .ReSTIRGIActive = active,
                        .SSGIRequested = ssgi,
                        .DDGITailRequested = tail,
                    };
                    const IndirectDiffuseSources sources = SelectIndirectDiffuseSources(inputs);

                    EXPECT_NE(sources.DDGIAtPrimary, sources.ReSTIRGIAtPrimary)
                        << "active=" << active << " ssgi=" << ssgi << " tail=" << tail
                        << ": the indirect diffuse term must have exactly one owner at the primary vertex";

                    if (sources.ReSTIRGIAtPrimary)
                    {
                        EXPECT_FALSE(sources.SSGIComposite)
                            << "SSGI composited a third estimate of the same integral while ReSTIR GI owned it";
                    }
                    else
                    {
                        EXPECT_FALSE(sources.DDGIAtSecondary)
                            << "the probe cache was reported read at a bounce vertex that was never traced";
                        EXPECT_EQ(sources.SSGIComposite, ssgi)
                            << "SSGI's own switch stopped being honoured while this tier was standing down";
                    }
                }
            }
        }
    }

    // The cache is read at ONE vertex per path, and enabling the tier MOVES which
    // one. Spelled out as its own assertion because the property above constrains
    // the primary vertex and this is the other half of the sentence.
    TEST(ReSTIRGIContract, TheProbeCacheIsReadAtExactlyOneVertexPerPath)
    {
        const auto off = SelectIndirectDiffuseSources({ .ReSTIRGIActive = false, .DDGITailRequested = true });
        EXPECT_TRUE(off.DDGIAtPrimary);
        EXPECT_FALSE(off.DDGIAtSecondary);

        const auto on = SelectIndirectDiffuseSources({ .ReSTIRGIActive = true, .DDGITailRequested = true });
        EXPECT_FALSE(on.DDGIAtPrimary);
        EXPECT_TRUE(on.DDGIAtSecondary);

        // Turning the tail off makes the tier strictly ONE BOUNCE. That is a
        // QUALITY choice — an indoor scene loses its multi-bounce fill — and
        // never a correctness one, so it must not put the cache back at the
        // primary vertex.
        const auto noTail = SelectIndirectDiffuseSources({ .ReSTIRGIActive = true, .DDGITailRequested = false });
        EXPECT_FALSE(noTail.DDGIAtPrimary);
        EXPECT_FALSE(noTail.DDGIAtSecondary);
        EXPECT_TRUE(noTail.ReSTIRGIAtPrimary);
    }

    // -------------------------------------------------------------------------
    // The technique table and the settings
    // -------------------------------------------------------------------------

    TEST(ReSTIRGIContract, TechniqueSelectionReportsTheMostFundamentalReasonFirst)
    {
        // Everything wrong at once. The FIRST guard is what gets reported: a
        // frame that trips an earlier row would trip the later ones too, and
        // reporting "this scene has nothing to bounce" in front of "this device
        // has no ray tracing" would bury the reason that explains the frame.
        ReSTIRGITechniqueInputs inputs{};
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason, ReSTIRGIFallbackReason::NotRequested);

        inputs.Requested = true;
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason, ReSTIRGIFallbackReason::RenderingPathUnsupported);
        inputs.DeferredPathActive = true;
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason, ReSTIRGIFallbackReason::ShaderUnavailable);
        inputs.ShadersReady = true;
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason, ReSTIRGIFallbackReason::RayTracingUnavailable);
        inputs.RayTracingAvailable = true;
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason,
                  ReSTIRGIFallbackReason::AccelerationStructureEmpty);
        inputs.TlasReady = true;
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason, ReSTIRGIFallbackReason::GPUSceneUnavailable);
        inputs.GPUSceneAvailable = true;
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason, ReSTIRGIFallbackReason::TargetUnavailable);
        inputs.TargetsAvailable = true;
        inputs.HistoryLayoutMatches = false;
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason, ReSTIRGIFallbackReason::LayoutVersionMismatch);
        inputs.HistoryLayoutMatches = true;
        EXPECT_EQ(SelectReSTIRGITechnique(inputs).Reason,
                  ReSTIRGIFallbackReason::NoIndirectSourceInScene);

        inputs.Engagement.LightCount = 1;
        const auto decision = SelectReSTIRGITechnique(inputs);
        EXPECT_TRUE(decision.IsReSTIR());
        EXPECT_EQ(decision.Reason, ReSTIRGIFallbackReason::None);
        EXPECT_EQ(decision.Effective, IndirectDiffuseTechnique::ReSTIRGI);
    }

    // GI's engagement counts every light TYPE, including directional — which
    // differs from DI's on purpose, and the difference is not cosmetic: the sun
    // bouncing off a floor is exactly the indirect light this tier exists for,
    // while DI excludes directional lights because the clustered loop keeps them
    // for their cascades.
    TEST(ReSTIRGIContract, EngagePredicateCountsEveryIndirectSourceIncludingTheSky)
    {
        EXPECT_FALSE(ReSTIRGIEngagePredicate({}));
        EXPECT_TRUE(ReSTIRGIEngagePredicate({ .LightCount = 1 }));
        EXPECT_TRUE(ReSTIRGIEngagePredicate({ .EmissiveTriangles = 1 }));
        // An outdoor scene with no lights at all is lit entirely by the sky, so
        // leaving this out would stand the tier down on exactly the scenes it
        // renders best.
        EXPECT_TRUE(ReSTIRGIEngagePredicate({ .EnvironmentAvailable = true }));
    }

    TEST(ReSTIRGIContract, EveryFallbackReasonAndDebugViewHasAName)
    {
        // A counter nobody can read is not a counter. Every enumerator gets a
        // sentence, and the loop is what stops the next one being added without.
        for (u32 i = 0; i < std::to_underlying(ReSTIRGIFallbackReason::Count); ++i)
        {
            const auto name = ToString(static_cast<ReSTIRGIFallbackReason>(i));
            EXPECT_NE(name, "unknown") << "fallback reason " << i;
            EXPECT_FALSE(name.empty());
        }
        for (u32 i = 0; i < std::to_underlying(ReSTIRGIDebugView::Count); ++i)
        {
            const auto name = ToString(static_cast<ReSTIRGIDebugView>(i));
            EXPECT_NE(name, "unknown") << "debug view " << i;
        }
        for (u32 i = 0; i < std::to_underlying(GISampleKind::Count); ++i)
        {
            EXPECT_NE(ToString(static_cast<GISampleKind>(i)), "unknown") << "sample kind " << i;
        }
        for (u32 i = 0; i < std::to_underlying(IndirectDiffuseTechnique::Count); ++i)
        {
            EXPECT_NE(ToString(static_cast<IndirectDiffuseTechnique>(i)), "unknown") << "technique " << i;
        }
    }

    TEST(ReSTIRGIContract, SanitizeSettingsHoldsEveryKnobToItsRange)
    {
        ReSTIRGISettings wild{};
        wild.InitialCandidates = 9999;
        wild.SpatialNeighbours = 9999;
        wild.SpatialPasses = 0;
        wild.TemporalMCap = std::numeric_limits<f32>::quiet_NaN();
        wild.MaxSampleAge = 0;
        wild.SpatialRadiusPixels = -5.0f;
        wild.MinReconnectionDistance = std::numeric_limits<f32>::infinity();
        wild.MaxBounceDistance = -1.0f;
        wild.MaxRadianceClamp = std::numeric_limits<f32>::infinity();
        wild.RayOriginNormalBias = std::numeric_limits<f32>::quiet_NaN();
        wild.BiasMode = static_cast<ReSTIR::BiasMode>(99);
        wild.DebugView = static_cast<ReSTIRGIDebugView>(99);

        const ReSTIRGISettings clean = SanitizeReSTIRGISettings(wild);
        const ReSTIRGISettings defaults{};
        EXPECT_LE(clean.InitialCandidates, kReSTIRGIMaxInitialCandidates);
        EXPECT_GE(clean.InitialCandidates, 1u);
        EXPECT_LE(clean.SpatialNeighbours, kReSTIRGIMaxSpatialNeighbours);
        EXPECT_GE(clean.SpatialPasses, 1u);
        EXPECT_TRUE(std::isfinite(clean.TemporalMCap));
        // 1, NEVER 0: an age cap of zero would drop every sample on the frame
        // after it was traced, which is temporal reuse switched off by another
        // name — and it would report itself as running.
        EXPECT_GE(clean.MaxSampleAge, 1u);
        EXPECT_LE(clean.MaxSampleAge, ReSTIR::kMaxSampleAgeFrames);
        EXPECT_GE(clean.SpatialRadiusPixels, 1.0f);
        EXPECT_TRUE(std::isfinite(clean.MinReconnectionDistance));
        EXPECT_GT(clean.MaxBounceDistance, 0.0f);
        EXPECT_TRUE(std::isfinite(clean.MaxRadianceClamp));
        EXPECT_TRUE(std::isfinite(clean.RayOriginNormalBias));
        EXPECT_EQ(clean.BiasMode, defaults.BiasMode);
        EXPECT_EQ(clean.DebugView, ReSTIRGIDebugView::Radiance);

        // Sanitizing twice changes nothing: a sanitizer that was not idempotent
        // would make SettingsClamped fire every frame and the counter useless.
        EXPECT_EQ(SanitizeReSTIRGISettings(clean), clean);
        // The defaults must already be legal, or every scene load reports a clamp.
        EXPECT_EQ(SanitizeReSTIRGISettings(defaults), defaults);

        // THE DEFAULT CANDIDATE COUNT IS 1, and that is a claim about cost rather
        // than about quality: a GI candidate is a bounce ray plus an NEE shadow
        // ray, where a DI candidate is a table lookup. A default of 32 here would
        // be 64 rays per pixel.
        EXPECT_EQ(defaults.InitialCandidates, 1u);
    }

    // -------------------------------------------------------------------------
    // The GLSL twin's constants
    // -------------------------------------------------------------------------

    TEST(ReSTIRGIContract, ShaderConstantsMatchTheCppOnes)
    {
        const auto paramsPath = ResolveShaderPath("include/ReSTIRGIParams.glsl");
        const auto reservoirPath = ResolveShaderPath("include/ReservoirGI.glsl");
        ASSERT_TRUE(std::filesystem::exists(paramsPath)) << paramsPath.string();
        ASSERT_TRUE(std::filesystem::exists(reservoirPath)) << reservoirPath.string();

        const std::string params = ReadTextFile(paramsPath);
        const std::string reservoir = ReadTextFile(reservoirPath);
        ASSERT_FALSE(params.empty());
        ASSERT_FALSE(reservoir.empty());

        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_MAX_INITIAL_CANDIDATES"), kReSTIRGIMaxInitialCandidates);
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_MAX_SPATIAL_NEIGHBOURS"), kReSTIRGIMaxSpatialNeighbours);

        // The LAYOUT VERSION, and it is deliberately NOT DI's: the two layouts
        // share an encoding but not a shape, so one shared number would make a DI
        // packing bump invalidate GI's history for a change that cannot affect it.
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_GI_RESERVOIR_LAYOUT_VERSION"), kGIReservoirLayoutVersion);

        // The packed sample kinds. Their values live in a texture, so a renumber
        // on either side reads every reservoir as the wrong family.
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_GI_SAMPLE_NONE"), std::to_underlying(GISampleKind::None));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_GI_SAMPLE_SURFACE_HIT"),
                  std::to_underlying(GISampleKind::SurfaceHit));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_GI_SAMPLE_ENVIRONMENT"),
                  std::to_underlying(GISampleKind::Environment));
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_GI_SAMPLE_KIND_COUNT"),
                  std::to_underlying(GISampleKind::Count));

        // The age lane's ceiling. An age past it would WRAP in the shader and
        // make the oldest samples look the freshest.
        EXPECT_EQ(ScanConstUint(reservoir, "OLO_GI_MAX_SAMPLE_AGE"), kMaxSampleAgeFrames);

        // The debug views are read from the UBO as an integer, so the GLSL macro
        // and the enum must agree or the wrong AOV lands on screen.
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_RADIANCE"),
                  std::to_underlying(ReSTIRGIDebugView::Radiance));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_RAW_CANDIDATE"),
                  std::to_underlying(ReSTIRGIDebugView::RawCandidate));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_HISTORY_VALIDITY"),
                  std::to_underlying(ReSTIRGIDebugView::HistoryValidity));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_VARIANCE"),
                  std::to_underlying(ReSTIRGIDebugView::Variance));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_RESERVOIR_M"),
                  std::to_underlying(ReSTIRGIDebugView::ReservoirM));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_RESERVOIR_W"),
                  std::to_underlying(ReSTIRGIDebugView::ReservoirW));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_SAMPLE_KIND"),
                  std::to_underlying(ReSTIRGIDebugView::SampleKind));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_SAMPLE_AGE"),
                  std::to_underlying(ReSTIRGIDebugView::SampleAge));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_SAMPLE_RADIANCE"),
                  std::to_underlying(ReSTIRGIDebugView::SampleRadiance));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_RECONNECTION_LENGTH"),
                  std::to_underlying(ReSTIRGIDebugView::ReconnectionLength));
        EXPECT_EQ(ScanDefine(params, "OLO_RESTIR_GI_VIEW_BIAS_CLAMP"),
                  std::to_underlying(ReSTIRGIDebugView::BiasClampState));

        // ReservoirGI.glsl must EXPORT the four state operations under GI's
        // names, or every GI draw stops compiling. They come from the core's
        // macro, so the check is that the macro is invoked with GI's prefix.
        EXPECT_NE(reservoir.find("OLO_DEFINE_RESERVOIR_STATE_OPS(OloGIReservoir, OloGISample, OloGIReservoir)"),
                  std::string::npos)
            << "include/ReservoirGI.glsl no longer instantiates the shared state operations";
    }

    // The UBO the four draws share. A drift here is not a compile error on
    // either side — it is every lane after the drift read at the wrong offset,
    // which produces a plausible wrong image.
    TEST(ReSTIRGIContract, ParamsBlockDeclaresTheLanesTheUBOCarries)
    {
        const auto path = ResolveShaderPath("include/ReSTIRGIParams.glsl");
        ASSERT_TRUE(std::filesystem::exists(path)) << path.string();
        const std::string source = ReadTextFile(path);
        ASSERT_FALSE(source.empty());

        for (const char* lane : { "u_InvView", "u_InvProjection", "u_View", "u_PrevInvView",
                                  "u_PrevInvProjection", "u_TlasAddressAndFrame", "u_SlotCounts",
                                  "u_EmissiveTable", "u_MaterialTable", "u_ResamplingCounts",
                                  "u_ReuseParams", "u_EstimatorParams", "u_GIParams", "u_Environment",
                                  "u_PrevOriginDelta", "u_ScreenParams" })
        {
            EXPECT_NE(source.find(lane), std::string::npos) << "missing lane " << lane;
        }

        // Sixteen lanes: five mat4 (320 B), five uvec4 (80 B) and six vec4
        // (96 B). The static_assert in ShaderBindingLayout.h pins the C++ side;
        // this is the half that notices when the GLSL block stops matching it.
        EXPECT_EQ(UBOStructures::ReSTIRGIUBO::GetSize(), 496u);

        // THE PREVIOUS-FRAME PAIR IS READ, unlike the one #1140 deleted from the
        // DI block. If a change ever makes them dead, delete them the same way
        // rather than leaving 128 bytes of uniform describing a reconstruction
        // nothing performs.
        const auto temporalPath = ResolveShaderPath("ReSTIR_GI_TemporalReuse.glsl");
        ASSERT_TRUE(std::filesystem::exists(temporalPath)) << temporalPath.string();
        const std::string temporal = ReadTextFile(temporalPath);
        EXPECT_NE(temporal.find("u_PrevInvProjection"), std::string::npos)
            << "the temporal draw no longer reads the previous projection - either its reconnection "
               "Jacobian regressed to DI's exact 1, or the lane is now dead and should be deleted";
        EXPECT_NE(temporal.find("u_PrevInvView"), std::string::npos);
        EXPECT_NE(temporal.find("u_PrevOriginDelta"), std::string::npos)
            << "the previous shading point is being reconstructed without the render-origin delta, which "
               "is exact at the world origin and wrong by the origin once the grid snaps";
    }

    // The five history planes must be five DIFFERENT keys, and none of them may
    // collide with DI's. A collision is not a crash: it is one tier reading the
    // other's reservoirs, at a layout that happens to be the same shape.
    TEST(ReSTIRGIContract, ReservoirHistoryPlanesAreDistinctFromEachOtherAndFromDIs)
    {
        EXPECT_NE(TemporalHistoryEffect::ReSTIRGI, TemporalHistoryEffect::ReSTIRDI);
        EXPECT_NE(TemporalHistoryPlane::ReservoirSample, TemporalHistoryPlane::ReservoirRadiance);
        EXPECT_NE(TemporalHistoryPlane::ReservoirRadiance, TemporalHistoryPlane::ReservoirState);
        EXPECT_NE(TemporalHistoryPlane::ReservoirState, TemporalHistoryPlane::SurfaceGeometry);
        EXPECT_NE(TemporalHistoryPlane::SurfaceGeometry, TemporalHistoryPlane::MomentsFirst);
    }
} // namespace OloEngine::Tests
