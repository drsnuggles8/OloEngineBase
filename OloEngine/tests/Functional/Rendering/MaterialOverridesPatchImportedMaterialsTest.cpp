// OLO_TEST_LAYER: Functional
#include "OloEnginePCH.h"

// =============================================================================
// MaterialOverridesPatchImportedMaterialsTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene (MaterialOverridesComponent) × Asset (one imported MeshSource shared
//   by several entities) × Renderer (SubmeshMaterialResolve + the GPU Scene
//   material key), issue #1533.
//
// A MaterialOverride names one IMPORTED material and patches a COPY of it for
// one entity. Three things must hold on every path, and each fails silently:
//   - the patched submesh shades with the copy (kind, skin profile, thickness,
//     the optional factors) while its other fields — and every other submesh —
//     keep the imported values;
//   - the shared imported material is never modified, so a second entity using
//     the same mesh source is not re-skinned by the first one's patch;
//   - the GPU Scene record of the patched submesh is keyed to the copy, not to
//     the shared imported record, so "the record cannot name one material while
//     the draw shades with another" still holds.
// MaterialComponent keeps overriding every submesh, and an override that names
// no imported material is reported rather than quietly patching nothing.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "PropertyTests/ScopedWarningCapture.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialOverride.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>

#include <utility>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr u64 kNoseProfile = 0x0DD6'05E0'0000'0001ull;

    // Two submeshes, each with its own imported material slot — the shape of a
    // skinned body whose nose is a submesh of the one mesh.
    Ref<MeshSource> MakeTwoSubmeshSource()
    {
        TArray<Vertex> vertices;
        for (i32 i = 0; i < 6; ++i)
        {
            vertices.Add(Vertex(glm::vec3(static_cast<f32>(i), 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f)));
        }
        TArray<u32> indices;
        for (u32 i = 0; i < 6; ++i)
        {
            indices.Add(i);
        }
        auto source = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
        for (u32 s = 0; s < 2; ++s)
        {
            Submesh submesh;
            submesh.m_BaseVertex = s * 3;
            submesh.m_BaseIndex = s * 3;
            submesh.m_VertexCount = 3;
            submesh.m_IndexCount = 3;
            submesh.m_MaterialIndex = s;
            source->AddSubmesh(submesh);
        }

        // The imported materials carry non-default values in fields the patch
        // must NOT touch, so a copy that dropped one is visible.
        Ref<Material> nose = Material::CreatePBR("DogNose", glm::vec3(0.35f, 0.2f, 0.15f), 0.1f, 0.8f);
        nose->SetNormalScale(1.75f);
        nose->SetEmissiveFactor(glm::vec4(0.01f, 0.02f, 0.03f, 1.0f));
        nose->SetAlphaMode(AlphaMode::Mask);
        nose->SetAlphaCutoff(0.3f);
        Ref<Material> fur = Material::CreatePBR("DogFur", glm::vec3(0.6f, 0.45f, 0.3f), 0.0f, 0.9f);
        source->SetImportedMaterials(TArray<Ref<Material>>{ nose, fur });
        return source;
    }

    MaterialOverride MakeNoseOverride()
    {
        MaterialOverride patch;
        patch.MaterialName = "DogNose";
        patch.Kind = MaterialKind::Skin;
        patch.SkinProfile = kNoseProfile;
        patch.ThicknessFactor = 0.004f;
        patch.OverrideBaseColor = true;
        patch.BaseColor = glm::vec4(0.12f, 0.08f, 0.07f, 1.0f);
        patch.OverrideRoughness = true;
        patch.Roughness = 0.35f;
        // Metallic deliberately NOT overridden: the imported 0.1 must survive.
        return patch;
    }
} // namespace

class MaterialOverridesPatchImportedMaterialsTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // A registered mesh source, so its imported materials resolve to a real
        // Imported GPU Scene key (the owner half is the asset handle).
        EnableAssetManager({});
        m_Source = MakeTwoSubmeshSource();
        (void)AssetManager::AddMemoryOnlyAsset(m_Source);
        m_ImportedNose = m_Source->GetImportedMaterials()[0];
        m_ImportedFur = m_Source->GetImportedMaterials()[1];

        m_Dog = GetScene().CreateEntity("Dog");
        m_Dog.AddComponent<MeshComponent>(m_Source);
        m_Dog.AddComponent<MaterialOverridesComponent>().m_Overrides.Add(MakeNoseOverride());

        Ref<Material> fallback = Material::CreatePBR("EngineDefault", glm::vec3(0.8f), 0.0f, 0.5f);
        m_Default = *fallback;
    }

    [[nodiscard]] const MaterialOverrideCache* Patches(Entity entity)
    {
        return GetScene().PrepareMaterialOverrides(static_cast<entt::entity>(entity), m_Source->GetImportedMaterials());
    }

    [[nodiscard]] const Material& Resolve(Entity entity, u32 submesh)
    {
        const Material* overrideMaterial =
            entity.HasComponent<MaterialComponent>() ? &entity.GetComponent<MaterialComponent>().m_Material : nullptr;
        return ResolveSubmeshMaterial(overrideMaterial, Patches(entity), m_Source.get(), submesh, m_Default);
    }

    [[nodiscard]] GPUSceneMaterialKey Key(Entity entity, u32 submesh)
    {
        const Material* overrideMaterial =
            entity.HasComponent<MaterialComponent>() ? &entity.GetComponent<MaterialComponent>().m_Material : nullptr;
        const Material* imported = m_Source->GetImportedMaterialPtrForSubmesh(submesh);
        return Renderer3D::ResolveGPUSceneMaterialKey(overrideMaterial, ResolveMaterialPatch(Patches(entity), imported),
                                                      static_cast<u64>(entity.GetUUID()), m_Source, imported,
                                                      m_Source->GetSubmeshes()[static_cast<i32>(submesh)].m_MaterialIndex);
    }

    void ExpectImportedNoseUntouched() const
    {
        EXPECT_EQ(m_ImportedNose->GetMaterialKind(), MaterialKind::Generic);
        EXPECT_EQ(static_cast<u64>(m_ImportedNose->GetSkinProfileHandle()), 0ull);
        EXPECT_FLOAT_EQ(m_ImportedNose->GetThicknessFactor(), 0.0f);
        EXPECT_FLOAT_EQ(m_ImportedNose->GetRoughnessFactor(), 0.8f);
        EXPECT_FLOAT_EQ(m_ImportedNose->GetBaseColorFactor().r, 0.35f);
    }

    Ref<MeshSource> m_Source;
    Ref<Material> m_ImportedNose;
    Ref<Material> m_ImportedFur;
    Material m_Default;
    Entity m_Dog;
};

TEST_F(MaterialOverridesPatchImportedMaterialsTest, ThePatchedSubmeshShadesWithACopyAndTheOtherKeepsItsImportedMaterial)
{
    const Material& nose = Resolve(m_Dog, 0);
    const Material& fur = Resolve(m_Dog, 1);

    EXPECT_NE(&nose, m_ImportedNose.Raw()) << "the patched submesh must shade with a COPY, not the shared import";
    EXPECT_EQ(&fur, m_ImportedFur.Raw()) << "a submesh no override names must keep its imported material";

    // The patch.
    EXPECT_EQ(nose.GetMaterialKind(), MaterialKind::Skin);
    EXPECT_EQ(static_cast<u64>(nose.GetSkinProfileHandle()), kNoseProfile);
    EXPECT_FLOAT_EQ(nose.GetThicknessFactor(), 0.004f);
    EXPECT_FLOAT_EQ(nose.GetRoughnessFactor(), 0.35f);
    EXPECT_FLOAT_EQ(nose.GetBaseColorFactor().r, 0.12f);
    EXPECT_FLOAT_EQ(nose.GetBaseColorFactor().g, 0.08f);

    // Everything else is the imported material's.
    EXPECT_FLOAT_EQ(nose.GetMetallicFactor(), 0.1f) << "metallic was not overridden and must keep the imported value";
    EXPECT_EQ(nose.GetName(), "DogNose");
    EXPECT_FLOAT_EQ(nose.GetNormalScale(), 1.75f);
    EXPECT_FLOAT_EQ(nose.GetEmissiveFactor().b, 0.03f);
    EXPECT_EQ(nose.GetAlphaMode(), AlphaMode::Mask);
    EXPECT_FLOAT_EQ(nose.GetAlphaCutoff(), 0.3f);

    // And the shared import is exactly what it was.
    ExpectImportedNoseUntouched();
}

TEST_F(MaterialOverridesPatchImportedMaterialsTest, AMaterialComponentStillOverridesEverySubmesh)
{
    auto& materialComponent = m_Dog.AddComponent<MaterialComponent>();
    materialComponent.m_Material.SetName("Override");

    EXPECT_EQ(Patches(m_Dog), nullptr) << "a MaterialComponent overrides every submesh, so no patch may apply";
    EXPECT_EQ(&Resolve(m_Dog, 0), &materialComponent.m_Material);
    EXPECT_EQ(&Resolve(m_Dog, 1), &materialComponent.m_Material);
    EXPECT_EQ(Key(m_Dog, 0).m_Source, static_cast<u32>(GPUSceneMaterialSource::EntityOverride));
}

