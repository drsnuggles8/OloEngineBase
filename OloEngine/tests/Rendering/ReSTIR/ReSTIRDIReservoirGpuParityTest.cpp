// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ReSTIRDIReservoirGpuParityTest.cpp — pins include/Reservoir.glsl's PACKING
// against its C++ twin in Renderer/ReSTIR/ReservoirDI.h, across a real RGBA32F
// render target (issue #1140).
//
// THE BUG THIS FILE EXISTS FOR
// ----------------------------
// Reservoir layout v1 stored each integer field as a FLOAT BIT PATTERN. A small
// integer reinterpreted as a float is a DENORMAL — `kind | lightIndex << 3` for
// kind 1, index 50 is 401, which as a bit pattern is 5.6e-43 — and a GPU is
// permitted to flush denormals to zero. This one does. Every reservoir read back
// with sample kind None, every downstream unpack saw an EMPTY reservoir, the
// resolve wrote black, and the whole tier did nothing while `olo_restir_stats`
// reported it active and healthy. There was nothing in OloEngine.log, because
// nothing had failed: the arithmetic was right and the STORE destroyed it.
//
// That is the shape of failure this test is built around, and it dictates the
// design: the probe WRITES the packed lanes into an RGBA32F attachment and this
// file reads them back off the texture. A test that packed and unpacked inside a
// single shader invocation would have passed v1 without noticing, because the
// value never went to memory. The comparison has to span the storage round-trip.
//
// WHAT IT CHECKS
// --------------
//   1. The identity lane survives storage and decodes to the (kind, lightIndex)
//      that went in — including at the TOP of the encodable range, where a
//      bit-pattern encoding stops being representable at all.
//   2. The octahedral normal lane round-trips to within its 12-bit-per-axis
//      resolution, over the FULL SPHERE — the z < 0 fold is a separate code path
//      in both languages and the one most likely to be transcribed wrongly.
//   3. The GLSL layout-version constant equals the C++ one, read through the
//      same kind of lane the reservoir planes use.
//
// This is the shaderpipe sibling of the headless ReSTIRDIContractTest (which
// pins the estimator's arithmetic on a machine with no device) and
// ReSTIRDIOracleTest (which pins it against an analytic and a quadrature
// reference). Neither subsumes this one: they cannot tell you the two languages
// disagree, and they cannot tell you the encoding does not survive a texture.
//
// SKIPs cleanly without a GL 4.6 context, like every other GPU test here.
//
// Classification: shaderpipe (compiles + runs a production shader include,
// compares against CPU math).
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirDI.h"
#include "OloEngine/Renderer/Shader.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Must match INDEX_STEPS / KIND_STEPS in ReSTIRReservoirParityProbe.glsl.
        // A mismatch would silently compare different grids on the two sides, so
        // both constants are named in both files and the decode below mirrors
        // the shader's line for line.
        constexpr u32 kIndexSteps = 256;
        constexpr u32 kKindSteps = 5;
        // The stride the probe multiplies the index band by, to reach the top of
        // the exactly-representable range: 255 * 8191 << 3 + 4 = 16'709'644,
        // just under 2^24, which is where an f32 stops counting by ones.
        constexpr u32 kIndexStride = 8191;

        constexpr u32 kWidth = 256;  // light-index axis
        constexpr u32 kHeight = 250; // packed (kind band, normal direction) axis

        constexpr f32 kPi = 3.14159265358979f;

        // Minimal fullscreen-draw harness, mirroring ClosureV2GpuParityTest's.
        // Deliberately a local copy rather than a shared header: this file must
        // be able to state exactly what GL state its comparison ran under.
        struct ReservoirProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            ReservoirProbeHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                // RGBA32F because that is what the reservoir planes ARE. The
                // point of this test is the fidelity of this exact format, so
                // anything narrower here would be testing a different thing.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/ReSTIRReservoirParityProbe.glsl");
            }

            // Returns false on a resource-creation failure (a missing probe
            // shader or an FBO the driver refused). A gtest ASSERT here would
            // only exit THIS helper and the caller would then null-deref in
            // ReadOutput(), so the status propagates instead.
            [[nodiscard]] bool Draw()
            {
                if (m_OutputFB == nullptr || m_Shader == nullptr)
                {
                    ADD_FAILURE() << (m_OutputFB == nullptr
                                          ? "reservoir parity probe framebuffer was not created"
                                          : "ReSTIRReservoirParityProbe.glsl failed to load/compile");
                    return false;
                }

                GLStateGuard guard("ReSTIRReservoirGpuParity::Draw", GLStateGuard::Policy::Restore);
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

        struct GridPoint
        {
            u32 Kind = 0;
            u32 LightIndex = 0;
            glm::vec3 Normal{ 0.0f };
            f32 Theta = 0.0f;
        };

        // The C++ side of the identical grid. Mirrors the shader's decode
        // exactly — including the pixel-centre offsets, which is what keeps the
        // two evaluating the SAME inputs rather than two grids that merely span
        // the same ranges.
        [[nodiscard]] GridPoint DecodeGridPoint(u32 x, u32 y)
        {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kHeight);

            GridPoint point;
            const f32 indexT = std::clamp(u, 0.0f, 1.0f);
            point.LightIndex =
                static_cast<u32>(std::min(std::floor(indexT * static_cast<f32>(kIndexSteps)),
                                          static_cast<f32>(kIndexSteps - 1))) *
                kIndexStride;

            const f32 scaled = std::clamp(v, 0.0f, 1.0f) * static_cast<f32>(kKindSteps);
            const f32 band = std::min(std::floor(scaled), static_cast<f32>(kKindSteps - 1));
            const f32 withinBand = scaled - band;
            point.Kind = static_cast<u32>(band);

            point.Theta = withinBand * kPi;
            const f32 phi = withinBand * (2.0f * kPi) * 3.0f;
            point.Normal = glm::vec3(std::sin(point.Theta) * std::cos(phi),
                                     std::sin(point.Theta) * std::sin(phi), std::cos(point.Theta));
            return point;
        }
    } // namespace

    // The identity lane, across the storage round-trip. This is the one that
    // failed under v1, and it failed TOTALLY — every value read back as zero —
    // so an exact comparison is the right assertion. The encoding is
    // integer-valued and every value in range is exact in f32, which is the
    // entire reason it is arithmetic rather than a bit cast.
    TEST(ReSTIRDIReservoirGpuParity, IdentityLaneSurvivesAnRGBA32FRoundTrip)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ReservoirProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        u32 mismatches = 0;
        u32 flushedToZero = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const GridPoint point = DecodeGridPoint(x, y);
                const f32 gpuPacked = pixels[(static_cast<sizet>(y) * kWidth + x) * 4 + 0];
                const f32 cpuPacked = ReSTIR::PackReservoirIdentity(point.Kind, point.LightIndex);

                // Counted separately because it is the v1 signature, and a
                // failure that names it saves the next reader the bisect.
                if (cpuPacked > 0.0f && gpuPacked == 0.0f)
                    ++flushedToZero;

                u32 gpuKind = 0;
                u32 gpuIndex = 0;
                ReSTIR::UnpackReservoirIdentity(gpuPacked, gpuKind, gpuIndex);
                if (gpuKind != point.Kind || gpuIndex != point.LightIndex)
                {
                    if (mismatches < 8)
                    {
                        ADD_FAILURE() << "identity lane disagreed at (" << x << ", " << y << "): wrote kind "
                                      << point.Kind << " index " << point.LightIndex << " (packed "
                                      << cpuPacked << "), read back packed " << gpuPacked << " -> kind "
                                      << gpuKind << " index " << gpuIndex;
                    }
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(mismatches, 0u) << mismatches << " of " << (kWidth * kHeight)
                                  << " identity lanes did not survive the round-trip";
        EXPECT_EQ(flushedToZero, 0u)
            << flushedToZero << " packed identities read back as ZERO — that is the layout v1 denormal "
                                "signature: the encoding is producing values the GPU is flushing, so it "
                                "is storing bit patterns somewhere rather than numbers";
    }

    // The octahedral normal lane, over the full sphere. Tolerance is the
    // encoding's own resolution, not a fudge: 12 bits per axis over [-1, 1] is a
    // quantisation step of 2/4095, and the octahedral fold can put roughly two
    // steps of angular error at the equator where the mapping is most stretched.
    TEST(ReSTIRDIReservoirGpuParity, OctahedralNormalRoundTripsOverTheFullSphere)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ReservoirProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        // The angular budget the 12-bit encoding is entitled to, applied both to
        // "did the direction survive" and to "do the two languages agree".
        // Stated as a cosine so it is a direct comparison rather than an acos of
        // a value that may sit a ulp outside [-1, 1]. One quantisation step is
        // 2/4095 in each octahedral axis, and the fold can put about two of them
        // of angular error at the equator where the mapping is most stretched.
        constexpr f32 kMinCosine = 0.9995f;

        f32 worstCosine = 1.0f;
        u32 failures = 0;
        u32 hemisphereFailures = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const GridPoint point = DecodeGridPoint(x, y);
                const f32 gpuPacked = pixels[(static_cast<sizet>(y) * kWidth + x) * 4 + 1];

                // The C++ twin must produce the same encoded number, and the
                // decode of the GPU's number must land on the input direction.
                // Checking both is what separates "the two languages disagree"
                // from "the encoding is lossy in a way we did not budget for".
                const f32 cpuPacked = ReSTIR::PackReservoirNormal(point.Normal);
                const glm::vec3 decoded = ReSTIR::UnpackReservoirNormal(gpuPacked);
                const f32 cosine = glm::dot(decoded, point.Normal);
                worstCosine = std::min(worstCosine, cosine);

                // The two languages are compared as DIRECTIONS, not as packed
                // integers. A packed difference is not a usable metric here: the
                // two axes have wildly different weights in the number (one step
                // of x is 4096, one step of y is 1), so a legitimate tie at a
                // quantisation boundary — where the two languages' fp32 lands
                // either side of a .5 — reads as a difference of 4096 and a
                // catastrophic octant flip can read as a difference of 1. The
                // first version of this test compared the integers and failed on
                // 2560 harmless boundary ties while the real wrap-around it was
                // built to catch sat in the same list.
                const glm::vec3 cpuDecoded = ReSTIR::UnpackReservoirNormal(cpuPacked);
                const f32 crossLanguageCosine = glm::dot(cpuDecoded, decoded);

                if (point.Normal.z < 0.0f && cosine < kMinCosine)
                    ++hemisphereFailures;

                if (cosine < kMinCosine || crossLanguageCosine < kMinCosine)
                {
                    if (failures < 8)
                    {
                        ADD_FAILURE()
                            << "normal lane disagreed at (" << x << ", " << y << "): wrote (" << point.Normal.x
                            << ", " << point.Normal.y << ", " << point.Normal.z << ") theta " << point.Theta
                            << "; C++ packed " << cpuPacked << ", GPU packed " << gpuPacked << ", GPU decodes to ("
                            << decoded.x << ", " << decoded.y << ", " << decoded.z << "), cos " << cosine
                            << ", C++ vs GPU cos " << crossLanguageCosine;
                    }
                    ++failures;
                }
            }
        }
        EXPECT_EQ(failures, 0u) << failures << " of " << (kWidth * kHeight)
                                << " normals failed; worst cosine " << worstCosine;
        // Called out on its own: the z < 0 branch is the octahedral fold, it is
        // a different code path in both languages, and a transcription slip
        // there fails ONLY on the lower hemisphere — which is half the emitters
        // in any interior scene and none of the ones you look at first.
        EXPECT_EQ(hemisphereFailures, 0u)
            << hemisphereFailures << " failures were in the LOWER hemisphere (z < 0) — that is the "
                                     "octahedral fold, not the encoding generally";
    }

    // The version constant itself, read through the same kind of lane the
    // reservoir planes use. ReSTIRDIContractTest scans the GLSL source text for
    // this; here it comes off the GPU, so a driver that miscompiled the constant
    // would be caught as well as a source edit that moved only one side.
    TEST(ReSTIRDIReservoirGpuParity, LayoutVersionMatchesTheCppConstant)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ReservoirProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        const f32 reported = pixels[3];
        EXPECT_FLOAT_EQ(reported, static_cast<f32>(ReSTIR::kReservoirLayoutVersion))
            << "Reservoir.glsl's OLO_RESERVOIR_LAYOUT_VERSION and ReservoirDI.h's "
               "kReservoirLayoutVersion disagree. They gate whether last frame's reservoirs are "
               "reinterpreted or dropped, so a disagreement is a plausible wrong image rather than "
               "a crash — bump both together.";
    }
} // namespace OloEngine::Tests
