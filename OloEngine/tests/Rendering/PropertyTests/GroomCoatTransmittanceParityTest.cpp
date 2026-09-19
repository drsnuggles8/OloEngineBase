#include "OloEnginePCH.h"

// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GroomCoatTransmittanceParityTest.cpp — pins GroomCoatShadow::CoatTransmittance
// against the REAL COMPILED include/GroomCoatShadowCommon.glsl (issue #1360).
//
// WHY IT EXISTS, AND WHY NOW. GroomCoatShadowCommon.glsl said in its own header
// that there was no GPU parity test for this twin. #1360 then changed the
// transmittance formula on both sides at once — from exp(-kappa * tau) to the
// Poisson generating form exp(-tau * (1 - exp(-kappa))) — and a change applied
// twice by hand is precisely the change that lands on one side only.
//
// The failure that would follow is SILENT. A coat shaded with the old form
// still looks like a coat; every CPU-side number in the suite would keep
// agreeing with itself, because they are all computed from the C++ arm; and the
// visual-evidence A/B compares the coat against itself with the feature off, so
// it cannot see the two arms drifting in the same direction. Nothing else in
// the suite can tell.
//
// SCOPE: the transmittance half of the twin only. The march itself
// (oloGroomCoatOpticalDepth against SampleDensityVolume) still has no probe —
// it needs a 3D volume uploaded and read back identically on both sides, which
// is a larger harness than this slice, and pretending otherwise here would make
// this file's name a lie.
//
// WHY A TOLERANCE AND NOT EXACT EQUALITY. Two exp() calls, one of them nested,
// whose last bits a driver is entitled to compute differently from the host's
// libm — and the C++ arm spells the inner one with expm1 while GLSL, which has
// no expm1, spells it 1 - exp(-kappa). A relative bound of 2e-3 with a small
// absolute floor is the same bar GroomFibreGpuParityTest sets, for the same
// reason: tight enough that a structural divergence (the old formula, a dropped
// term, a swapped argument) cannot hide under it, loose enough to survive a
// different transcendental implementation.
//
// SKIPs cleanly without a GL 4.6 context, like every other GPU test here.
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 64;
        constexpr u32 kHeight = 64;

        constexpr f32 kRelativeTolerance = 2.0e-3f;
        constexpr f32 kAbsoluteFloor = 1.0e-6f;

        // The probe's grid, restated. It must match
        // GroomCoatTransmittanceParityProbe.glsl exactly — that IS the
        // comparison, so it is written out rather than factored away.
        //
        // kappa runs the authored clamp range end to end (0.25 .. 16.0) because
        // that is where the two candidate formulas differ most: at kappa = 16
        // the retired form returns exp(-16 tau) while the shipped one floors at
        // exp(-tau), which are orders of magnitude apart at the top of the tau
        // range.
        struct ProbeArgs
        {
            f64 Tau;
            f32 Kappa;
        };

        [[nodiscard]] ProbeArgs ArgsAt(u32 x, u32 y)
        {
            ProbeArgs args;
            args.Tau = 0.02 * static_cast<f64>(x + 1u);
            args.Kappa = 0.25f * static_cast<f32>(y + 1u);
            return args;
        }

        [[nodiscard]] bool WithinTolerance(f32 shader, f32 expected)
        {
            const f32 delta = std::abs(shader - expected);
            return delta <= std::max(kAbsoluteFloor, std::abs(expected) * kRelativeTolerance);
        }

        struct CoatProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            CoatProbeHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                // RGBA32F: a half-float readback would quantise away exactly
                // the drift this exists to catch, and the low corner of the
                // grid is where the two forms differ least.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/GroomCoatTransmittanceParityProbe.glsl");
            }

            void Draw()
            {
                GLStateGuard guard("GroomCoatTransmittanceParity::Draw", GLStateGuard::Policy::Restore);
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
    } // namespace

    TEST(GroomCoatTransmittanceParity, TheCompiledShaderMatchesTheCppTwin)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        CoatProbeHarness harness;
        harness.Draw();
        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

        u32 mismatches = 0;
        u32 reported = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * kWidth + x) * 4u;
                const ProbeArgs args = ArgsAt(x, y);

                // THE ARGUMENTS FIRST. The shader echoes back what it was
                // handed, so a disagreement below is unambiguously "the two
                // compute different functions" rather than "the two were given
                // different numbers" — which are the two ways this can fail and
                // they need different fixes.
                ASSERT_NEAR(pixels[i + 1], static_cast<f32>(args.Tau), 1.0e-5f)
                    << "the probe grid disagrees about tau at (" << x << ", " << y << ")";
                ASSERT_NEAR(pixels[i + 2], args.Kappa, 1.0e-5f)
                    << "the probe grid disagrees about kappa at (" << x << ", " << y << ")";

                // THE NON-FINITE CASE, which no point on a finite grid reaches
                // and which is the one input where a disagreement matters most.
                // Rule 10 of groom-coat-self-shadowing.md: a corrupt optical
                // depth reads FULLY LIT, because a bright coat is visibly "this
                // did not run" while a black one is indistinguishable from a
                // correct silhouette. The C++ arm has always rejected an
                // infinity; the shader passed it straight into exp() and
                // returned black until #1360.
                // Compared against the C++ twin, NOT against a literal 1.0f.
                // The point of this file is that the two agree; hard-coding the
                // expected answer would leave the one input it calls most
                // important as the one input the twins are not compared on.
                EXPECT_FLOAT_EQ(pixels[i + 3],
                                GroomCoatShadow::CoatTransmittance(std::numeric_limits<f64>::infinity(),
                                                                   args.Kappa))
                    << "an infinite optical depth read " << pixels[i + 3] << " on the GPU at kappa " << args.Kappa
                    << ", where the C++ twin reads fully lit";

                const f32 expected = GroomCoatShadow::CoatTransmittance(args.Tau, args.Kappa);
                if (WithinTolerance(pixels[i], expected))
                {
                    continue;
                }
                ++mismatches;
                if (reported < 8u)
                {
                    ++reported;
                    ADD_FAILURE() << "coat transmittance drift at tau " << args.Tau << ", kappa " << args.Kappa
                                  << ": shader " << pixels[i] << " vs C++ " << expected;
                }
            }
        }

        EXPECT_EQ(mismatches, 0u)
            << mismatches << " of " << (kWidth * kHeight)
            << " samples disagree; GroomCoatShadow::CoatTransmittance and oloGroomCoatTransmittance are "
               "attenuating the coat differently, so the CPU comparison in "
               "docs/analysis/groom-coat-self-shadowing-1248.md describes a coat that is not on screen";
    }

    TEST(GroomCoatTransmittanceParity, TheGridWouldRejectTheFormulaThatWasReplaced)
    {
        // WITHOUT THIS, THE TEST ABOVE COULD BE ASSERTING A COINCIDENCE. Two
        // formulas that happen to agree everywhere the grid looks would pass it
        // while proving nothing, and these two DO agree closely at the bottom
        // corner: at kappa = 0.25 and tau = 0.02 the retired exp(-kappa * tau)
        // and the shipped exp(-tau * (1 - exp(-kappa))) differ by well under
        // the tolerance above.
        //
        // So this asserts that the grid REACHES the region where they do not.
        // It runs on the CPU alone and needs no GPU: what it checks is a
        // property of the sample points, not of the shader.
        u32 discriminating = 0;
        f32 largestSeparation = 0.0f;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const ProbeArgs args = ArgsAt(x, y);
                const f32 shipped = GroomCoatShadow::CoatTransmittance(args.Tau, args.Kappa);
                const f32 retired = static_cast<f32>(std::exp(-static_cast<f64>(args.Kappa) * args.Tau));
                if (!WithinTolerance(retired, shipped))
                {
                    ++discriminating;
                }
                largestSeparation = std::max(largestSeparation, std::abs(shipped - retired));
            }
        }

        // A MAJORITY, not merely one point: a grid that only just reached the
        // discriminating region would pass the test above on a shader that was
        // wrong nearly everywhere.
        EXPECT_GT(discriminating, (kWidth * kHeight) / 2u)
            << discriminating << " of " << (kWidth * kHeight)
            << " grid points can tell the two formulas apart, which is too few for the parity test above to "
               "mean anything";
        EXPECT_GT(largestSeparation, 0.2f) << "largest separation over the grid = " << largestSeparation;
    }
} // namespace OloEngine::Tests