TEST_F(MaterialOverridesPatchImportedMaterialsTest, TheGpuSceneRecordNamesThePatchedCopyNotTheSharedImport)
{
    const GPUSceneMaterialKey patchedNose = Key(m_Dog, 0);
    const GPUSceneMaterialKey fur = Key(m_Dog, 1);
    const u64 dogId = static_cast<u64>(m_Dog.GetUUID());

    EXPECT_EQ(patchedNose.m_Source, static_cast<u32>(GPUSceneMaterialSource::EntityPatch));
    EXPECT_EQ(patchedNose.m_Owner, dogId);
    EXPECT_EQ(patchedNose.m_Slot, 0u);
    EXPECT_EQ(fur.m_Source, static_cast<u32>(GPUSceneMaterialSource::Imported));
    EXPECT_EQ(fur.m_Owner, static_cast<u64>(m_Source->GetHandle()));
    EXPECT_EQ(fur.m_Slot, 1u);
    EXPECT_NE(patchedNose, fur);

    // The unpatched key of the SAME submesh is the shared Imported record — the
    // one a second entity using this mesh keeps shading with. The patched key
    // must not be it.
    Entity other = GetScene().CreateEntity("OtherDog");
    other.AddComponent<MeshComponent>(m_Source);
    const GPUSceneMaterialKey sharedNose = Key(other, 0);
    EXPECT_EQ(sharedNose.m_Source, static_cast<u32>(GPUSceneMaterialSource::Imported));
    EXPECT_EQ(sharedNose.m_Owner, static_cast<u64>(m_Source->GetHandle()));
    EXPECT_NE(patchedNose, sharedNose);
    EXPECT_EQ(&Resolve(other, 0), m_ImportedNose.Raw())
        << "an entity without overrides must keep shading with the shared imported material";
}

TEST_F(MaterialOverridesPatchImportedMaterialsTest, EditingOrRemovingTheOverrideRebuildsFromTheImportedMaterial)
{
    ASSERT_FLOAT_EQ(Resolve(m_Dog, 0).GetRoughnessFactor(), 0.35f);

    // An edited field rebuilds the copy: a stale one would keep shading 0.35.
    m_Dog.GetComponent<MaterialOverridesComponent>().m_Overrides[0].Roughness = 0.6f;
    EXPECT_FLOAT_EQ(Resolve(m_Dog, 0).GetRoughnessFactor(), 0.6f);

    // An emptied list restores the imported material.
    m_Dog.GetComponent<MaterialOverridesComponent>().m_Overrides.Reset();
    EXPECT_EQ(Patches(m_Dog), nullptr);
    EXPECT_EQ(&Resolve(m_Dog, 0), m_ImportedNose.Raw());

    // So does removing the component.
    m_Dog.GetComponent<MaterialOverridesComponent>().m_Overrides.Add(MakeNoseOverride());
    EXPECT_NE(&Resolve(m_Dog, 0), m_ImportedNose.Raw());
    m_Dog.RemoveComponent<MaterialOverridesComponent>();
    EXPECT_EQ(&Resolve(m_Dog, 0), m_ImportedNose.Raw());

    ExpectImportedNoseUntouched();
}

TEST_F(MaterialOverridesPatchImportedMaterialsTest, TwoEntitiesSharingAMeshNeverShareAPatch)
{
    Entity pup = GetScene().CreateEntity("Pup");
    pup.AddComponent<MeshComponent>(m_Source);
    MaterialOverride pinkNose = MakeNoseOverride();
    pinkNose.BaseColor = glm::vec4(0.8f, 0.4f, 0.45f, 1.0f);
    pup.AddComponent<MaterialOverridesComponent>().m_Overrides.Add(pinkNose);

    const Material& dogNose = Resolve(m_Dog, 0);
    const Material& pupNose = Resolve(pup, 0);
    EXPECT_NE(&dogNose, &pupNose);
    EXPECT_FLOAT_EQ(dogNose.GetBaseColorFactor().r, 0.12f);
    EXPECT_FLOAT_EQ(pupNose.GetBaseColorFactor().r, 0.8f);
    EXPECT_NE(Key(m_Dog, 0), Key(pup, 0)) << "each entity's patched copy is its own record";
    ExpectImportedNoseUntouched();
}

TEST_F(MaterialOverridesPatchImportedMaterialsTest, ACopiedComponentStartsWithAnEmptyCache)
{
    (void)Resolve(m_Dog, 0); // build the cache
    const auto& original = m_Dog.GetComponent<MaterialOverridesComponent>();
    ASSERT_EQ(original.m_Cache.GetPatchCount(), 1u);

    // Play (Scene::Copy), duplication and undo snapshots all copy the component;
    // none of them may share the Material objects the original built.
    const MaterialOverridesComponent copy = original;
    EXPECT_EQ(copy.m_Cache.GetPatchCount(), 0u);
    EXPECT_TRUE(copy == original) << "the cache is derived state and must not take part in equality";
}

TEST_F(MaterialOverridesPatchImportedMaterialsTest, AnOverrideNamingNoImportedMaterialIsReportedOnceAndPatchesNothing)
{
    const Tests::ScopedWarningCapture warnings;
    auto& overrides = m_Dog.GetComponent<MaterialOverridesComponent>().m_Overrides;
    overrides[0].MaterialName = "DogNoze"; // the typo that looks like an unpatched mesh

    for (int frame = 0; frame < 4; ++frame)
    {
        EXPECT_EQ(&Resolve(m_Dog, 0), m_ImportedNose.Raw());
    }
    EXPECT_EQ(warnings.Count("override #0 ('DogNoze')"), 1u)
        << "an override that patches nothing must be reported, once per edit of the list";
    EXPECT_EQ(ClassifyMaterialOverride({ overrides.GetData(), static_cast<sizet>(overrides.Num()) }, 0,
                                       m_Source->GetImportedMaterials()),
              MaterialOverrideStatus::NoMatchingMaterial);
}
