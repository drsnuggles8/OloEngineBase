// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ReSTIRGIReservoirGpuParityTest.cpp — pins include/ReservoirGI.glsl's PACKING
// against its C++ twin in Renderer/ReSTIR/ReservoirGI.h, across a real RGBA32F
// render target (issue #1169).
//
// WHY THIS FILE EXISTS ALONGSIDE ReSTIRDIReservoirGpuParityTest. The ENCODING is
// now shared — include/ReservoirCore.glsl and ReservoirCore.h — and DI's probe
// already drives it. What is GI's alone is what the identity lane CARRIES: an
// AGE, not a light index, sharing that lane with a three-value kind.
//
// And the age is not just a different number in the same slot. It is the SECOND
// STALENESS BOUND (design note §10), the one that exists because #976's validity
// test only looks at the receiving surface and therefore accepts a sample whose
// own vertex was re-lit or moved. An age that saturated wrongly — or decoded one
// step low, or wrapped — would make the OLDEST samples look the freshest, which
// is precisely the failure the cap was added to prevent, arriving through the
// cap itself. Nothing about that is visible in an image: a stale bounce is a
// plausible bounce.
//
// THE DESIGN IS DICTATED BY #1140's BUG. Layout v1 stored integers as float BIT
// PATTERNS; a small integer reinterpreted as a float is a DENORMAL, the GPU
// flushed it to zero, every reservoir read back empty, the resolve wrote black,
// and the counters still said the tier was healthy. So the probe WRITES the
// packed lanes into an RGBA32F attachment and this file reads them back off the
// texture: a test that packed and unpacked inside one shader invocation would
// have passed v1, because the value never went to memory.
//
// SKIPs cleanly without a GL 4.6 context, like every other GPU test here.
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/ReSTIR/ReservoirGI.h"
#include "OloEngine/Renderer/Shader.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Must match AGE_STEPS / KIND_STEPS in ReSTIRGIReservoirParityProbe.glsl.
        // A mismatch would silently compare different grids on the two sides, so
        // both constants are named in both files and the decode below mirrors the
        // shader's line for line.
        constexpr u32 kAgeSteps = 256;
        constexpr u32 kKindSteps = 3;
        // The stride the probe multiplies the age band by, so the sweep reaches
        // the TOP of the cap rather than the bottom 255 frames — which is where a
        // saturation written as a wrap would show and where nothing else looks.
        constexpr u32 kAgeStride = ReSTIR::kMaxSampleAgeFrames / (kAgeSteps - 1);
        // The stride is INTEGER division, so it cannot be trusted to land on the
        // cap: 4096 / 255 is 16, and 255 * 16 is 4080. The sweep therefore stopped
        // sixteen frames short of the one value that matters most - the exact cap,
        // which is where a lane written one bit too narrow saturates or wraps. The
        // top band is pinned to the cap for that reason, and the shader does the
        // same so the two grids stay identical.
        static_assert(kAgeStride > 0, "kAgeSteps has outgrown the age cap; the sweep would collapse to zero");
        static_assert(kAgeStride * (kAgeSteps - 1) <= ReSTIR::kMaxSampleAgeFrames,
                      "the age sweep would run past the cap and the comparison would test the clamp, not the pack");

        constexpr u32 kWidth = 256;  // age axis
        constexpr u32 kHeight = 252; // packed (kind band, normal direction) axis; 252 = 3 * 84

        constexpr f32 kPi = 3.14159265358979f;

        // Minimal fullscreen-draw harness. Deliberately a local copy rather than
        // a shared header: this file must be able to state exactly what GL state
        // its comparison ran under.
        struct GIReservoirProbeHarness
        {
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            GIReservoirProbeHarness()
            {
                FramebufferSpecification spec{};
                spec.Width = kWidth;
                spec.Height = kHeight;
                // RGBA32F because that is what the reservoir planes ARE. The point
                // of this test is the fidelity of this exact format, so anything
                // narrower here would be testing a different thing.
                spec.Attachments = { FramebufferTextureFormat::RGBA32F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create("assets/shaders/tests/ReSTIRGIReservoirParityProbe.glsl");
            }

            // Returns false on a resource-creation failure. A gtest ASSERT here
            // would only exit THIS helper and the caller would then null-deref in
            // ReadOutput(), so the status propagates instead.
            [[nodiscard]] bool Draw()
            {
                if (m_OutputFB == nullptr || m_Shader == nullptr)
                {
                    ADD_FAILURE() << (m_OutputFB == nullptr
                                          ? "GI reservoir parity probe framebuffer was not created"
                                          : "ReSTIRGIReservoirParityProbe.glsl failed to load/compile");
                    return false;
                }

                GLStateGuard guard("ReSTIRGIReservoirGpuParity::Draw", GLStateGuard::Policy::Restore);
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
            u32 Age = 0;
            glm::vec3 Normal{ 0.0f };
            f32 Theta = 0.0f;
        };

        // The C++ side of the identical grid. Mirrors the shader's decode exactly
        // — including the pixel-centre offsets, which is what keeps the two
        // evaluating the SAME inputs rather than two grids that merely span the
        // same ranges.
        [[nodiscard]] GridPoint DecodeGridPoint(u32 x, u32 y)
        {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kHeight);

            GridPoint point;
            const f32 ageT = std::clamp(u, 0.0f, 1.0f);
            const u32 ageBand = static_cast<u32>(std::min(std::floor(ageT * static_cast<f32>(kAgeSteps)),
                                                          static_cast<f32>(kAgeSteps - 1)));
            // The top band IS the cap, not 255 strides short of it. Mirrors the
            // probe's line.
            point.Age = (ageBand == kAgeSteps - 1) ? ReSTIR::kMaxSampleAgeFrames : ageBand * kAgeStride;

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

    // The identity lane, across the storage round-trip. The encoding is
    // integer-valued and every value in range is exact in f32 — which is the
    // entire reason it is arithmetic rather than a bit cast — so an exact
    // comparison is the right assertion.
    TEST(ReSTIRGIReservoirGpuParity, IdentityLaneCarriesKindAndAgeThroughAnRGBA32FRoundTrip)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GIReservoirProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        u32 mismatches = 0;
        u32 flushedToZero = 0;
        u32 agedWrong = 0;
        for (u32 y = 0; y < kHeight; ++y)
        {
            for (u32 x = 0; x < kWidth; ++x)
            {
                const GridPoint point = DecodeGridPoint(x, y);
                const f32 gpuPacked = pixels[(static_cast<sizet>(y) * kWidth + x) * 4 + 0];
                const f32 cpuPacked = ReSTIR::PackGIIdentity(static_cast<ReSTIR::GISampleKind>(point.Kind),
                                                             point.Age);

                // Counted separately because it is the layout v1 signature, and a
                // failure that names it saves the next reader the bisect.
                if (cpuPacked > 0.0f && gpuPacked == 0.0f)
                    ++flushedToZero;

                ReSTIR::GISampleKind gpuKind{};
                u32 gpuAge = 0;
                ReSTIR::UnpackGIIdentity(gpuPacked, gpuKind, gpuAge);
                // Called out on its own: an age that came back LOWER than it went
                // in is the failure the cap exists to prevent, arriving through
                // the cap itself — the oldest samples would look the freshest and
                // would never be dropped.
                if (gpuAge < point.Age)
                    ++agedWrong;
                if (std::to_underlying(gpuKind) != point.Kind || gpuAge != point.Age)
                {
                    if (mismatches < 8)
                    {
                        ADD_FAILURE() << "identity lane disagreed at (" << x << ", " << y << "): wrote kind "
                                      << point.Kind << " age " << point.Age << " (packed " << cpuPacked
                                      << "), read back packed " << gpuPacked << " -> kind "
                                      << std::to_underlying(gpuKind) << " age " << gpuAge;
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
        EXPECT_EQ(agedWrong, 0u)
            << agedWrong << " ages read back LOWER than they were written — a sample that ages backwards "
                            "is never dropped by the age cap, which is the one failure that cap exists "
                            "to prevent";
    }

    // The octahedral normal lane, over the full sphere. Shared with DI through
    // include/ReservoirCore.glsl — driven again here because GI writes it through
    // its OWN pack function and a plane-layout slip would put the normal in the
    // wrong lane while both encoders stayed correct.
    TEST(ReSTIRGIReservoirGpuParity, OctahedralNormalRoundTripsOverTheFullSphere)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GIReservoirProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        // The angular budget the 12-bit encoding is entitled to. Stated as a
        // cosine so it is a direct comparison rather than an acos of a value that
        // may sit a ulp outside [-1, 1].
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

                const f32 cpuPacked = ReSTIR::PackReservoirNormal(point.Normal);
                const glm::vec3 decoded = ReSTIR::UnpackReservoirNormal(gpuPacked);
                const f32 cosine = glm::dot(decoded, point.Normal);
                worstCosine = std::min(worstCosine, cosine);

                // Compared as DIRECTIONS, not as packed integers: the two axes
                // have wildly different weights in the number, so a legitimate
                // tie at a quantisation boundary reads as a difference of 4096
                // while a catastrophic octant flip can read as a difference of 1.
                const glm::vec3 cpuDecoded = ReSTIR::UnpackReservoirNormal(cpuPacked);
                const f32 crossLanguageCosine = glm::dot(cpuDecoded, decoded);

                if (point.Normal.z < 0.0f && cosine < kMinCosine)
                    ++hemisphereFailures;

                if (cosine < kMinCosine || crossLanguageCosine < kMinCosine)
                {
                    if (failures < 8)
                    {
                        ADD_FAILURE() << "normal lane disagreed at (" << x << ", " << y << "): wrote ("
                                      << point.Normal.x << ", " << point.Normal.y << ", " << point.Normal.z
                                      << ") theta " << point.Theta << "; C++ packed " << cpuPacked
                                      << ", GPU packed " << gpuPacked << ", GPU decodes to (" << decoded.x
                                      << ", " << decoded.y << ", " << decoded.z << "), cos " << cosine
                                      << ", C++ vs GPU cos " << crossLanguageCosine;
                    }
                    ++failures;
                }
            }
        }
        EXPECT_EQ(failures, 0u) << failures << " of " << (kWidth * kHeight)
                                << " normals failed; worst cosine " << worstCosine;
        // The z < 0 branch is the octahedral fold: a different code path in both
        // languages, and a transcription slip there fails ONLY on the lower
        // hemisphere — which is every downward-facing bounce vertex in any
        // interior scene and none of the ones you look at first.
        EXPECT_EQ(hemisphereFailures, 0u)
            << hemisphereFailures << " failures were in the LOWER hemisphere (z < 0) — that is the "
                                     "octahedral fold, not the encoding generally";
    }

    // The version constant, read through the same kind of lane the reservoir
    // planes use. ReSTIRGIContractTest scans the GLSL source text for it; here it
    // comes off the GPU, so a driver that miscompiled the constant is caught as
    // well as a source edit that moved only one side.
    TEST(ReSTIRGIReservoirGpuParity, LayoutVersionMatchesTheCppConstantAndDiffersFromDIs)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        GIReservoirProbeHarness harness;
        ASSERT_TRUE(harness.Draw());

        std::vector<f32> pixels;
        harness.ReadOutput(pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4);

        const f32 reported = pixels[3];
        EXPECT_FLOAT_EQ(reported, static_cast<f32>(ReSTIR::kGIReservoirLayoutVersion))
            << "ReservoirGI.glsl's OLO_GI_RESERVOIR_LAYOUT_VERSION and ReservoirGI.h's "
               "kGIReservoirLayoutVersion disagree. They gate whether last frame's reservoirs are "
               "reinterpreted or dropped, so a disagreement is a plausible wrong image rather than a "
               "crash — bump both together.";
    }
} // namespace OloEngine::Tests
