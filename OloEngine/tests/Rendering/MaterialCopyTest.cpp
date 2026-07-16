// OLO_TEST_LAYER: unit
//
// Copy-completeness guard for Material's THREE hand-maintained copy paths (issue #629).
//
// Material cannot use `= default` copy/assignment: its base (RendererResource -> Asset) is
// RefCounted and DELETES its copy/move operations on purpose, so the derived copy ctor and
// operator= are hand-written — each default-constructs a fresh base identity and then copies
// the derived fields one by one. That hand-maintained field list is exactly what dropped
// m_AlphaMode + m_AlphaCutoff (an alpha-masked material copied back OPAQUE, so Model::DrawParallel
// — which copies the material by value into MeshSubmitDesc::MaterialData — cut a leaf out on one
// path and rendered a solid card on another). Material::Copy was a THIRD list that had drifted
// even further (it also dropped every PBR factor and the IBL maps); it now delegates to the copy
// constructor, so the two remaining lists are the copy ctor and operator=.
//
// This test sets EVERY non-GL Material field to a distinct non-default sentinel, copies through
// all three routes, and asserts the sentinel survives. Reverting the alpha (or any covered) field
// from either list makes this fail — it is the loud tripwire that the silent field list lacked.
//
// >>> When you add a copyable field to Material, add it to the copy ctor, operator=, AND the
//     sentinel/assert pair below. <<<
//
// (Texture / IBL-map Ref<> fields are pointer-copies that need a live GL context to instantiate a
// real texture; they are exercised by the rendering path-parity tests, not here. This unit test
// owns the scalar / enum / vector / string / uniform-map fields — which is where all three drift
// bugs actually landed.)

#include "OloEngine/Renderer/Material.h"

#include <gtest/gtest.h>

using namespace OloEngine;

namespace
{
    // A shader is not needed to exercise copy semantics — Material default-constructs with a null
    // shader and every field below is set through the public API without touching GL.
    Ref<Material> MakeSentinelMaterial()
    {
        auto m = Ref<Material>::Create();
        m->SetName("sentinel-material");
        m->SetType(MaterialType::PBR);

        // Flags: two distinct bits, neither of them the DepthTest default.
        m->SetFlag(MaterialFlag::TwoSided, true);
        m->SetFlag(MaterialFlag::DisableShadowCasting, true);

        m->SetAmbient(glm::vec3(0.11f, 0.22f, 0.33f));
        m->SetDiffuse(glm::vec3(0.44f, 0.55f, 0.66f));
        m->SetSpecular(glm::vec3(0.77f, 0.88f, 0.99f));
        m->SetShininess(37.5f);

        m->SetBaseColorFactor(glm::vec4(0.1f, 0.2f, 0.3f, 0.4f));
        m->SetEmissiveFactor(glm::vec4(0.5f, 0.6f, 0.7f, 0.8f));
        m->SetMetallicFactor(0.625f);
        m->SetRoughnessFactor(0.125f);
        m->SetNormalScale(1.75f);
        m->SetOcclusionStrength(0.875f);
        m->SetEnableIBL(true);

        // The two fields that regressed (issue #629): a non-default alpha mode + cutoff.
        m->SetAlphaMode(AlphaMode::Mask);
        m->SetAlphaCutoff(0.375f);

        // A representative uniform of each scalar kind, to guard the uniform maps.
        m->Set("u_TestFloat", 12.5f);
        m->Set("u_TestInt", 7);
        m->Set("u_TestBool", true);

        return m;
    }

    // Asserts every sentinel field set above survived the copy.
    void ExpectSentinelFields(const Material& m)
    {
        EXPECT_EQ(m.GetName(), "sentinel-material");
        EXPECT_EQ(m.GetType(), MaterialType::PBR);

        EXPECT_TRUE(m.GetFlag(MaterialFlag::TwoSided));
        EXPECT_TRUE(m.GetFlag(MaterialFlag::DisableShadowCasting));

        EXPECT_FLOAT_EQ(m.GetAmbient().x, 0.11f);
        EXPECT_FLOAT_EQ(m.GetAmbient().z, 0.33f);
        EXPECT_FLOAT_EQ(m.GetDiffuse().y, 0.55f);
        EXPECT_FLOAT_EQ(m.GetSpecular().z, 0.99f);
        EXPECT_FLOAT_EQ(m.GetShininess(), 37.5f);

        EXPECT_FLOAT_EQ(m.GetBaseColorFactor().w, 0.4f);
        EXPECT_FLOAT_EQ(m.GetEmissiveFactor().x, 0.5f);
        EXPECT_FLOAT_EQ(m.GetMetallicFactor(), 0.625f);
        EXPECT_FLOAT_EQ(m.GetRoughnessFactor(), 0.125f);
        EXPECT_FLOAT_EQ(m.GetNormalScale(), 1.75f);
        EXPECT_FLOAT_EQ(m.GetOcclusionStrength(), 0.875f);
        EXPECT_TRUE(m.IsIBLEnabled());

        // The regression the guard exists for.
        EXPECT_EQ(m.GetAlphaMode(), AlphaMode::Mask);
        EXPECT_FLOAT_EQ(m.GetAlphaCutoff(), 0.375f);

        EXPECT_FLOAT_EQ(m.GetFloat("u_TestFloat"), 12.5f);
        EXPECT_EQ(m.GetInt("u_TestInt"), 7);
        EXPECT_TRUE(m.GetBool("u_TestBool"));
    }
} // namespace

TEST(MaterialCopyTest, CopyConstructorPreservesEveryField)
{
    const Ref<Material> original = MakeSentinelMaterial();
    const Material copy(*original);
    ExpectSentinelFields(copy);
}

TEST(MaterialCopyTest, CopyAssignmentPreservesEveryField)
{
    const Ref<Material> original = MakeSentinelMaterial();

    // Assign onto a fully-default material so a dropped field is visibly the default, not a
    // coincidental match with the sentinel.
    Material assigned;
    assigned = *original;
    ExpectSentinelFields(assigned);
}

TEST(MaterialCopyTest, StaticCopyPreservesEveryField)
{
    const Ref<Material> original = MakeSentinelMaterial();
    const Ref<Material> copy = Material::Copy(original);
    ASSERT_TRUE(copy);
    ExpectSentinelFields(*copy);
}

TEST(MaterialCopyTest, StaticCopyRenamesButKeepsFields)
{
    const Ref<Material> original = MakeSentinelMaterial();
    const Ref<Material> copy = Material::Copy(original, "renamed");
    ASSERT_TRUE(copy);
    EXPECT_EQ(copy->GetName(), "renamed");
    // Everything except the name still round-trips.
    EXPECT_EQ(copy->GetAlphaMode(), AlphaMode::Mask);
    EXPECT_FLOAT_EQ(copy->GetAlphaCutoff(), 0.375f);
    EXPECT_FLOAT_EQ(copy->GetMetallicFactor(), 0.625f);
}
