#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GltfPhysicalMaterialImportTest — headless, no GL context.
//
// Proves the glTF side of issue #970 against a REAL FILE rather than a mocked
// aiMaterial: the fixture at assets/models/TransmissionTest/TransmissionTest.gltf
// carries KHR_materials_transmission, KHR_materials_ior and
// KHR_materials_volume, and this test reads it with a raw Assimp::Importer (the
// same approach MeshInterchangeTest uses, and for the same reason -- the
// engine's Model path would need a GL context to Build()).
//
// WHY A REAL FILE MATTERS HERE. The import maps three extensions onto Assimp
// material keys, and whether Assimp's glTF2 importer actually populates those
// keys is an assumption about a third-party library, not about our code. A
// mock would assert our mapping against our own belief. This asserts it against
// Assimp 6.0.4 as vendored, so a dependency bump that changes the key -- e.g.
// stops routing KHR_materials_ior onto the generic $mat.refracti -- fails here
// instead of silently importing every glass material as IOR 1.5.
//
// The third fixture material is a CONTROL with no extensions at all: it pins
// the "no behaviour change for legacy materials" half, since a legacy material
// must come back with every physical field at its neutral default.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Renderer/GltfPhysicalMaterial.h"
#include "OloEngine/Renderer/Material.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

using namespace OloEngine;

namespace
{
    std::filesystem::path FixturePath()
    {
        return std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "models" / "TransmissionTest" /
               "TransmissionTest.gltf";
    }

    // Finds a fixture material by its authored name. Index order is an Assimp
    // implementation detail; the name is what the fixture actually promises.
    const aiMaterial* FindMaterial(const aiScene* scene, const std::string& wanted)
    {
        for (u32 i = 0; i < scene->mNumMaterials; ++i)
        {
            aiString name;
            if (scene->mMaterials[i]->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && wanted == name.C_Str())
                return scene->mMaterials[i];
        }
        return nullptr;
    }

    class GltfPhysicalMaterialImportTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            const auto path = FixturePath();
            ASSERT_TRUE(std::filesystem::exists(path))
                << "fixture missing: " << path.string()
                << " — it is checked in; see docs/guides/gltf-material-extensions.md";

            // No postprocessing flags that could touch materials: this test is
            // about what the importer read, not about the mesh.
            m_Scene = m_Importer.ReadFile(path.string(), aiProcess_Triangulate);
            ASSERT_NE(m_Scene, nullptr) << "Assimp failed to read the fixture: " << m_Importer.GetErrorString();
            ASSERT_GE(m_Scene->mNumMaterials, 3u);

            ResetPhysicalMaterialStats();
        }

        Assimp::Importer m_Importer;
        const aiScene* m_Scene = nullptr;
    };
} // namespace

TEST_F(GltfPhysicalMaterialImportTest, ThinGlassImportsTransmissionAndIorWithNoVolume)
{
    const aiMaterial* source = FindMaterial(m_Scene, "ThinGlass");
    ASSERT_NE(source, nullptr);

    Material material;
    ImportGltfPhysicalMaterial(source, material);

    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), 1.0f);
    EXPECT_TRUE(material.IsTransmissive());
    EXPECT_FLOAT_EQ(material.GetIOR(), 1.5f);

    // The fixture omits KHR_materials_volume entirely, so this material is
    // thin-walled: no thickness, no absorption. That is the documented
    // difference between "thin glass" and "a volume".
    EXPECT_FLOAT_EQ(material.GetThicknessFactor(), 0.0f);
    EXPECT_FALSE(material.HasVolume());
    EXPECT_TRUE(std::isinf(material.GetAttenuationDistance()));

    const glm::vec3 sigma = material.GetAttenuationSigma();
    EXPECT_FLOAT_EQ(sigma.r, 0.0f);
    EXPECT_FLOAT_EQ(sigma.g, 0.0f);
    EXPECT_FLOAT_EQ(sigma.b, 0.0f);
}

