// =============================================================================
// FoliageAlphaCoverageWarningTest.cpp — issue #1399, through the real renderer.
//
// FoliageAlphaCoverageContractTest pins the arithmetic on synthetic images.
// This drives REAL layers through Scene -> FoliageRenderer::GenerateInstances
// with the repository's own vegetation, and pins what an author would see:
//
//   * the configuration the issue was found on — grass.png mapped onto the
//     procedural pine — warns, once, naming the layer and the mesh part;
//   * the shipped Woodland Pines layer stays quiet, near mesh and impostor
//     alike — and its impostor atlas is made of the pine's own materials. It
//     used to be baked with the layer's BILLBOARD painted over the pine's atlas
//     UVs (12% of the surface passed), which after the OBJ v fix left the far
//     pines all but invisible;
//   * an impostor-only layer is judged through its bake, part by part;
//   * a correctly authored grass card stays quiet, and an opaque texture on a
//     card warns as a solid rectangle;
//   * a slider dragged within an implausible range does not repeat the line,
//     and dragging back into it after leaving does;
//   * the shipped grass, imported by the real Model path, passes ~92% of its
//     blade surface — the end-to-end guard for the OBJ v convention (#1399
//     found the #1398 plants sampling their atlases upside down at ~45%).
//
// The expected fractions are an INDEPENDENT measurement: a NumPy sampler over
// the OBJ files, run while writing this test, with the engine's own UV
// convention (Model flips every OBJ's v, and Texture2D flips on upload). So
// this also checks that the C++ path samples the texels the renderer samples.
//
// Needs a GL 4.6 context for the mesh import and the impostor bake; SKIPs
// cleanly without one.
//
// OLO_TEST_LAYER: integration
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "ScopedWarningCapture.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageAlphaCoverage.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    namespace
    {
        namespace AC = FoliageAlphaCoverage;

        // Relative to OloEditor/, the suite's working directory.
        constexpr const char* kStandInPine = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kStandInAlbedo = "assets/textures/grass.png";
        constexpr const char* kPine = "SandboxProject/Assets/Models/Vegetation/pine/pine.obj";
        constexpr const char* kPineCard = "SandboxProject/Assets/Models/Vegetation/pine/Textures/pine_card.png";
        constexpr const char* kPineBark = "SandboxProject/Assets/Models/Vegetation/pine/Textures/pine_bark.png";
        constexpr const char* kGrassCard = "SandboxProject/Assets/Models/Vegetation/grass/Textures/grass_card.png";
        constexpr const char* kGrassMesh = "SandboxProject/Assets/Models/Vegetation/grass/grass.obj";

        enum LayerSlot : u32
        {
            kStandInSlot = 0,
            kWoodlandSlot,
            kMeadowSlot,
            kSolidCardSlot,
            kGrassMeshSlot,
            kImpostorOnlySlot,
            kSlotCount
        };

        // Share of the Woodland pine's impostor atlas its baked silhouettes
        // cover, read back from the GPU. Measured with the parts baked from
        // their own materials: 8.7%. With the layer billboard painted over
        // every part, as before #1399: 3.9% (a probe of the old behaviour).
        // The floor sits between the two.
        constexpr f64 kImpostorFloor = 0.06;

        [[nodiscard]] const AC::Entry* FindEntry(std::span<const AC::Entry> entries, AC::Role kind,
                                                 std::string_view texture = {})
        {
            const auto it = std::ranges::find_if(entries, [&](const AC::Entry& entry)
                                                 { return entry.Kind == kind &&
                                                          (texture.empty() || entry.Texture.ToView().find(texture) != std::string_view::npos); });
            return it == entries.end() ? nullptr : &*it;
        }
    } // namespace

    class FoliageAlphaCoverageWarningTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(64, 64);

            m_TerrainEntity = scene.CreateEntity("Terrain");
            auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 7;
            terrain.m_ProceduralResolution = 64;
            terrain.m_ProceduralOctaves = 2;
            terrain.m_WorldSizeX = 64.0f;
            terrain.m_WorldSizeZ = 64.0f;
            terrain.m_HeightScale = 2.0f;
            terrain.m_TessellationEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                terrain.m_Material->AddLayer(layer);

            auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
            foliage.m_Enabled = true;
            foliage.m_Layers.SetNum(kSlotCount);

            // The configuration #1224's acceptance pass found by eye.
            FoliageLayer& standIn = foliage.m_Layers[kStandInSlot];
            standIn.Name = "StandInPine";
            standIn.MeshPath = kStandInPine;
            standIn.AlbedoPath = kStandInAlbedo;
            standIn.UseAuthoredMesh = true;
            standIn.AlphaCutoff = 0.3f;

            // Scenes/FoliageMeadowToWoodland.olo's Woodland Pines, as shipped.
            FoliageLayer& woodland = foliage.m_Layers[kWoodlandSlot];
            woodland.Name = "WoodlandPines";
            woodland.MeshPath = kPine;
            woodland.AlbedoPath = kPineCard;
            woodland.UseAuthoredMesh = true;
            woodland.UseImpostor = true;
            woodland.AlphaCutoff = 0.3f;

            FoliageLayer& meadow = foliage.m_Layers[kMeadowSlot];
            meadow.Name = "MeadowCard";
            meadow.AlbedoPath = kGrassCard;
            meadow.AlphaCutoff = 0.5f;

            // Bark on a billboard: an image with no transparency at all.
            FoliageLayer& solid = foliage.m_Layers[kSolidCardSlot];
            solid.Name = "SolidCard";
            solid.AlbedoPath = kPineBark;
            solid.AlphaCutoff = 0.5f;

            // Scenes/FoliageMeadowToWoodland.olo's Meadow Grass mesh, as shipped.
            FoliageLayer& grass = foliage.m_Layers[kGrassMeshSlot];
            grass.Name = "MeadowGrassMesh";
            grass.MeshPath = kGrassMesh;
            grass.AlbedoPath = kGrassCard;
            grass.UseAuthoredMesh = true;
            grass.AlphaCutoff = 0.5f;

            // The stand-in configuration again, drawn ONLY as an impostor: its
            // one part has no material texture, so the bake uses the layer
            // albedo, and the near mesh never measures it.
            FoliageLayer& impostorOnly = foliage.m_Layers[kImpostorOnlySlot];
            impostorOnly.Name = "ImpostorOnlyPine";
            impostorOnly.MeshPath = kStandInPine;
            impostorOnly.AlbedoPath = kStandInAlbedo;
            impostorOnly.UseAuthoredMesh = false;
            impostorOnly.UseImpostor = true;
            impostorOnly.AlphaCutoff = 0.3f;

            for (auto& layer : foliage.m_Layers)
            {
                layer.Density = 0.01f;
                layer.SplatmapChannel = -1;
                layer.MinSlopeAngle = 0.0f;
                layer.MaxSlopeAngle = 90.0f;
                layer.WindStrength = 0.0f;
            }
            foliage.m_NeedsRebuild = true;
        }

        void Regenerate()
        {
            m_TerrainEntity.GetComponent<FoliageComponent>().m_NeedsRebuild = true;
            RunOneFrame();
        }

        void RunOneFrame()
        {
            EditorCamera camera(60.0f, 1.0f, 0.5f, 500.0f);
            camera.SetViewportSize(64.0f, 64.0f);
            camera.SetPose(glm::vec3(32.0f, 20.0f, 90.0f), 0.0f, 0.3f);
            RunEditorFrames(camera, 1);
        }

        [[nodiscard]] FoliageComponent& Foliage()
        {
            return m_TerrainEntity.GetComponent<FoliageComponent>();
        }

        [[nodiscard]] std::span<const AC::Entry> Coverage(u32 slot)
        {
            const auto& renderer = Foliage().m_Renderer;
            return renderer ? renderer->GetAlphaCoverage(slot) : std::span<const AC::Entry>{};
        }

        Entity m_TerrainEntity;
    };

    TEST_F(FoliageAlphaCoverageWarningTest, TheStandInPineWarnsOnceAndTheShippedArtOnlyWhereItIsWrong)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedWarningCapture warnings;
        RunOneFrame();
        ASSERT_TRUE(Foliage().m_Renderer) << "the foliage system never generated";

        // ── The stand-in pine: grass.png on the procedural conifer ───────────
        {
            const auto entries = Coverage(kStandInSlot);
            const AC::Entry* mesh = FindEntry(entries, AC::Role::AuthoredMesh);
            ASSERT_NE(mesh, nullptr) << "the authored mesh was not measured - did pine.obj load?";
            ASSERT_TRUE(mesh->Measured);
            // No material texture, so the part draws the layer albedo.
            EXPECT_NE(mesh->Texture.ToView().find("grass.png"), std::string_view::npos);
            const f32 fraction = mesh->Coverage.PassFraction(0.3f);
            EXPECT_NEAR(fraction, 0.194f, 0.02f) << "independent NumPy measurement: 19.4% of the pine's surface";
            EXPECT_EQ(AC::Judge(AC::Role::AuthoredMesh, fraction), AC::Verdict::TooSparse);

            // Its CARD is the same texture on a quad, and 17.9% is right there.
            const AC::Entry* card = FindEntry(entries, AC::Role::Card);
            ASSERT_NE(card, nullptr);
            EXPECT_NEAR(card->Coverage.PassFraction(0.3f), 0.179f, 0.005f) << "the issue's own figure";
            EXPECT_EQ(AC::Judge(AC::Role::Card, card->Coverage.PassFraction(0.3f)), AC::Verdict::Plausible);

            EXPECT_EQ(warnings.Count("layer 'StandInPine' draws part 0 of 'pine.obj'"), 1u);
            EXPECT_EQ(warnings.Count("layer 'StandInPine' draws its card"), 0u);
        }

        // ── Woodland Pines, as shipped ───────────────────────────────────────
        {
            const auto entries = Coverage(kWoodlandSlot);
            u32 parts = 0;
            for (const auto& entry : entries)
            {
                if (entry.Kind != AC::Role::AuthoredMesh)
                    continue;
                ++parts;
                ASSERT_TRUE(entry.Measured) << entry.Texture.ToView();
                EXPECT_EQ(AC::Judge(entry.Kind, entry.Coverage.PassFraction(0.3f)), AC::Verdict::Plausible)
                    << entry.Surface.ToView() << " '" << entry.Texture.ToView() << "' passes "
                    << entry.Coverage.PassFraction(0.3f);
            }
            EXPECT_EQ(parts, 3u) << "trunk, bark and foliage each carry their own texture";

            const AC::Entry* foliage = FindEntry(entries, AC::Role::AuthoredMesh, "pine_foliage.png");
            ASSERT_NE(foliage, nullptr);
            EXPECT_NEAR(foliage->Coverage.PassFraction(0.3f), 0.544f, 0.02f);

            // The bake draws the near mesh's own parts and textures, so with the
            // mesh drawn it is not judged a second time; and the impostor rides
            // the card draw, so the flat card is never drawn either.
            EXPECT_EQ(FindEntry(entries, AC::Role::ImpostorBake), nullptr);
            EXPECT_EQ(FindEntry(entries, AC::Role::Card), nullptr);
            EXPECT_EQ(warnings.Count("layer 'WoodlandPines'"), 0u) << "the shipped pines are correct and must stay quiet";

            // What the far pines are made of, read back from the GPU: the share
            // of the atlas the baked silhouettes cover. See kImpostorFloor.
            const ImpostorAtlas* atlas = Foliage().m_Renderer->GetImpostorAtlas(kWoodlandSlot);
            ASSERT_NE(atlas, nullptr) << "Woodland Pines baked no impostor";
            TArray64<u8> texels;
            ASSERT_TRUE(atlas->Albedo->GetData(texels)) << "impostor atlas readback failed";
            ASSERT_FALSE(texels.IsEmpty());
            u64 covered = 0;
            for (i64 i = 3; i < texels.Num(); i += 4)
                covered += texels[i] > 0 ? 1u : 0u;
            const f64 atlasCoverage = static_cast<f64>(covered) / static_cast<f64>(texels.Num() / 4);
            EXPECT_GT(atlasCoverage, kImpostorFloor)
                << "the far pines are baked mostly empty: is the impostor painted with the layer billboard instead of "
                   "the pine's own materials?";
        }

        // ── An impostor-only layer, judged through its bake ──────────────────
        {
            const auto entries = Coverage(kImpostorOnlySlot);
            const AC::Entry* baked = FindEntry(entries, AC::Role::ImpostorBake, "grass.png");
            ASSERT_NE(baked, nullptr) << "the impostor-only layer's bake was not measured";
            ASSERT_TRUE(baked->Measured);
            EXPECT_NEAR(baked->Coverage.PassFraction(0.3f), 0.194f, 0.02f) << "the stand-in pine's surface, again";
            EXPECT_EQ(FindEntry(entries, AC::Role::AuthoredMesh), nullptr) << "UseAuthoredMesh is off";
            EXPECT_EQ(warnings.Count("layer 'ImpostorOnlyPine' bakes part 0 of 'pine.obj' into its impostor"), 1u);
        }

        // ── Cards ────────────────────────────────────────────────────────────
        {
            const AC::Entry* meadow = FindEntry(Coverage(kMeadowSlot), AC::Role::Card);
            ASSERT_NE(meadow, nullptr);
            EXPECT_NEAR(meadow->Coverage.PassFraction(0.5f), 0.120f, 0.005f);
            EXPECT_EQ(warnings.Count("layer 'MeadowCard'"), 0u) << "a 12% grass card is correct, not a defect";

            const AC::Entry* solid = FindEntry(Coverage(kSolidCardSlot), AC::Role::Card);
            ASSERT_NE(solid, nullptr);
            EXPECT_FLOAT_EQ(solid->Coverage.PassFraction(0.5f), 1.0f);
            EXPECT_EQ(warnings.Count("layer 'SolidCard' draws its card"), 1u);
            EXPECT_EQ(warnings.Count("solid rectangle"), 1u);
        }

        // ── The OBJ v convention, through the real import ────────────────────
        {
            const AC::Entry* blades = FindEntry(Coverage(kGrassMeshSlot), AC::Role::AuthoredMesh, "grass_blades.png");
            ASSERT_NE(blades, nullptr) << "the grass mesh's own blade texture was not measured";
            ASSERT_TRUE(blades->Measured);
            // 92.4% as authored (tools/vegetation-import/measure_coverage.py);
            // 45% when the atlas is sampled upside down, which is what a
            // standard bottom-up OBJ v gives once Model flips it.
            EXPECT_GT(blades->Coverage.PassFraction(0.5f), 0.85f)
                << "the grass blades sample their atlas upside down: was grass.obj written with standard OBJ v? "
                   "Model flips every OBJ's v (vegetation-asset-import.md rule 4)";
            EXPECT_EQ(warnings.Count("layer 'MeadowGrassMesh'"), 0u);
        }
    }

    TEST_F(FoliageAlphaCoverageWarningTest, ASliderDraggedThroughABadRangeWarnsOncePerEntry)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedWarningCapture warnings;
        constexpr std::string_view kMarker = "layer 'StandInPine' draws part 0";

        RunOneFrame();
        ASSERT_EQ(warnings.Count(kMarker), 1u);

        // Still implausible at every step of a drag: no repeat.
        auto& layer = Foliage().m_Layers[kStandInSlot];
        for (const f32 cutoff : { 0.32f, 0.35f, 0.4f })
        {
            layer.AlphaCutoff = cutoff;
            Regenerate();
        }
        EXPECT_EQ(warnings.Count(kMarker), 1u) << "one line per implausible configuration, not per slider tick";

        // Out of the bad range (cutoff 0 keeps every texel) and back in.
        layer.AlphaCutoff = 0.0f;
        Regenerate();
        EXPECT_EQ(warnings.Count(kMarker), 1u);
        layer.AlphaCutoff = 0.3f;
        Regenerate();
        EXPECT_EQ(warnings.Count(kMarker), 2u) << "leaving the bad range re-arms the warning";

        // A diagnostic: the authored value is never touched.
        EXPECT_FLOAT_EQ(layer.AlphaCutoff, 0.3f);

        // The same through an impostor, where every cutoff step RE-BAKES the
        // atlas. The bake's parts and textures do not change, so the entries
        // must not be re-measured — a re-measure would re-arm the latch and
        // repeat the line on every step.
        constexpr std::string_view kImpostorMarker = "layer 'ImpostorOnlyPine' bakes part 0";
        ASSERT_EQ(warnings.Count(kImpostorMarker), 1u);
        auto& impostorLayer = Foliage().m_Layers[kImpostorOnlySlot];
        for (const f32 cutoff : { 0.32f, 0.35f, 0.4f })
        {
            impostorLayer.AlphaCutoff = cutoff;
            Regenerate();
        }
        EXPECT_EQ(warnings.Count(kImpostorMarker), 1u) << "an impostor re-bake must not re-arm the warning";
    }
} // namespace OloEngine::Tests
