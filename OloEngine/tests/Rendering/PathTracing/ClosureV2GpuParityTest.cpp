// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ClosureV2GpuParityTest.cpp — pins the PBR CLOSURE V2 Evaluate/Pdf pair in
// PBRCommon.glsl against their C++ twins (issue #975).
//
// THE CONTRACT THIS FILE DEFENDS
// -------------------------------
// The v2 closure ships as a mirrored triple: closureV2Evaluate /
// closureV2SampleBRDF / closureV2Pdf in PBRCommon.glsl, and ClosureV2Evaluate
// (ReferenceBRDF.h) / BSDF::Sample / BSDF::Pdf (PBRClosureBSDF.h) on the CPU.
// The reference path tracer integrates the C++ side; the raster path (and any
// future GPU sampler / ReSTIR pass) shades the GLSL side. "A divergence
// between raster and reference IS the bug" is only true while the two agree —
// a hand transcription drifts, and nothing downstream can detect it: the
// reference still converges beautifully, against a closure the renderer no
// longer uses.
//
// So: render PbrClosureV2ParityProbe.glsl — which calls the production
// closureV2Evaluate and closureV2Pdf directly — over a
// (roughness x metallic x NdotL) grid, read it back, and evaluate the C++
// twins on the identical grid. Any disagreement beyond fp32 evaluation-order
// noise fails here, immediately, with the offending parameters named.
//
// This is the shaderpipe sibling of the headless ClosureV2 tests (which pin
// the C++ closure's mathematical properties — furnace, reciprocity,
// Sample/Pdf agreement). Neither subsumes the other: this test cannot tell
// you the mixture density is the wrong measure, and those cannot tell you the
// two languages have diverged. Unlike the Legacy probe
// (ReferenceBRDFGpuParityTest), this one pins the DENSITY across the language
// boundary too — Legacy GLSL has no PDF, so that drift axis simply could not
// be guarded before v2.
//
// SKIPs cleanly without a GL 4.6 context, like every other GPU test here.
//
// Classification: shaderpipe (compiles + runs a production shader, compares
// against CPU math).
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/PathTracing/PBRClosureBSDF.h"
#include "OloEngine/Renderer/PathTracing/ReferenceBRDF.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"
#include "OloEngine/Renderer/Shader.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::PathTracing;

    namespace
    {
        // Must match METALLIC_STEPS in PbrClosureV2ParityProbe.glsl. A mismatch
        // would silently compare different metallic values on the two sides —
        // so the test asserts the decoded band count against the shader's own
        // constant by construction (both are 4 and both are named here).
        constexpr u32 kMetallicSteps = 4;

        constexpr u32 kWidth = 128;  // roughness axis
        constexpr u32 kHeight = 128; // packed (metallic band, NdotL) axis

        // Minimal fullscreen-draw harness, mirroring
        // ReferenceBRDFGpuParityTest's ParityProbeHarness. Deliberately a local
        // copy rather than a shared header: this file must be able to state
        // exactly what GL state its comparison ran under.
        struct ParityProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            ParityProbeHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                // RGBA32F, not 16F: a half-float readback would blur the
                // comparison to ~1e-3 relative and hide exactly the kind of
                // small constant-factor drift this test exists to catch.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/PbrClosureV2ParityProbe.glsl");
            }

            // Returns false on a resource-creation failure (a missing probe
            // shader or an FBO the driver refused). A gtest ASSERT here would
            // only exit THIS helper — the caller would then null-deref in
            // ReadOutput() — so the status propagates and every call site
            // ASSERT_TRUEs it before reading anything back.
            [[nodiscard]] bool Draw()
            {
                if (m_OutputFB == nullptr || m_Shader == nullptr)
                {
                    ADD_FAILURE() << (m_OutputFB == nullptr
                                          ? "parity probe framebuffer was not created"
                                          : "PbrClosureV2ParityProbe.glsl failed to load/compile");
                    return false;
                }

                GLStateGuard guard("ClosureV2GpuParity::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(0);
                ::glFinish();
                m_OutputFB->Unbind();
                return true;
            }

            void ReadOutput(std::vector<f32>& out) const
            {
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            }
        };

        // The C++ side of the identical grid. Mirrors the shader's decode
        // exactly — including the pixel-centre offsets, which is what keeps the
        // two evaluating the SAME parameter values rather than two grids that
        // merely span the same ranges.
        struct GridPoint
        {
            f32 Roughness = 0.0f;
            f32 Metallic = 0.0f;
            glm::vec3 L{ 0.0f };
        };

        [[nodiscard]] GridPoint DecodeGridPoint(u32 x, u32 y)
        {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kHeight);

            GridPoint point;
            point.Roughness = std::clamp(u, 0.0f, 1.0f);

            const f32 scaled = std::clamp(v, 0.0f, 1.0f) * static_cast<f32>(kMetallicSteps);
            const f32 band = std::min(std::floor(scaled), static_cast<f32>(kMetallicSteps - 1));
            const f32 withinBand = scaled - band;

            point.Metallic = band / static_cast<f32>(kMetallicSteps - 1);

            const f32 theta = withinBand * (kPi * 0.5f);
            point.L = glm::vec3(std::sin(theta), 0.0f, std::cos(theta));
            return point;
        }

        // The v2 material every texel's C++ Pdf is computed against — only
        // metallic/roughness vary per grid point. Albedo must match the probe's
        // hardcoded (0.9, 0.6, 0.3) or the specular-lobe probability (a
        // luminance ratio over F0 and diffuse albedo) diverges by construction.
        [[nodiscard]] ReferenceMaterial MakeV2Material(const GridPoint& point)
        {
            ReferenceMaterial material;
            material.BaseColor = glm::vec3(0.9f, 0.6f, 0.3f);
            material.Metallic = point.Metallic;
            material.Roughness = point.Roughness;
            material.Model = PBRModel::ClosureV2;
            return material;
        }
    } // namespace

    TEST(ClosureV2GpuParity, V2EvaluateMatchesCppTwin)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ParityProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 v(0.0f, 0.0f, 1.0f);
        const glm::vec3 albedo(0.9f, 0.6f, 0.3f);

        // Tolerance model: the two implementations run the SAME arithmetic in
        // a different order on different hardware, so exact equality is not
        // available. A relative tolerance with an absolute floor is the right
        // shape — the closure spans several orders of magnitude across this
        // grid (the alpha-clamped near-mirror lobe is BRIGHT by design, versus
        // a grazing dielectric's diffuse floor), so a pure absolute bound would
        // be meaninglessly loose at the top and impossible at the bottom.
        //
        // 1% relative is roughly two orders of magnitude tighter than any
        // structural change to the closure: a swapped visibility term moves
        // values by tens of percent, a dropped 1/pi by ~3x, a missing
        // multi-scatter lobe by up to ~20% at high roughness, an alpha clamp
        // applied on one side only by orders of magnitude near roughness 0. It
        // is loose enough that fp32 reassociation, a driver's fast-math
        // `inversesqrt`, and the bilinear energy-table lookups (identical
        // tables, different interpolation order) cannot trip it.
        constexpr f32 kRelativeTolerance = 0.01f;
        constexpr f32 kAbsoluteFloor = 1e-4f;

        u32 mismatches = 0;
        f32 worstRelative = 0.0f;
        u32 worstX = 0;
        u32 worstY = 0;

        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const GridPoint point = DecodeGridPoint(x, y);
                const glm::vec3 expected =
                    ClosureV2Evaluate(n, v, point.L, albedo, point.Metallic, point.Roughness);

                const sizet base = (static_cast<sizet>(y) * kWidth + x) * 4;
                const glm::vec3 actual(pixels[base + 0], pixels[base + 1], pixels[base + 2]);

                for (glm::length_t channel = 0; channel < 3; ++channel)
                {
                    ASSERT_TRUE(std::isfinite(actual[channel]))
                        << "GPU produced a non-finite v2 closure value at (" << x << ", " << y << ")";

                    const f32 difference = std::abs(actual[channel] - expected[channel]);
                    const f32 scale = std::max({ std::abs(expected[channel]), std::abs(actual[channel]), kAbsoluteFloor });
                    const f32 relative = difference / scale;
                    if (relative > worstRelative)
                    {
                        worstRelative = relative;
                        worstX = x;
                        worstY = y;
                    }
                    if (relative > kRelativeTolerance && difference > kAbsoluteFloor)
                        ++mismatches;
                }
            }
        }

        const GridPoint worstPoint = DecodeGridPoint(worstX, worstY);
        const glm::vec3 worstExpected =
            ClosureV2Evaluate(n, v, worstPoint.L, albedo, worstPoint.Metallic, worstPoint.Roughness);
        const sizet worstBase = (static_cast<sizet>(worstY) * kWidth + worstX) * 4;

        EXPECT_EQ(mismatches, 0u)
            << mismatches << " of " << (kWidth * kHeight * 3) << " samples disagree.\n"
            << "Worst at roughness = " << worstPoint.Roughness << ", metallic = " << worstPoint.Metallic
            << ", NdotL = " << worstPoint.L.z << " (relative " << worstRelative << ")\n"
            << "  GLSL closureV2Evaluate = (" << pixels[worstBase + 0] << ", " << pixels[worstBase + 1] << ", "
            << pixels[worstBase + 2] << ")\n"
            << "  C++  ClosureV2Evaluate = (" << worstExpected.x << ", " << worstExpected.y << ", "
            << worstExpected.z << ")\n"
            << "The PBR CLOSURE V2 section of PBRCommon.glsl and the ClosureV2* twins in\n"
            << "Renderer/PathTracing/ReferenceBRDF.h have DRIFTED. Until they are reconciled the\n"
            << "reference path tracer is validating v2 materials against a closure the renderer no\n"
            << "longer shades — every raster-vs-reference comparison on them is meaningless.";
    }

    // The alpha channel carries closureV2Pdf, compared through the production
    // dispatch (BSDF::Pdf with Model == ClosureV2) rather than a re-derived
    // formula — so a drift in EITHER the density math (PdfGGXVNDF, the VNDF
    // Jacobian, the clamped roughness) OR the lobe mixture
    // (SpecularLobeProbability vs closureV2SpecularProbability) fails here.
    //
    // This pins the DENSITY across the language boundary, which the Legacy
    // probe never could: Legacy GLSL has no PDF at all, so a GPU sampler had
    // nothing to agree with. v2's whole contract is that Evaluate, Sample and
    // Pdf share one D — this test is the cross-language half of that promise
    // (the headless tests pin Sample()'s reported Pdf == Pdf() on the C++
    // side; GLSL's closureV2SampleBRDF calls closureV2Pdf directly, so pinning
    // closureV2Pdf pins the sampler's density too).
    //
    // A separate test rather than extra channels in the one above so that a
    // failure names WHICH function drifted — a wrong Evaluate and a wrong Pdf
    // have completely different causes and completely different fixes (a wrong
    // Pdf doesn't even change the rendered image of a unidirectional tracer,
    // only its variance and its MIS weights — the classic silent bias).
    TEST(ClosureV2GpuParity, V2PdfMatchesCppTwin)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ParityProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        const glm::vec3 n(0.0f, 0.0f, 1.0f);
        const glm::vec3 v(0.0f, 0.0f, 1.0f);

        // Same tolerance model as the Evaluate comparison above, and for the
        // same reason: the mixture density spans orders of magnitude between
        // the near-mirror VNDF spike and the grazing cosine lobe, so
        // relative-with-an-absolute-floor is the only shape that is neither
        // vacuous at the top nor impossible at the bottom. 1% relative sits
        // far below any structural drift (a G1 dropped from the VNDF density
        // is tens of percent, a mixture weight clamped on one side only is a
        // step change) and far above fp32 evaluation-order noise.
        constexpr f32 kRelativeTolerance = 0.01f;
        constexpr f32 kAbsoluteFloor = 1e-4f;

        u32 mismatches = 0;
        f32 worstRelative = 0.0f;
        u32 worstX = 0;
        u32 worstY = 0;

        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const GridPoint point = DecodeGridPoint(x, y);
                const f32 expected = BSDF::Pdf(MakeV2Material(point), n, v, point.L);
                const f32 actual = pixels[(static_cast<sizet>(y) * kWidth + x) * 4 + 3];

                ASSERT_TRUE(std::isfinite(actual))
                    << "GPU produced a non-finite v2 pdf at (" << x << ", " << y << ")";

                const f32 difference = std::abs(actual - expected);
                const f32 scale = std::max({ std::abs(expected), std::abs(actual), kAbsoluteFloor });
                const f32 relative = difference / scale;
                if (relative > worstRelative)
                {
                    worstRelative = relative;
                    worstX = x;
                    worstY = y;
                }
                if (relative > kRelativeTolerance && difference > kAbsoluteFloor)
                    ++mismatches;
            }
        }

        const GridPoint worstPoint = DecodeGridPoint(worstX, worstY);
        EXPECT_EQ(mismatches, 0u)
            << mismatches << " of " << (kWidth * kHeight) << " samples disagree.\n"
            << "Worst at roughness = " << worstPoint.Roughness << ", metallic = " << worstPoint.Metallic
            << ", NdotL = " << worstPoint.L.z << " (relative " << worstRelative << ")\n"
            << "  GLSL closureV2Pdf = " << pixels[(static_cast<sizet>(worstY) * kWidth + worstX) * 4 + 3] << "\n"
            << "  C++  BSDF::Pdf    = " << BSDF::Pdf(MakeV2Material(worstPoint), n, v, worstPoint.L) << "\n"
            << "The v2 density has DRIFTED between PBRCommon.glsl's closureV2Pdf and the C++ twins\n"
            << "(PBRClosureBSDF.h BSDF::Pdf / ReferenceBRDF.h PdfGGXVNDF). A GPU sampler dividing by\n"
            << "one density while the reference MIS-weights with the other is silently biased —\n"
            << "check the mixture weight (closureV2SpecularProbability) and the clamped roughness\n"
            << "(closureV2Roughness must feed sampler, density AND evaluation alike) first.";
    }

    // The probe must actually exercise the interesting parts of the domain; a
    // grid that came back all-zero (a shader that failed to compile and left a
    // cleared target) would make both parity tests above pass vacuously.
    TEST(ClosureV2GpuParity, V2ProbeGridCoversANonTrivialRange)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ParityProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        f32 minimum = std::numeric_limits<f32>::max();
        f32 maximumRgb = 0.0f;
        f32 maximumAlpha = 0.0f;
        for (sizet i = 0; i < static_cast<sizet>(kWidth) * kHeight; ++i)
        {
            for (u32 channel = 0; channel < 3; ++channel)
            {
                const f32 value = pixels[i * 4 + channel];
                minimum = std::min(minimum, value);
                maximumRgb = std::max(maximumRgb, value);
            }
            const f32 alpha = pixels[i * 4 + 3];
            minimum = std::min(minimum, alpha);
            maximumAlpha = std::max(maximumAlpha, alpha);
        }

        // Not `>= 0`: `kD * albedo / pi + specular` computes kD as `1 - F`, and
        // at grazing angles F rounds a hair above 1 in fp32, so the GPU
        // legitimately returns values like -2e-9. That is rounding, not a
        // negative closure — the bound is set where a real sign error (a
        // flipped visibility term, an inverted Fresnel) would land, orders of
        // magnitude away. The pdf in alpha is non-negative by construction, so
        // it can only tighten this bound, never loosen it.
        EXPECT_GE(minimum, -1e-5f) << "the v2 closure or pdf returned a meaningfully negative value";
        EXPECT_GT(maximumRgb, 0.5f)
            << "the probe's rgb channels are uniformly near zero — the shader almost certainly "
               "failed to compile, which would make V2EvaluateMatchesCppTwin pass vacuously";
        EXPECT_GT(maximumAlpha, 0.1f)
            << "the probe's alpha channel is uniformly near zero — the shader almost certainly "
               "failed to compile, which would make V2PdfMatchesCppTwin pass vacuously";
    }
} // namespace OloEngine::Tests
