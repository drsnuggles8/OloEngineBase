// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// SkinOralSurfaceParityTest.cpp — the GLSL oral surface IS the CPU one.
// Issue #1245.
//
// WHY THIS EXISTS BESIDE SkinOralSurfaceTest. That test pins the CPU maths and
// its energy argument. This one pins that the SHADER computes the same maths —
// and the two together are what make the arrangement
// Renderer/SkinOralSurface.h describes actually load-bearing:
//
//   "THE MATHS LIVES HERE AND THE SHADER TRANSCRIBES IT ... a physical decision
//    that only exists inside a .glsl file cannot be unit tested."
//
// A specification nothing is compared against is a comment. The probe
// (assets/shaders/tests/ShaderUnit_SkinOralSurface.glsl) CALLS the production
// functions rather than transcribing them, so agreement here is agreement for
// all four of their real callers: PBR_MultiLight{,_Skinned}.glsl,
// include/ForwardPlusCommon.glsl and include/DeferredLightingShared.glsl.
//
// THE ASSERTION THAT MATTERS MOST is CaseCoatLobeParity: the coat's GGX lobe
// compared against SkinOralCoatSpecular at an oblique configuration. Every
// other coat case is a PROPERTY, and a property can hold while the number is
// wrong by a factor of a thousand — which is what a missing
// 1/(4 NdotV NdotL), a Smith term paired with the wrong alpha or a dropped D
// normalization would each look like. It also pins that the CPU transcribed
// PBRCommon.glsl's distributionGGX and visibilitySmithGGXCorrelated correctly,
// since the shader side calls the originals.
//
// THE SECOND MOST IMPORTANT is CaseThePartition. That is the energy statement
// the whole feature rests on, asserted as an exact identity on the GPU rather
// than only on the CPU — because it is the GPU's arithmetic that ships.
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
#include "OloEngine/Renderer/SkinOralSurface.h"
#include "OloEngine/Renderer/SkinProfile.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The probe's column layout — mirrored from the shader's header, where
        // each case is named. An enum so a mismatch between the two is a
        // compile-visible name rather than a magic index.
        enum ProbeCase : u32
        {
            CaseF0Parity = 0,
            CaseF0Enamel = 1,
            CaseF0AtAirIsZero = 2,
            CaseFresnelParity = 3,
            CaseFresnelAtGrazing = 4,
            CaseThePartition = 5,
            CaseCoatLobeParity = 6,
            CaseCoatLobeDegenerate = 7,
            CaseZeroStrengthExact = 8,
            CaseCoatDarkensDiffuse = 9,
            CaseDiffuseNeverNegative = 10,
            CaseCavityZeroIsOne = 11,
            CaseCavityOneIsAo = 12,
            CaseCavityMonotone = 13,
            CaseLaneGateWrongVersion = 14,
            CaseLaneGateRightVersion = 15,
            CaseCoatLobeAtGrazing = 16,
            CaseCount
        };

        // THE FIXTURE, mirrored from the shader's literals.
        // TheProbeFixtureMatchesTheProfileItClaimsToBe below asserts that the
        // lane literals really are what the named profile packs — so the mirror
        // cannot drift silently, which is the standing risk with a hand-copied
        // fixture.
        constexpr f32 kSalivaIor = 1.33f;
        constexpr f32 kEnamelIor = 1.63f;
        constexpr f32 kCoatStrength = 0.45f;
        constexpr f32 kCoatRoughness = 0.12f;
        constexpr f32 kCavityOcclusion = 0.6f;
        constexpr f32 kObliqueCos = 0.37f;
        constexpr f32 kAo = 0.25f;

        // The grazing pair, mirrored from the probe. dot(N, H) 0.30 against
        // dot(V, H) 0.97 — the configuration that separates the two cosines
        // Schlick could be given. See CaseCoatLobeAtGrazing.
        constexpr f32 kGrazingRoughness = 0.25f;

        // THE TOLERANCE IS A PROPERTY OF THE FORMAT, not a number chosen until
        // the test passed — the same reasoning, and the same helper, as
        // SkinLayeredSpecularParityTest. The probe writes RGBA16F, whose
        // mantissa is 11 bits, so the relative spacing is about 2^-11; the
        // factor of two covers the two roundings a comparison involves, and the
        // 1e-5 floor keeps the bound meaningful for the values near 0.02 that
        // the Fresnel cases produce.
        [[nodiscard]] f32 HalfTolerance(f32 magnitude)
        {
            constexpr f32 kHalfRelative = 1.0e-3f;
            return std::max(1.0e-5f, std::abs(magnitude) * kHalfRelative);
        }

        struct ProbeHarness
        {
            u32 m_Width;
            u32 m_Height;
            Ref<Framebuffer> m_OutputFB;
            Ref<Shader> m_Shader;
            FullscreenPass m_Pass;

            ProbeHarness(u32 width, u32 height, const char* shaderPath)
                : m_Width(width), m_Height(height)
            {
                FramebufferSpecification spec{};
                spec.Width = width;
                spec.Height = height;
                // RGBA16F, not RGBA8. Case 8 carries a SIGNED difference (the
                // coat's effect minus zero), which an 8-bit target would clamp
                // to 0 and turn into a silent pass — the worst possible failure
                // for a case whose whole job is to notice a difference.
                spec.Attachments = { FramebufferTextureFormat::RGBA16F };
                m_OutputFB = Framebuffer::Create(spec);
                m_Shader = Shader::Create(shaderPath);
            }

            void Draw()
            {
                GLStateGuard guard("SkinOralSurfaceProbe::Draw", GLStateGuard::Policy::Restore);
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

    // THE MIRROR CHECK, and it runs on the CPU alone so it cannot be skipped by
    // a headless machine. The probe hard-codes its lane rather than taking a
    // uniform (so a failure cannot be a bad bind); the price is that the
    // literals could drift from any real profile, at which point the GPU/CPU
    // comparison below would be comparing two answers to a question nobody
    // asks. This pins the literals to a profile.
    TEST(SkinOralSurfaceParity, TheProbeFixtureMatchesTheProfileItClaimsToBe)
    {
        SkinProfileParameters profile{};
        profile.EvaluationModel = SkinEvaluationModel::OralSurface;
        profile.Oral.CoatStrength = kCoatStrength;
        profile.Oral.CoatRoughness = kCoatRoughness;
        profile.Oral.CoatIor = kSalivaIor;
        profile.Oral.CavityOcclusion = kCavityOcclusion;
        ASSERT_TRUE(profile.Sanitize()) << "the probe fixture fell outside the authored bounds";

        const glm::vec4 lane = SkinOralLane(profile);
        EXPECT_NEAR(lane.x, kCoatStrength, 1.0e-6f)
            << "ShaderUnit_SkinOralSurface.glsl's kLane no longer matches what a real profile packs";
        EXPECT_NEAR(lane.y, kCoatRoughness, 1.0e-6f);
        EXPECT_NEAR(lane.z, SkinOralCoatF0(kSalivaIor), 1.0e-6f);
        EXPECT_NEAR(lane.w, kCavityOcclusion, 1.0e-6f);

        // And the two indices really are different surfaces, which is the
        // relationship the enamel cases below are about.
        EXPECT_GT(SkinOralCoatF0(kEnamelIor), SkinOralCoatF0(kSalivaIor))
            << "the probe's two indices no longer describe two different materials";
    }

    TEST(SkinOralSurfaceParity, TheProductionShaderComputesTheCpuMaths)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ProbeHarness harness(static_cast<u32>(CaseCount), 1u,
                             "assets/shaders/tests/ShaderUnit_SkinOralSurface.glsl");

        // THE SHADER COMPILED, ASKED BEFORE ANYTHING IS DRAWN. Shader::Create
        // hands back an OpenGLShader even when its status is Failed, and Bind()
        // on a non-Ready program returns without binding while Draw() carries on
        // regardless. Without this the probe would report whatever the no-shader
        // draw left in the attachment — a zero that looks exactly like a term
        // correctly returning zero.
        ASSERT_TRUE(harness.m_Shader) << "the probe shader was not created at all";
        ASSERT_TRUE(harness.m_Shader->IsReady())
            << "assets/shaders/tests/ShaderUnit_SkinOralSurface.glsl did not compile — check OloEngine.log for the "
               "compiler diagnostic. Every assertion below would otherwise measure a frame no shader wrote.";

        harness.Draw();

        std::vector<f32> pixels;
        harness.ReadOutputRgbaFloat(pixels);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(CaseCount) * 4u);

        const auto texel = [&](ProbeCase c)
        {
            const std::size_t base = static_cast<std::size_t>(c) * 4u;
            return glm::vec4(pixels[base + 0u], pixels[base + 1u], pixels[base + 2u], pixels[base + 3u]);
        };

        // A readback is an external float boundary — validate before anything is
        // compared, or a NaN would make every EXPECT below quietly false rather
        // than reporting that the draw failed.
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(pixels[i]))
                << "probe texel " << i << " is non-finite — the draw or the readback failed, which makes nothing "
                                          "after this measurable.";
        }

        // ── The index-of-refraction conversion ──────────────────────────────

        const f32 cpuSaliva = SkinOralCoatF0(kSalivaIor);
        const f32 cpuEnamel = SkinOralCoatF0(kEnamelIor);

        EXPECT_NEAR(texel(CaseF0Parity).r, cpuSaliva, HalfTolerance(cpuSaliva))
            << "THE SHADER'S IOR -> F0 CONVERSION IS NOT THE CPU'S. The production lane carries F0 already, "
               "converted once on the CPU — but if the two formulas disagree, then the shader-side helper this "
               "probe calls is documenting a conversion the engine does not perform, and the next person to reach "
               "for it gets a different wetness.";
        EXPECT_NEAR(texel(CaseF0Enamel).r, cpuEnamel, HalfTolerance(cpuEnamel))
            << "and enamel disagrees too, so the first case was not agreeing by coincidence at a fixed point.";
        EXPECT_NEAR(texel(CaseF0AtAirIsZero).r, 0.0f, 1.0e-5f)
            << "A COAT OF THE INDEX OF AIR REFLECTED SOMETHING. That value is the second, redundant spelling of "
               "'dry', and it is the reason the authored bound's floor is 1 rather than something above it.";

        // ── The Fresnel ─────────────────────────────────────────────────────

        const f32 cpuFresnel = SkinOralCoatFresnel(cpuSaliva, kObliqueCos);
        EXPECT_NEAR(texel(CaseFresnelParity).r, cpuFresnel, HalfTolerance(cpuFresnel))
            << "THE SHADER'S SCHLICK IS NOT THE CPU'S. Both sides spell the fifth power as four multiplies; a "
               "pow() on one side, or dot(N,V) substituted for dot(N,H), lands here and nowhere a frame would "
               "show it plainly.";
        EXPECT_NEAR(texel(CaseFresnelAtGrazing).r, 1.0f, HalfTolerance(1.0f))
            << "THE FRESNEL DID NOT REACH ONE AT GRAZING. That limit is what makes a wet surface rim-bright, and "
               "a coat that does not have it reads as a plastic sheen instead of a film.";

        // ── The partition — the energy statement ────────────────────────────

        const glm::vec4 partition = texel(CaseThePartition);
        EXPECT_NEAR(partition.r, 1.0f, HalfTolerance(1.0f))
            << "WHAT THE COAT TOOK AND WHAT IT LEFT DO NOT ADD TO ONE. This is the energy statement the whole "
               "feature rests on (Renderer/SkinOralSurface.h): the coat is a PARTITION of the incident energy, "
               "not a blend. A coat that does not partition leaks a few percent of extra light per light per "
               "frame, which no screenshot will ever show and every lighting artist will eventually chase.";
        // And the two halves individually match the CPU, so the sum is not
        // right by two compensating errors.
        const f32 cpuAttenuation = SkinOralCoatAttenuation(kCoatStrength, cpuFresnel);
        EXPECT_NEAR(partition.g, cpuAttenuation, HalfTolerance(cpuAttenuation));
        EXPECT_NEAR(partition.b, cpuFresnel, HalfTolerance(cpuFresnel));

        // ── The coat lobe ───────────────────────────────────────────────────

        const glm::vec3 N{ 0.0f, 0.0f, 1.0f };
        const glm::vec3 V = glm::normalize(glm::vec3(0.15f, -0.10f, 1.0f));
        const glm::vec3 L = glm::normalize(glm::vec3(0.45f, 0.30f, 0.84f));

        const f32 cpuCoat = SkinOralCoatSpecular(N, V, L, kCoatRoughness, cpuSaliva);
        EXPECT_NEAR(texel(CaseCoatLobeParity).r, cpuCoat, HalfTolerance(cpuCoat))
            << "THE SHADER'S COAT LOBE IS NOT THE CPU'S. This is the case that catches a missing "
               "1/(4 NdotV NdotL), a Smith term paired with the wrong alpha, or a dropped normalization in D — "
               "none of which moves the partition above by a single bit, and all of which change how bright a wet "
               "mouth is by an order of magnitude.";
        EXPECT_GT(cpuCoat, 0.0f) << "the fixture is degenerate — the parity above would pass on two zeros";

        EXPECT_FLOAT_EQ(texel(CaseCoatLobeDegenerate).r, 0.0f)
            << "A CANCELLING VIEW AND LIGHT PRODUCED SOMETHING. There is no half-vector there; the honest answer "
               "is zero, and the alternative is a NaN that takes a whole frame's specular with it.";

        // ── Applying the coat ───────────────────────────────────────────────

        const glm::vec4 zeroStrength = texel(CaseZeroStrengthExact);
        EXPECT_FLOAT_EQ(zeroStrength.x, 0.0f);
        EXPECT_FLOAT_EQ(zeroStrength.y, 0.0f);
        EXPECT_FLOAT_EQ(zeroStrength.z, 0.0f)
            << "A COAT STRENGTH OF ZERO CHANGED THE SPLIT. That value is the A/B control arm the wet-vs-dry "
               "evidence is measured against; if it is not an exact identity, 'dry' is a third variant rather "
               "than the baseline and every comparison built on it is meaningless.";

        const glm::vec4 darkens = texel(CaseCoatDarkensDiffuse);
        EXPECT_LT(darkens.r, darkens.g)
            << "THE COAT DID NOT TAKE ENERGY FROM THE DIFFUSE HALF. A film in front of the tissue means light it "
               "reflects never reaches the tissue. A coat that left the diffuse alone is a highlight painted on "
               "top — and because the diffuse half is exactly what oloSkinDiffusionOutput hands the screen-space "
               "blur, that is also how a wet lip would end up making the tissue under it glow.";
        EXPECT_GT(darkens.b, 0.0f) << "the coat added no specular at all, so the case above proves nothing";

        EXPECT_GE(texel(CaseDiffuseNeverNegative).r, 0.0f)
            << "THE STRONGEST LEGAL COAT DROVE THE DIFFUSE NEGATIVE. That half is blurred and re-added by the "
               "diffusion pass, so a negative value becomes a dark halo around every wet surface.";

        // ── The cavity weight ───────────────────────────────────────────────

        EXPECT_FLOAT_EQ(texel(CaseCavityZeroIsOne).r, 1.0f)
            << "A CAVITY OCCLUSION OF ZERO CHANGED THE TRANSMITTED TERM. That is the identity arm of the 'no "
               "glowing interiors' A/B: a version-4 profile that authored only a coat must transmit precisely "
               "what issue #1242 shipped.";
        EXPECT_NEAR(texel(CaseCavityOneIsAo).r, kAo, HalfTolerance(kAo))
            << "A FULLY SPENT CAVITY OCCLUSION DID NOT RETURN THE AO, so the identity above may be passing "
               "because the function does nothing at all.";

        const glm::vec4 monotone = texel(CaseCavityMonotone);
        EXPECT_GT(monotone.r, monotone.g)
            << "MORE CAVITY OCCLUSION TRANSMITTED MORE. That inequality IS the 'no glowing interiors' claim; "
               "everything else about it is a picture.";

        // ── The version gate ────────────────────────────────────────────────

        const glm::vec4 stale = texel(CaseLaneGateWrongVersion);
        EXPECT_FLOAT_EQ(stale.r, 0.0f)
            << "A VERSION-3 PROFILE ACQUIRED A COAT. The deferred table is indexed by a slot that can be stale by "
               "a frame after a scene change, and a stale slot must LOSE the effect rather than acquire someone "
               "else's wetness.";
        EXPECT_FLOAT_EQ(stale.g, 0.0f) << "a non-skin material acquired a coat";

        // ── The coat lobe at grazing — WHICH COSINE SCHLICK TOOK ────────────

        // THE CASE THAT WOULD HAVE CAUGHT THE ORIGINAL BUG. Every other case in
        // this probe is near normal incidence, where Schlick at dot(N, H) and at
        // dot(V, H) agree to five decimal places — so all of them passed while
        // the implementation took the wrong one. This configuration separates
        // them by a factor of nine.
        const glm::vec4 grazing = texel(CaseCoatLobeAtGrazing);
        const glm::vec3 gv = glm::normalize(glm::vec3(0.90f, 0.0f, 0.4359f));
        const glm::vec3 gl = glm::normalize(glm::vec3(0.9123f, 0.3801f, 0.1521f));
        const glm::vec3 gh = glm::normalize(gv + gl);

        // The probe reports both cosines, so a fixture that stopped separating
        // them fails HERE rather than silently weakening the comparison below.
        EXPECT_LT(grazing.g, 0.5f) << "the probe's grazing fixture no longer separates the two cosines";
        EXPECT_GT(grazing.b, 0.9f) << "the probe's grazing fixture no longer separates the two cosines";
        EXPECT_NEAR(grazing.g, glm::dot(N, gh), HalfTolerance(glm::dot(N, gh)));
        EXPECT_NEAR(grazing.b, glm::dot(gv, gh), HalfTolerance(glm::dot(gv, gh)));

        const f32 cpuGrazing = SkinOralCoatSpecular(N, gv, gl, kGrazingRoughness, cpuSaliva);
        EXPECT_NEAR(grazing.r, cpuGrazing, HalfTolerance(cpuGrazing))
            << "THE SHADER AND THE CPU DISAGREE ABOUT THE COAT AT GRAZING. This is the case that tells "
               "dot(V, H) from dot(N, H): if one side takes the view-to-half angle and the other the "
               "normal-to-half angle, every head-on case still agrees and only this one moves. Schlick is a "
               "statement about the angle of INCIDENCE on the reflecting microfacet — include/PBRCommon.glsl "
               "passes dot(H, V) at all six of its microfacet call sites.";

        const glm::vec4 live = texel(CaseLaneGateRightVersion);
        EXPECT_NEAR(live.x, kCoatStrength, HalfTolerance(kCoatStrength))
            << "and a version-4 skin pixel got NOTHING, so the gate above is closed for everybody — which would "
               "make the whole feature inert while every property test still passed.";
        EXPECT_NEAR(live.w, kCavityOcclusion, HalfTolerance(kCavityOcclusion));
    }
} // namespace OloEngine::Tests