TEST_F(GltfPhysicalMaterialImportTest, TintedVolumeGlassImportsTheWholeVolumeExtension)
{
    const aiMaterial* source = FindMaterial(m_Scene, "TintedVolumeGlass");
    ASSERT_NE(source, nullptr);

    Material material;
    ImportGltfPhysicalMaterial(source, material);

    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), 0.9f);
    EXPECT_NEAR(material.GetIOR(), 1.52f, 1.0e-5f);

    // KHR_materials_volume: thickness, attenuation colour and a FINITE
    // attenuation distance — the three that make absorption depth-dependent.
    EXPECT_TRUE(material.HasVolume());
    EXPECT_NEAR(material.GetThicknessFactor(), 0.8f, 1.0e-5f);
    EXPECT_NEAR(material.GetAttenuationDistance(), 0.5f, 1.0e-5f);
    EXPECT_NEAR(material.GetAttenuationColor().r, 0.9f, 1.0e-5f);
    EXPECT_NEAR(material.GetAttenuationColor().g, 0.4f, 1.0e-5f);
    EXPECT_NEAR(material.GetAttenuationColor().b, 0.2f, 1.0e-5f);

    // The derived extinction must be finite, positive, and ordered like the
    // attenuation colour: the least-transmitted channel absorbs most.
    const glm::vec3 sigma = material.GetAttenuationSigma();
    for (int channel = 0; channel < 3; ++channel)
    {
        EXPECT_TRUE(std::isfinite(sigma[channel]));
        EXPECT_GT(sigma[channel], 0.0f);
    }
    EXPECT_GT(sigma.b, sigma.g);
    EXPECT_GT(sigma.g, sigma.r);
}

TEST_F(GltfPhysicalMaterialImportTest, TheControlMaterialImportsEveryPhysicalFieldAtItsNeutralDefault)
{
    const aiMaterial* source = FindMaterial(m_Scene, "OpaqueControl");
    ASSERT_NE(source, nullptr);

    Material material;
    ImportGltfPhysicalMaterial(source, material);

    // This is the "no behaviour change for legacy materials" assertion at the
    // import boundary: a material with no extensions must be indistinguishable
    // from one imported by a build that predates the feature.
    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), 0.0f);
    EXPECT_FALSE(material.IsTransmissive());
    EXPECT_FLOAT_EQ(material.GetThicknessFactor(), 0.0f);
    EXPECT_FALSE(material.HasVolume());
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().r, 1.0f);
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().g, 1.0f);
    EXPECT_FLOAT_EQ(material.GetAttenuationColor().b, 1.0f);

    const glm::vec3 sigma = material.GetAttenuationSigma();
    EXPECT_FLOAT_EQ(sigma.r, 0.0f);
    EXPECT_FLOAT_EQ(sigma.g, 0.0f);
    EXPECT_FLOAT_EQ(sigma.b, 0.0f);

    // The IOR is NOT read for a non-transmissive material at all. Assimp routes
    // KHR_materials_ior onto the generic $mat.refracti key, which OBJ/MTL also
    // writes on ordinary opaque materials (routinely 1.0, meaning F0 = 0), so
    // importing it unconditionally would contaminate every such material and
    // make it re-serialize with a PhysicalMaterial block it never had.
    EXPECT_FALSE(material.IsTransmissive());
    EXPECT_FLOAT_EQ(material.GetIOR(), kDefaultIOR);
}

TEST_F(GltfPhysicalMaterialImportTest, ImportIsIdempotentAndSurvivesANullMaterial)
{
    const aiMaterial* source = FindMaterial(m_Scene, "TintedVolumeGlass");
    ASSERT_NE(source, nullptr);

    Material material;
    ImportGltfPhysicalMaterial(source, material);
    const f32 transmission = material.GetTransmissionFactor();
    const f32 thickness = material.GetThicknessFactor();

    // Re-importing the same source must not accumulate or drift — the two glTF
    // walks (Model and AnimatedModel) can both reach a shared material.
    ImportGltfPhysicalMaterial(source, material);
    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), transmission);
    EXPECT_FLOAT_EQ(material.GetThicknessFactor(), thickness);

    // A null aiMaterial is a no-op, not a crash.
    ImportGltfPhysicalMaterial(nullptr, material);
    EXPECT_FLOAT_EQ(material.GetTransmissionFactor(), transmission);
}

TEST_F(GltfPhysicalMaterialImportTest, TheFixtureCarriesNoUnsupportedTexturesSoNothingIsCounted)
{
    // The fixture uses factors only, so the "ignored map" counters must stay at
    // zero. If a future fixture gains a transmission or thickness texture this
    // fails, which is the point: the drop has to be acknowledged, not absorbed.
    for (const char* name : { "ThinGlass", "TintedVolumeGlass", "OpaqueControl" })
    {
        const aiMaterial* source = FindMaterial(m_Scene, name);
        ASSERT_NE(source, nullptr) << name;
        Material material;
        ImportGltfPhysicalMaterial(source, material);
    }

    const PhysicalMaterialStats stats = GetPhysicalMaterialStats();
    EXPECT_EQ(stats.TransmissionMapsIgnored, 0u);
    EXPECT_EQ(stats.ThicknessMapsIgnored, 0u);
}
