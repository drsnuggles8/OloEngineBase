#include "OloEnginePCH.h"

// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// MaterialReferenceAovParityTest — issue #1255's THIRD acceptance criterion:
// aligned reference/production AOV comparisons across representative parameter
// ranges and light directions.
//
// WHAT "ALIGNED" MEANS HERE, and why it is stronger than a screenshot A/B.
// Two images can only be compared if the camera, the exposure and the tonemap
// state are identical; get any of the three wrong and the comparison measures
// the setup. The strongest available answer to that is to compare an AOV that
// has NONE of the three: these tests render the production shading functions
// into an RGBA32F target with no camera, no exposure and no tonemap, and
// compare the LINEAR shading values texel for texel against the independent
// references in PathTracing/MaterialReference.h.
//
// That is a deliberate choice over a rendered beauty A/B, and the reason is in
// the memory of this repo as well as in the issue: a ratio of tone-mapped sums
// reads backwards, and evidence PNG shading depends on test order to the tune
// of thousands of pixels. An AOV in linear float has neither problem, and its
// alignment is a property of the setup rather than a claim about it.
//
// WHAT EACH TEST COVERS, and what the axes are:
//
//   1. FIBRE AMBIENT AOV vs the cylinder walk. The probe sweeps sin(theta_o)
//      from -0.95 to 0.95 — a full range of LIGHT DIRECTIONS relative to the
//      strand — and emits all four lobes. The comparison is against
//      MaterialReference's cylinder walk, so it is the shipped SHADER against
//      an independent oracle, which neither GroomFibreGpuParityTest (shader vs
//      the C++ twin) nor GroomFibrePropertyTests (twin vs its own converged
//      quadrature) provides.
//
//   2. LEAF SWEEP AOV vs the transcription. 64 x 64 view and light directions,
//      two authored profiles and two controls. This is the test that closes the
//      seam LeafTransmissionReferenceTest opens: from here on, that file's
//      statements about "the production leaf lobe" are statements about the
//      compiled shader.
//
//   3. LEAF SWEEP AOV vs the SLAB's predictions. The orderings the independent
//      slab reference makes — forward scattering beats back scattering, a
//      broader authored lobe is flatter across the view hemisphere — asserted
//      on the SHADER's own output rather than on the transcription. That is the
//      reference judging the pixels.
//
// WHAT IS NOT HERE, said rather than left as a gap. The skin diffusion pass has
// no shading function on the GPU to probe: SkinDiffusion.glsl consumes a tap
// table the CPU built, so the profile arithmetic never runs on the device and
// there is no AOV of it to capture. Its reference comparison is therefore
// CPU-side, in NonlocalTransportReferenceTest, and that is a property of where
// the code lives rather than a hole in this file.
//
// Classification: shaderpipe — compiles and runs production shader includes and
// compares against CPU math, the same category as GroomFibreGpuParityTest.
// SKIPs cleanly without a GL 4.6 context.
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#include "PathTracing/MaterialReference.h"
#include "PathTracing/ProductionLeafLobeMirror.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomFibreScattering.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/FoliageLeafProfile.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // NOT `Ref`: OloEngine::Ref<T> is the engine's smart pointer and this
        // file holds framebuffers in it.
        namespace MatRef = MaterialReference;

        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        constexpr f64 kPi = 3.14159265358979323846;

        // A small harness, deliberately the same shape as
        // GroomFibreGpuParityTest's: RGBA32F, no blend, no depth, no cull, one
        // fullscreen draw and a float readback. RGBA32F rather than RGBA16F
        // because these are linear shading values and a half-float readback
        // would quantise away exactly the drift the comparison is for.
        struct ProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            explicit ProbeHarness(const char* shaderPath)
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                GLStateGuard guard("MaterialReferenceAovParity::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(0);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            void ReadOutput(std::vector<f32>& out) const
            {
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            }
        };

        [[nodiscard]] bool WithinTolerance(f64 shader, f64 expected, f64 relative, f64 absoluteFloor)
        {
            return std::abs(shader - expected) <= std::max(absoluteFloor, std::abs(expected) * relative);
        }

        // ---------------------------------------------------------------------
        // The leaf sweep's grid, restated. It MUST match
        // assets/shaders/tests/ShaderUnit_FoliageTransmissionSweep.glsl exactly;
        // that restatement is the comparison, so it is written out on both sides
        // rather than factored into something one side could change alone.
        // ---------------------------------------------------------------------
        struct LeafSweepSample
        {
            glm::dvec3 N{ 0.0, 0.0, 1.0 };
            glm::dvec3 V{ 0.0 };
            glm::dvec3 LBack{ 0.0 };
            glm::dvec3 LFront{ 0.0 };
            f64 MuV = 0.0;
        };

        [[nodiscard]] LeafSweepSample LeafSweepAt(u32 x, u32 y)
        {
            LeafSweepSample sample;
            const f64 muV = (static_cast<f64>(x) + 0.5) / 64.0;
            const f64 muL = (static_cast<f64>(y) + 0.5) / 64.0;
            const f64 phiL = kPi * static_cast<f64>((5u * x + 11u * y) % 64u) / 63.0;

            sample.MuV = muV;
            sample.V = glm::dvec3(std::sqrt(std::max(0.0, 1.0 - (muV * muV))), 0.0, muV);

            const f64 sinL = std::sqrt(std::max(0.0, 1.0 - (muL * muL)));
            sample.LBack = glm::dvec3(sinL * std::cos(phiL), sinL * std::sin(phiL), -muL);
            sample.LFront = glm::dvec3(sinL * std::cos(phiL), sinL * std::sin(phiL), muL);
            return sample;
        }

        constexpr f64 kLeafThickness = 0.75;

        [[nodiscard]] FoliageLeafProfile LeafProfileA()
        {
            FoliageLeafProfile profile;
            profile.TransmissionStrength = 1.0f;
            profile.Distortion = 0.35f;
            profile.Power = 4.0f;
            profile.Wrap = 0.50f;
            profile.Ambient = 0.35f;
            return profile;
        }

        // The same leaf with the exponent dropped, and ONLY the exponent — see
        // the probe's header for why one axis at a time is what the orderings
        // need.
        [[nodiscard]] FoliageLeafProfile LeafProfileB()
        {
            FoliageLeafProfile profile = LeafProfileA();
            profile.Power = 1.5f;
            return profile;
        }

        // The fibre ambient probe's material, from the AUTHORED side. The probe
        // hard-codes the DERIVED sigma_a; building it here with the production
        // fit rather than pasting the literals means the derivation is part of
        // what the comparison covers.
        [[nodiscard]] GroomFibreParams AmbientProbeParams()
        {
            GroomFibreAuthoring authored;
            authored.PigmentMode = GroomFibrePigmentMode::Melanin;
            authored.Eumelanin = 1.3f;
            authored.Pheomelanin = 0.0f;
            authored.LongitudinalRoughness = 0.3f;
            authored.AzimuthalRoughness = 0.3f;
            authored.TiltDegrees = 2.0f;
            authored.IndexOfRefraction = kGroomFibreDefaultIOR;
            authored.Intensity = 1.0f;
            authored.HSamples = 4;
            return MakeGroomFibreParams(authored);
        }
    } // namespace

    // =========================================================================
    // 1. Fibre — the shipped shader's ambient AOV against the cylinder walk.
    // =========================================================================

    TEST(MaterialReferenceAovParity, FibreAmbientAovMatchesTheCylinderWalk)
    {
        // THE PROBE IS ALREADY THERE, and reusing it rather than adding a
        // parallel one is the point: GroomFibreAmbientParityProbe.glsl emits
        // oloGroomFibreAmbientResponse's four lobes over a sweep of
        // sin(theta_o), which is the fibre's response to a unit environment and
        // therefore — divided by cos(theta_o) — each path's MEAN ATTENUATION
        // over the fibre's width. That is exactly the quantity the independent
        // cylinder walk computes.
        //
        // So this is the shipped GLSL against an oracle that shares no code with
        // it, over 64 light directions, in a linear AOV. A sign error in
        // Chiang's recurrence, a wrong Bravais index or a dropped tail would
        // show here and nowhere else in the suite.
        OLO_ENSURE_GPU_OR_SKIP();

        ProbeHarness harness("assets/shaders/tests/GroomFibreAmbientParityProbe.glsl");
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        const GroomFibreParams params = AmbientProbeParams();
        const f64 eta = static_cast<f64>(params.Eta);
        const glm::dvec3 sigmaA(params.SigmaA);

        // 4e-3 relative, 1e-6 absolute floor. The shader runs f32 transcendentals
        // through a four-node quadrature and the reference runs f64 ones through
        // a 48-term series; 0.4% is comfortably above that and decades below any
        // structural divergence, which moves these values by tens of percent.
        constexpr f64 kRelative = 4.0e-3;
        constexpr f64 kAbsolute = 1.0e-6;

        const std::array<const char*, 4> names{ "R", "TT", "TRT", "residual" };
        u32 mismatches = 0;
        u32 reported = 0;

        for (u32 x = 0; x < kWidth; ++x)
        {
            const f64 sinThetaO = -0.95 + ((1.90 * static_cast<f64>(x)) / 63.0);
            const f64 cosThetaO = std::sqrt(std::max(0.0, 1.0 - (sinThetaO * sinThetaO)));

            // The probe's HSamples is 4 — the shipped quadrature order — so the
            // reference averages over the same four nodes. Using a converged
            // reference here would fold the quadrature's error into the
            // comparison and report it as a model disagreement.
            const std::array<glm::dvec3, 4> reference = MatRef::FibreFoldedAttenuationMean(sinThetaO, eta, sigmaA, 4);

            // Row 0 is enough for the value, but the probe varies y and the
            // ambient response does not depend on it, so every row is checked:
            // a shader that accidentally read the row would show as a
            // row-to-row difference the reference does not predict.
            for (u32 y = 0; y < kHeight; ++y)
            {
                const sizet i = ((static_cast<sizet>(y) * kWidth) + x) * 4u;
                for (u32 p = 0; p < 4u; ++p)
                {
                    // The probe writes the GREEN channel of each lobe, and the
                    // ambient response carries cos(theta_o).
                    const f64 expected = reference[p].y * cosThetaO;
                    if (WithinTolerance(static_cast<f64>(pixels[i + p]), expected, kRelative, kAbsolute))
                        continue;

                    ++mismatches;
                    if (reported < 8u)
                    {
                        ++reported;
                        ADD_FAILURE() << "ambient " << names[p] << " AOV at (" << x << ", " << y << "), sinThetaO "
                                      << sinThetaO << ": shader " << pixels[i + p] << " vs cylinder walk " << expected;
                    }
                }
            }
        }
        EXPECT_EQ(mismatches, 0u) << mismatches << " of " << (kWidth * kHeight * 4u)
                                  << " ambient AOV samples disagreed with the independent cylinder walk";
    }

    // =========================================================================
    // 2. Leaf — the shipped shader against the transcription that judges it.
    // =========================================================================

    TEST(MaterialReferenceAovParity, LeafTransmissionAovMatchesTheTranscription)
    {
        // THE SEAM-CLOSING TEST. LeafTransmissionReferenceTest's independent
        // slab reference judges ProductionLeafLobeMirror.h, a C++ transcription
        // of oloFoliageTransmissionDirect, because an L1 test cannot run a
        // shader. That transcription is a substitution, and a substitution with
        // no keeper is how a test suite goes green while the shipped shader
        // drifts away from everything it claims.
        //
        // This is the keeper: 4096 texels of the REAL compiled function against
        // the transcription, over two authored profiles and two controls.
        OLO_ENSURE_GPU_OR_SKIP();

        ProbeHarness harness("assets/shaders/tests/ShaderUnit_FoliageTransmissionSweep.glsl");
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        const FoliageLeafProfile profileA = LeafProfileA();
        const FoliageLeafProfile profileB = LeafProfileB();

        // 2e-3 relative with a 1e-6 floor. The lobe is a pow() of a dot product,
        // and a driver's pow is entitled to differ from the host's libm in its
        // last bits; 0.2% is the same bar GroomFibreGpuParityTest sets for the
        // same reason.
        constexpr f64 kRelative = 2.0e-3;
        constexpr f64 kAbsolute = 1.0e-6;

        u32 mismatches = 0;
        u32 reported = 0;
        f64 largestBacklitValue = 0.0;

        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet i = ((static_cast<sizet>(y) * kWidth) + x) * 4u;
                const LeafSweepSample sample = LeafSweepAt(x, y);

                const std::array<f64, 4> expected{
                    ProductionLeafLobe(sample.N, sample.V, sample.LBack, kLeafThickness, profileA),
                    ProductionLeafLobe(sample.N, sample.V, sample.LBack, kLeafThickness, profileB),
                    ProductionLeafLobe(sample.N, sample.V, sample.LFront, kLeafThickness, profileA),
                    ProductionLeafLobe(sample.N, sample.V, sample.LBack, 0.0, profileA),
                };
                const std::array<const char*, 4> names{ "profile A backlit", "profile B backlit",
                                                        "profile A front-lit", "thickness 0" };

                largestBacklitValue = std::max(largestBacklitValue, static_cast<f64>(pixels[i]));

                for (u32 c = 0; c < 4u; ++c)
                {
                    if (WithinTolerance(static_cast<f64>(pixels[i + c]), expected[c], kRelative, kAbsolute))
                        continue;
                    ++mismatches;
                    if (reported < 8u)
                    {
                        ++reported;
                        ADD_FAILURE() << names[c] << " drift at (" << x << ", " << y << "): shader " << pixels[i + c]
                                      << " vs transcription " << expected[c];
                    }
                }
            }
        }

        EXPECT_EQ(mismatches, 0u) << mismatches << " of " << (kWidth * kHeight * 4u)
                                  << " leaf AOV samples disagreed with the transcription";
        // A probe that returned zero everywhere would also report no mismatches
        // if the transcription were zero too. It is not, but the sweep has to
        // say so: a test that cannot fail is not evidence.
        EXPECT_GT(largestBacklitValue, 0.1) << "the sweep produced no transmission anywhere, so it proved nothing";
    }

    TEST(MaterialReferenceAovParity, LeafTransmissionAovObeysTheSlabsOrderings)
    {
        // THE REFERENCE JUDGING THE PIXELS. The independent slab reference makes
        // two structural claims that survive the absence of any parameter fit
        // between it and the phenomenological lobe (see
        // LeafTransmissionReferenceTest for why no fit exists). Both are
        // asserted here on the SHADER's own output:
        //
        //   FORWARD SCATTERING. A lamina transmits far more towards an eye
        //   roughly in line with the light behind it than towards one on the
        //   light's own side. The slab has this because scattering is forward
        //   biased; the production lobe has it because that is what it was built
        //   for. A term that lost it would be an ambient fill, which is #1234's
        //   named failure and #1255's reference confirming it independently.
        //
        //   BROADER IS FLATTER. The slab's lobe broadens with optical thickness
        //   and the production lobe broadens with a lower exponent, so profile B
        //   (power 1.5) must vary less across the view hemisphere than profile A
        //   (power 4.0). That is the one monotone correspondence between an
        //   authored parameter and a physical one, and it is measured rather
        //   than asserted from the shapes.
        OLO_ENSURE_GPU_OR_SKIP();

        ProbeHarness harness("assets/shaders/tests/ShaderUnit_FoliageTransmissionSweep.glsl");
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        const auto at = [&pixels](u32 x, u32 y, u32 channel)
        { return static_cast<f64>(pixels[(((static_cast<sizet>(y) * kWidth) + x) * 4u) + channel]); };

        // Thickness 0 is exactly 0 everywhere. Not "small": the shader returns
        // early, so anything else means the early-out is gone.
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
                ASSERT_EQ(at(x, y, 3), 0.0) << "thickness 0 transmitted at (" << x << ", " << y << ")";
        }

        // Forward scattering, summed over the whole sweep so the claim is about
        // the lobe and not about one lucky direction.
        f64 backlitTotal = 0.0;
        f64 frontlitTotal = 0.0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                backlitTotal += at(x, y, 0);
                frontlitTotal += at(x, y, 2);
            }
        }
        EXPECT_GT(backlitTotal, 3.0 * frontlitTotal)
            << "backlit total " << backlitTotal << " vs front-lit total " << frontlitTotal;

        // Broader is flatter — the one monotone correspondence between an
        // authored parameter and a physical one, measured on the shader's own
        // output.
        //
        // THE METRIC HAS TO BE FLOOR-FREE, and the first version of it was not.
        // It divided the row's peak-to-trough range by the row's mean, which the
        // wrap term's constant offset inflates — so it reported the NARROW
        // profile as the flatter one purely because that profile had the higher
        // wrap. The metric below removes the floor and the peak before it
        // measures anything: normalise the row onto [0, 1] by its own minimum
        // and maximum, then take the mean of that. It is the normalised area
        // under the lobe, it is invariant to both an additive floor and a
        // multiplicative gain, and what is left is shape.
        //
        // With the two profiles differing in the exponent alone, both see the
        // same half-vector and therefore the same underlying cosine t at every
        // texel, so the claim reduces to E[t^4] / max(t^4) < E[t^1.5] /
        // max(t^1.5) — true for any distribution of t in [0, 1], which is what
        // makes this an assertion about the model rather than about this grid.
        const auto normalisedArea = [&at](u32 row, u32 channel)
        {
            f64 sum = 0.0;
            f64 lowest = std::numeric_limits<f64>::max();
            f64 highest = 0.0;
            for (u32 x = 0; x < kWidth; ++x)
            {
                const f64 value = at(x, row, channel);
                sum += value;
                lowest = std::min(lowest, value);
                highest = std::max(highest, value);
            }
            const f64 mean = sum / static_cast<f64>(kWidth);
            return (mean - lowest) / std::max(highest - lowest, 1.0e-12);
        };

        // A near-normal backlight row, where both profiles have a well-formed
        // lobe rather than a grazing sliver.
        constexpr u32 kRow = 60;
        const f64 narrow = normalisedArea(kRow, 0);
        const f64 broad = normalisedArea(kRow, 1);
        EXPECT_GT(broad, narrow) << "exponent 4.0 normalised area " << narrow << " vs exponent 1.5 " << broad;
    }
} // namespace OloEngine::Tests
