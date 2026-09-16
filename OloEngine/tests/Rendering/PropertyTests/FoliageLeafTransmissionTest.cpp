// OLO_TEST_LAYER: L2
// =============================================================================
// FoliageLeafTransmissionTest.cpp
//
// The GLSL half of issue #1234's contracts, asserted against the PRODUCTION
// functions in assets/shaders/include/FoliageSurface.glsl — the probe
// (assets/shaders/tests/ShaderUnit_FoliageTransmission.glsl) includes them
// rather than transcribing them, so this is a test of the shading language the
// forward, forward+, deferred and impostor paths all share, not a test of a
// copy of it. That is the whole reason those four paths can be claimed to agree
// (the issue's third acceptance criterion) without four tests.
//
// The two claims that matter most, and why each is a test rather than a
// comment:
//
//   TRANSMISSION IS GATED BY THE SHADOW. #1234's second criterion names the
//   wrong answer outright — "leaf transmission is not an unshadowed ambient
//   constant" — and an unshadowed constant is exactly what the cheap version of
//   this effect is. The probe evaluates the SAME backlit geometry at shadow 1
//   and shadow 0 and this asserts the second is zero. A term that ignored its
//   shadow argument would still look right in every screenshot of an unoccluded
//   plant.
//
//   THE SHADOW LOOKUP IS BIASED ALONG THE LIT-SIDE NORMAL. This is the subtle
//   one. Cascaded-shadow sampling offsets the receiver along its normal to
//   escape self-shadowing acne; a backlit leaf's SHADING normal points away
//   from the light, so offsetting along it walks the sample into the leaf's own
//   depth and the shadow factor collapses — taking the transmission term with
//   it. The symptom reads as "the lobe is wrong" and is actually a sign. The
//   probe checks that oloFoliageShadowNormal returns a normal on the LIGHT's
//   side for a backlit leaf, and returns N UNCHANGED when the leaf is lit from
//   the front — the second half being what lets one lookup serve both lobes
//   without changing the reflected lobe's bias on foliage.
//
// Skips cleanly without a GL 4.6 context, like every probe fixture here.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The probe's column layout — mirrored from the shader's header, where
        // each case is named. Kept as an enum so a mismatch between the two is
        // a compile-visible name rather than a magic index.
        enum ProbeCase : u32
        {
            CaseBacklitLit = 0,
            CaseBacklitShadowed = 1,
            CaseAmbientHalf = 2,
            CaseFrontLit = 3,
            CaseZeroThickness = 4,
            CaseFaceNormalFront = 5,
            CaseFaceNormalBack = 6,
            CaseShadowNormalBacklit = 7,
            CaseShadowNormalFrontlit = 8,
            CaseCount
        };

        struct FoliageProbeHarness
        {
            u32 m_Width;
            u32 m_Height;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            FoliageProbeHarness(u32 width, u32 height, const char* shaderPath)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                // RGBA16F, not RGBA8: the lobe's output is linear HDR radiance
                // and the normal dots are signed. An 8-bit target would clamp
                // the -1 this test is specifically looking for to 0 and turn
                // the back-face case into a silent pass.
                spec.Attachments = { FramebufferTextureFormat::RGBA16F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                GLStateGuard guard("FoliageProbeHarness::Draw", GLStateGuard::Policy::Restore);
                m_OutputFB->Bind();
                ::glViewport(0, 0, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));
                ::glDisable(GL_BLEND);
                ::glDisable(GL_DEPTH_TEST);
                ::glDisable(GL_CULL_FACE);
                m_Shader->Bind();
                m_Pass.Draw(0);
                ::glFinish();
                m_OutputFB->Unbind();
            }

            void ReadOutputRgbaFloat(std::vector<f32>& out) const
            {
                ReadbackRgbaFloat(m_OutputFB->GetColorAttachmentRendererID(0), m_Width, m_Height, out);
            }
        };
    } // namespace

    TEST(FoliageLeafTransmissionTest, TheLobeAndTheTwoSidedRuleHoldInTheProductionShader)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        FoliageProbeHarness harness(static_cast<u32>(CaseCount), 1u,
                                    "assets/shaders/tests/ShaderUnit_FoliageTransmission.glsl");

        // THE SHADER COMPILED, ASKED BEFORE ANYTHING IS DRAWN. Shader::Create
        // hands back an OpenGLShader even when FinalizeGL set its status to
        // Failed, and Bind() on a non-Ready program returns without binding
        // while Draw() carries on regardless. Without this the probe would
        // report whatever the no-shader draw left in the attachment — a
        // non-finite texel or a zero that looks like a lobe returning zero —
        // and the failure would read as a shading bug rather than as
        // "ShaderUnit_FoliageTransmission.glsl did not compile".
        //
        // Same trap as Texture2D::Create, which also never returns null; see
        // docs/agent-rules/notes-renderer.md.
        ASSERT_TRUE(harness.m_Shader) << "the probe shader was not created at all";
        ASSERT_TRUE(harness.m_Shader->IsReady())
            << "assets/shaders/tests/ShaderUnit_FoliageTransmission.glsl did not compile — check "
               "OloEngine.log for the compiler diagnostic. Every assertion below would otherwise "
               "measure a frame no shader wrote.";

        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(CaseCount) * 4u);

        const auto red = [&](ProbeCase c)
        { return pixels[static_cast<std::size_t>(c) * 4u + 0u]; };
        const auto green = [&](ProbeCase c)
        { return pixels[static_cast<std::size_t>(c) * 4u + 1u]; };

        // A readback is an external float boundary — validate before anything
        // is compared, or a NaN would make every EXPECT below quietly false
        // rather than reporting that the draw failed.
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(pixels[i]))
                << "probe texel " << i << " is non-finite — the draw or the readback failed, which makes "
                << "nothing after this measurable.";
        }

        // ── The lobe ────────────────────────────────────────────────────────
        EXPECT_GT(red(CaseBacklitLit), 0.0f)
            << "a backlit leaf transmits nothing at all — the forward-scattering lobe is dead, and a canopy "
               "against the sun will read as a black cutout.";

        EXPECT_FLOAT_EQ(red(CaseBacklitShadowed), 0.0f)
            << "TRANSMISSION SURVIVED A SHADOW FACTOR OF ZERO. #1234's second acceptance criterion rules this "
               "out by name: the term is not an unshadowed ambient constant. A leaf behind a trunk must stop "
               "glowing. (The lit arm of the same geometry measured "
            << green(CaseBacklitShadowed) << ".)";
        EXPECT_GT(green(CaseBacklitShadowed), 0.0f)
            << "the lit control arm is also zero, so the zero above proves nothing — the probe itself is broken.";

        EXPECT_GT(red(CaseAmbientHalf), 0.0f)
            << "the environment half of the transmission is dead, so a leaf only glows where a direct light "
               "reaches it and the sky contributes nothing through it.";
        EXPECT_FLOAT_EQ(green(CaseAmbientHalf), 0.0f)
            << "the environment half returned a non-zero value for ZERO irradiance — which means it is a "
               "constant with an irradiance-shaped argument, i.e. exactly the ambient constant the issue "
               "rules out, hiding in the other half of the term.";

        EXPECT_LT(red(CaseFrontLit), green(CaseFrontLit))
            << "a FRONT-lit leaf transmits as much as a backlit one (" << red(CaseFrontLit) << " vs "
            << green(CaseFrontLit)
            << "). This lobe models forward scattering; a term equally bright from both sides is an ambient "
               "fill wearing a lobe's name, and the backlit demonstrating case would look no different from "
               "the front-lit control.";

        EXPECT_FLOAT_EQ(red(CaseZeroThickness), 0.0f)
            << "zero thickness still transmitted through the DIRECT half — so a layer that is not a leaf "
               "material (the pre-#1234 default) acquires a glow nobody authored.";
        EXPECT_FLOAT_EQ(green(CaseZeroThickness), 0.0f)
            << "zero thickness still transmitted through the AMBIENT half, with the same consequence.";

        // ── The two-sided rule ──────────────────────────────────────────────
        constexpr f32 kUnitTolerance = 1e-4f;
        EXPECT_NEAR(red(CaseFaceNormalFront), 1.0f, kUnitTolerance)
            << "oloFoliageFaceNormal flipped a normal that already faced the viewer.";
        EXPECT_GT(green(CaseFaceNormalFront), 0.0f) << "and the result does not face the viewer.";

        EXPECT_NEAR(red(CaseFaceNormalBack), -1.0f, kUnitTolerance)
            << "oloFoliageFaceNormal did NOT flip a normal facing away from the viewer. The back of every leaf "
               "then lights as if it were the front, and — because the deferred path stores this same normal in "
               "the G-Buffer while the forward path lights with it directly — the two paths disagree in exactly "
               "the cell #1234's third criterion is about.";
        EXPECT_GT(green(CaseFaceNormalBack), 0.0f)
            << "the flipped normal still does not face the viewer, which is the actual contract.";

        EXPECT_GT(red(CaseShadowNormalBacklit), 0.0f)
            << "oloFoliageShadowNormal returned a normal pointing AWAY from the light for a backlit leaf. The "
               "cascade's receiver normal-offset then pushes the sample point behind the leaf's own shadow-map "
               "depth, the shadow factor collapses to zero, and the transmission term it gates goes black — "
               "which reads as a broken lobe and is a sign error.";
        EXPECT_LT(green(CaseShadowNormalBacklit), 0.0f)
            << "and it is not the opposite of the shading normal, so it is not the lit-side normal at all.";

        EXPECT_NEAR(green(CaseShadowNormalFrontlit), 1.0f, kUnitTolerance)
            << "oloFoliageShadowNormal changed the normal for a FRONT-lit leaf. It must be the identity there: "
               "that is what lets ONE shadow lookup serve both the reflected and the transmitted lobe, and what "
               "keeps foliage's reflected lobe biased exactly as every other surface's is.";
        EXPECT_GT(red(CaseShadowNormalFrontlit), 0.0f);
    }

} // namespace OloEngine::Tests
