// OLO_TEST_LAYER: L8
//
// A TWO-SIDED planar caster must cast a shadow when lit from the front (issue #650).
//
// ShadowRenderPass renders every caster with FRONT-FACE culling (the standard
// peter-panning mitigation for closed geometry). For a single-sided planar mesh — a quad,
// a card, a banner, a foliage sheet — the only face that exists is the one facing the
// light, so front-culling removes it and the object casts NO shadow at all. It is silent:
// the object renders normally and simply has no shadow.
//
// The fix (issue #650): a caster whose material is MaterialFlag::TwoSided is rendered into
// the shadow map with culling DISABLED — consistent with the main pass and the virtual
// path, which already treat TwoSided that way. Single-sided geometry keeps front-culling,
// so closed meshes retain their peter-panning fix.
//
// The reproduction is a horizontal quad above nothing, lit from straight above (so its
// up-facing front is turned toward the light), marked TwoSided, and the assertion is on the
// CSM depth directly: did the caster render into the shadow map at all? Before the fix,
// front-culling drops it and the shadow map is empty; after, it is present. Asserting on the
// CSM (not on a darkened receiver) isolates exactly "did the cascade render the caster",
// which is the whole question — and needs no second surface to receive a shadow.

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glad/gl.h>
#include <gtest/gtest.h>

#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;
    } // namespace

    class TwoSidedShadowCast : public RendererAttachedTest
    {
      protected:
        Entity m_Quad;

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // Sun straight down — so the quad's up-facing front is turned TOWARD the light and
            // gets front-culled in the shadow pass. That is the exact configuration that drops a
            // single-sided planar caster's shadow.
            Entity sun = scene.CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.03f, -1.0f, 0.03f));
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = 3.0f;
            dl.m_CastShadows = true;

            // A horizontal quad (Plane primitive, normal +Y) a few units up. Single set of
            // triangles — its up face is the only one, and it faces the sun.
            m_Quad = scene.CreateEntity("TwoSidedQuad");
            auto& tc = m_Quad.GetComponent<TransformComponent>();
            tc.Translation = { 0.0f, 3.0f, 0.0f };
            tc.Scale = { 4.0f, 1.0f, 4.0f };
            auto& mc = m_Quad.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Plane;
            if (Ref<Mesh> plane = MeshPrimitives::CreatePlane(); plane)
                mc.m_MeshSource = plane->GetMeshSource();
            auto& mat = m_Quad.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
            mat.m_Material.SetFlag(MaterialFlag::TwoSided, true); // the flag under test
        }

        // Count CSM texels the caster rendered (depth below the 1.0 clear value), across all
        // cascades. Same non-vacuous protocol as the virtual-geometry shadow tests: render one
        // frame to bring the map up, scrub it to the clear value by hand, then render and read —
        // so a sub-1.0 texel can only come from THIS frame drawing the caster.
        sizet CasterTexelsAfterScrub()
        {
            ShadowSettings s = Renderer3D::GetShadowMap().GetSettings();
            s.Enabled = true;
            Renderer3D::GetShadowMap().SetSettings(s);

            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose({ 0.0f, 1.0f, 9.0f }, 0.0f, -0.15f);

            RunEditorFrames(camera, 1);

            const Ref<Texture2DArray>& csm = Renderer3D::GetShadowMap().GetCSMTextureArray();
            if (!csm)
                return 0;
            const u32 w = csm->GetWidth();
            const u32 h = csm->GetHeight();
            if (w == 0 || h == 0)
                return 0;

            {
                const f32 one = 1.0f;
                ::glClearTexImage(csm->GetRendererID(), 0, GL_DEPTH_COMPONENT, GL_FLOAT, &one);
            }

            RunEditorFrames(camera, 3);

            sizet casterTexels = 0;
            std::vector<f32> depth(static_cast<sizet>(w) * h);
            for (u32 layer = 0; layer < ShadowMap::MAX_CSM_CASCADES; ++layer)
            {
                ::glGetTextureSubImage(csm->GetRendererID(), 0, 0, 0, static_cast<GLint>(layer),
                                       static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1,
                                       GL_DEPTH_COMPONENT, GL_FLOAT,
                                       static_cast<GLsizei>(depth.size() * sizeof(f32)), depth.data());
                for (f32 d : depth)
                {
                    if (d < 0.999f)
                    {
                        ++casterTexels;
                    }
                }
            }
            return casterTexels;
        }
    };

    // The fix: a two-sided planar caster, lit from the front, renders into the shadow map.
    TEST_F(TwoSidedShadowCast, TwoSidedPlanarCasterCastsWhenLitFromFront)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ASSERT_TRUE(m_Quad.GetComponent<MaterialComponent>().m_Material.GetFlag(MaterialFlag::TwoSided));

        EXPECT_GT(CasterTexelsAfterScrub(), static_cast<sizet>(100))
            << "a TWO-SIDED planar caster lit from the front rendered NOTHING into the shadow map.\n\n"
               "ShadowRenderPass front-face-culls every caster, so a single-sided-geometry quad whose "
               "only face is turned toward the light is dropped from the shadow map — it casts no "
               "shadow (issue #650). A material flagged TwoSided must be rendered into the shadow map "
               "with culling disabled, exactly as it already is in the main pass. Split the shadow "
               "caster draw by two-sidedness in ShadowRenderPass::RenderCascadeOrFace.";
    }

    // The complement, so the fix stays narrow: a SINGLE-sided planar caster lit from the front
    // still casts nothing (front-culling is intentional for closed geometry and must be kept).
    // This documents that the fix is scoped to TwoSided and does not blanket-disable culling.
    TEST_F(TwoSidedShadowCast, SingleSidedPlanarCasterStillDoesNotCastFromFront)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        m_Quad.GetComponent<MaterialComponent>().m_Material.SetFlag(MaterialFlag::TwoSided, false);

        EXPECT_LT(CasterTexelsAfterScrub(), static_cast<sizet>(100))
            << "a SINGLE-sided planar caster lit from the front put depth in the shadow map — the fix "
               "for #650 disabled front-face culling too broadly. Only TwoSided casters should skip "
               "the front-face cull; single-sided geometry keeps it (peter-panning).";
    }
} // namespace OloEngine::Tests
