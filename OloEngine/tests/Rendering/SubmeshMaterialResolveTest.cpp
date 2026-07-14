// OLO_TEST_LAYER: unit
//
// The ONE material-precedence rule every mesh submission path must obey:
//   MaterialComponent override -> the submesh's IMPORTED material -> engine default.
//
// This existed only in Renderer3D::SubmitVirtualMesh. The classic paths each implemented
// a different half of it — the Scene MeshComponent loop did "override -> default" and never
// consulted the imported material (so a 103-submesh Sponza pushed through a MeshComponent
// shaded entirely flat engine-default grey), while Model::DrawParallel did
// "imported -> default" and ignored an authored MaterialComponent. Issue #629.
//
// The rule now lives in ONE header (SubmeshMaterialResolve.h) that all of them call; these
// tests pin the rule itself, including the two ways a mesh can carry no material at all
// (a procedurally-built MeshSource, and a submesh whose material index is out of range —
// UINT32_MAX is the importer's "no material" sentinel).

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        // A two-submesh MeshSource: submesh 0 -> material slot 0, submesh 1 -> material slot 1.
        // Geometry is irrelevant here (nothing is rendered), the submesh table is the point.
        Ref<MeshSource> MakeTwoSubmeshSource(u32 materialIndex0, u32 materialIndex1)
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

            Submesh a;
            a.m_BaseVertex = 0;
            a.m_BaseIndex = 0;
            a.m_VertexCount = 3;
            a.m_IndexCount = 3;
            a.m_MaterialIndex = materialIndex0;
            source->AddSubmesh(a);

            Submesh b;
            b.m_BaseVertex = 3;
            b.m_BaseIndex = 3;
            b.m_VertexCount = 3;
            b.m_IndexCount = 3;
            b.m_MaterialIndex = materialIndex1;
            source->AddSubmesh(b);

            return source;
        }

        Material MakeNamed(const char* name, const glm::vec3& color)
        {
            Ref<Material> const material = Material::CreatePBR(name, color, 0.0f, 0.5f);
            return material ? *material : Material{};
        }
    } // namespace

    TEST(SubmeshMaterialResolve, ImportedMaterialShadesEachSubmeshWhenThereIsNoOverride)
    {
        // The regression: this is what the classic MeshComponent path never did.
        Ref<MeshSource> source = MakeTwoSubmeshSource(0, 1);
        source->SetImportedMaterials({ Material::CreatePBR("Red", glm::vec3(1.0f, 0.0f, 0.0f)),
                                       Material::CreatePBR("Blue", glm::vec3(0.0f, 0.0f, 1.0f)) });

        const Material engineDefault = MakeNamed("EngineDefault", glm::vec3(0.8f));

        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, source.get(), 0u, engineDefault).GetName(), "Red");
        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, source.get(), 1u, engineDefault).GetName(), "Blue");
    }

    TEST(SubmeshMaterialResolve, MaterialComponentOverrideBeatsEverySubmeshsImportedMaterial)
    {
        Ref<MeshSource> source = MakeTwoSubmeshSource(0, 1);
        source->SetImportedMaterials({ Material::CreatePBR("Red", glm::vec3(1.0f, 0.0f, 0.0f)),
                                       Material::CreatePBR("Blue", glm::vec3(0.0f, 0.0f, 1.0f)) });

        const Material engineDefault = MakeNamed("EngineDefault", glm::vec3(0.8f));
        const Material override = MakeNamed("Override", glm::vec3(0.0f, 1.0f, 0.0f));

        EXPECT_EQ(ResolveSubmeshMaterial(&override, source.get(), 0u, engineDefault).GetName(), "Override");
        EXPECT_EQ(ResolveSubmeshMaterial(&override, source.get(), 1u, engineDefault).GetName(), "Override");
    }

    TEST(SubmeshMaterialResolve, ProcedurallyBuiltMeshSourceWithNoImportedMaterialsFallsBackToTheDefault)
    {
        // A MeshPrimitives cube / a runtime-generated mesh never had materials imported —
        // GetImportedMaterialForSubmesh() returns null and the default must apply cleanly.
        Ref<MeshSource> source = MakeTwoSubmeshSource(0, 1);
        const Material engineDefault = MakeNamed("EngineDefault", glm::vec3(0.8f));

        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, source.get(), 0u, engineDefault).GetName(), "EngineDefault");
        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, source.get(), 1u, engineDefault).GetName(), "EngineDefault");
    }

    TEST(SubmeshMaterialResolve, OutOfRangeAndSentinelMaterialIndicesFallBackToTheDefault)
    {
        // UINT32_MAX is Model::ProcessMesh's "this mesh had no material" sentinel; an index
        // past the end of the imported array is what a stale/mis-built cache produces.
        Ref<MeshSource> source = MakeTwoSubmeshSource(UINT32_MAX, 7u);
        source->SetImportedMaterials({ Material::CreatePBR("Red", glm::vec3(1.0f, 0.0f, 0.0f)) });

        const Material engineDefault = MakeNamed("EngineDefault", glm::vec3(0.8f));

        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, source.get(), 0u, engineDefault).GetName(), "EngineDefault");
        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, source.get(), 1u, engineDefault).GetName(), "EngineDefault");
        // Submesh index itself out of range.
        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, source.get(), 99u, engineDefault).GetName(), "EngineDefault");
    }

    TEST(SubmeshMaterialResolve, NullMeshSourceResolvesToOverrideThenDefault)
    {
        const Material engineDefault = MakeNamed("EngineDefault", glm::vec3(0.8f));
        const Material override = MakeNamed("Override", glm::vec3(0.0f, 1.0f, 0.0f));

        EXPECT_EQ(ResolveSubmeshMaterial(nullptr, static_cast<const MeshSource*>(nullptr), 0u, engineDefault).GetName(),
                  "EngineDefault");
        EXPECT_EQ(ResolveSubmeshMaterial(&override, static_cast<const MeshSource*>(nullptr), 0u, engineDefault).GetName(),
                  "Override");
    }

    TEST(SubmeshMaterialResolve, ShadowCastingIsDecidedByTheResolvedMaterialNotTheEntity)
    {
        // The classic paths gated shadow casting on the ENTITY's MaterialComponent, so an
        // entity with no MaterialComponent cast shadows unconditionally — Sponza's alpha-
        // masked plants projected solid quad silhouettes through MeshComponent while the
        // virtual path (which asks the resolved per-part material) correctly cast none.
        Material opaque = MakeNamed("Opaque", glm::vec3(0.5f));
        EXPECT_TRUE(MaterialCastsShadows(opaque));

        Material masked = MakeNamed("Masked", glm::vec3(0.5f));
        masked.SetAlphaMode(AlphaMode::Mask);
        EXPECT_FALSE(MaterialCastsShadows(masked));

        Material blended = MakeNamed("Blended", glm::vec3(0.5f));
        blended.SetAlphaMode(AlphaMode::Blend);
        EXPECT_FALSE(MaterialCastsShadows(blended));

        Material disabled = MakeNamed("ShadowsOff", glm::vec3(0.5f));
        disabled.SetFlag(MaterialFlag::DisableShadowCasting, true);
        EXPECT_FALSE(MaterialCastsShadows(disabled));
    }
} // namespace OloEngine::Tests
