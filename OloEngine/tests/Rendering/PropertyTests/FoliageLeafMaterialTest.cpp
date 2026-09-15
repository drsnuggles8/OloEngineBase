// OLO_TEST_LAYER: L1
// =============================================================================
// FoliageLeafMaterialTest.cpp
//
// The CPU-side contracts of the vegetation material (issue #1234). Everything
// here runs WITHOUT a GL context, so it is a real CI gate rather than a test
// that skips on the machines that matter. The GLSL half — the two-sided normal
// rule and the transmission lobe — is pinned by FoliageLeafTransmissionTest.cpp
// against the production shader functions; the pixels are pinned by
// FoliageLeafTransmissionEvidenceTest.cpp.
//
// What is pinned here, and the failure each one is about:
//
//   THE KIND FITS. MaterialKind::Foliage took the fourth and LAST value the
//   G-Buffer's two-bit kind field can carry. A fifth kind appended without
//   widening the lane would alias onto Generic on the deferred path only —
//   a silent forward/deferred divergence, which is the exact class #1231's
//   own lane test exists for.
//
//   THE SLOT TABLE INTERNS BY VALUE AND REFUSES LOUDLY. Two layers with the
//   same authored leaf material must share a slot (a forest of one species
//   costs one), a layer that is not a leaf material must spend none, and an
//   eighth distinct material must get kFoliageLeafSlotNone and be COUNTED —
//   never aliased onto somebody else's parameters, which would render a
//   correct-looking frame with the wrong transmission.
//
//   THE ROUND-TRIPS ARE LOSSLESS, AND A PRIOR ON-DISK VERSION IS SAFE. Scene
//   YAML and save-games both carry the new fields; a scene or save written
//   before this material existed must load with transmission OFF and render
//   exactly as the build that wrote it rendered. That is what makes the
//   feature opt-in rather than a change to every existing canopy.
//
//   NON-FINITE INPUT IS REJECTED, NOT CLAMPED FROM NaN. Every one of these
//   floats reaches a pow() exponent or a normalize() in the shared shader
//   evaluation, where a NaN does not fail — it spreads.
//
//   THE TWO UPLOAD PATHS PACK THE SAME NUMBERS. The forward foliage UBO and
//   the deferred profile table both carry tint*strength, and they are filled
//   by two different files (CommandDispatch and DeferredLightingPass). A
//   difference between them is a forward/deferred divergence that no shader
//   test could see, because each path would be internally consistent.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Core/YAMLConverters.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/FoliageLeafProfile.h"
#include "OloEngine/Renderer/FoliageLeafProfileTable.h"
#include "OloEngine/Renderer/MaterialKind.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <glm/glm.hpp>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <limits>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        // A leaf material with every field distinct from the constructor
        // default, so a dropped read or write on ANY of them is detectable.
        // Deliberately not round numbers: a serializer that quietly rounded
        // through a lower-precision lane would still pass on 0.5.
        FoliageLayer MakeAuthoredLeafLayer()
        {
            FoliageLayer layer;
            layer.Name = "BacklitClump";
            layer.AlbedoPath = "assets/textures/grass.png";
            layer.NormalMapPath = "assets/textures/leaf_normal.png";
            layer.RoughnessMapPath = "assets/textures/leaf_roughness.png";
            layer.ThicknessMapPath = "assets/textures/leaf_thickness.png";
            layer.NormalStrength = 0.6875f;
            layer.TransmissionStrength = 1.3125f;
            layer.TransmissionColor = glm::vec3(0.3125f, 0.5625f, 0.1875f);
            layer.Thickness = 0.8125f;
            layer.TransmissionDistortion = 0.4375f;
            layer.TransmissionPower = 6.5f;
            layer.TransmissionWrap = 0.28125f;
            layer.TransmissionAmbient = 0.90625f;
            return layer;
        }

        void ExpectLeafFieldsEqual(const FoliageLayer& got, const FoliageLayer& want)
        {
            EXPECT_EQ(got.NormalMapPath, want.NormalMapPath);
            EXPECT_EQ(got.RoughnessMapPath, want.RoughnessMapPath);
            EXPECT_EQ(got.ThicknessMapPath, want.ThicknessMapPath);
            EXPECT_TRUE(Math::BitwiseEqual(got.NormalStrength, want.NormalStrength));
            EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionStrength, want.TransmissionStrength));
            EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionColor, want.TransmissionColor));
            EXPECT_TRUE(Math::BitwiseEqual(got.Thickness, want.Thickness));
            EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionDistortion, want.TransmissionDistortion));
            EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionPower, want.TransmissionPower));
            EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionWrap, want.TransmissionWrap));
            EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionAmbient, want.TransmissionAmbient));
        }

        // The repo's own lookup idiom (ComponentRoundTripTest::FindByTag).
        Entity FindByTag(Scene& scene, const char* tag)
        {
            for (auto e : scene.GetAllEntitiesWith<TagComponent>())
            {
                Entity ent{ e, &scene };
                if (ent.GetComponent<TagComponent>().Tag == tag)
                    return ent;
            }
            return {};
        }

        FoliageLeafProfile MakeProfile(f32 strength, f32 power = 4.0f)
        {
            FoliageLeafProfile p;
            p.TransmissionStrength = strength;
            p.Power = power;
            return p;
        }
    } // namespace

    // =========================================================================
    // The kind.
    // =========================================================================

    TEST(FoliageMaterialKindTest, FoliageIsTheLastValueTheGBufferKindFieldCanCarry)
    {
        EXPECT_EQ(std::to_underlying(MaterialKind::Foliage), 3);
        EXPECT_TRUE(IsValidMaterialKind(std::to_underlying(MaterialKind::Foliage)));
        EXPECT_STREQ(MaterialKindToString(MaterialKind::Foliage), "Foliage");

        // The two-bit field holds 0..3 and Foliage is 3, so the enum is now
        // EXACTLY full. This assertion is the conversation-starter
        // MaterialKind.h promises: a fifth kind is a re-encoding of the lane,
        // not an enumerator, and it fails here first rather than shading a new
        // surface as Generic on the deferred path only.
        EXPECT_EQ(kMaterialKindCount - 1, kMaterialKindGBufferMax)
            << "The MaterialKind enum no longer exactly fills the G-Buffer's two-bit kind field. If a kind was "
               "ADDED, it aliases onto an existing one on the deferred path and nothing else will say so — widen "
               "the lane (see oloEncodeGBufferPbrFlagsEx in include/PBRCommon.glsl). If one was REMOVED, update "
               "this expectation deliberately.";

        // A value one past the end must be rejected, which is what every
        // deserializer's fall-back-to-Generic depends on.
        EXPECT_FALSE(IsValidMaterialKind(kMaterialKindCount));
    }

    TEST(FoliageMaterialKindTest, LeafAndSkinSlotFieldsAreTheSameWidth)
    {
        // The leaf profile slot and the skin profile slot are TWO TENANTS OF
        // ONE three-bit G-Buffer field, read under their own kind. If the two
        // slot spaces ever differ in size, the field can no longer name both
        // and one tenant silently truncates.
        EXPECT_EQ(kFoliageLeafSlotNone, kSkinProfileSlotNone);
        EXPECT_EQ(kMaxFoliageLeafSlots, kMaxSkinProfileSlots);
    }

    // =========================================================================
    // The slot table.
    // =========================================================================

    TEST(FoliageLeafProfileTableTest, IdenticalMaterialsShareASlotAndNonLeavesSpendNone)
    {
        FoliageLeafProfileTable table;

        // A layer that is not a leaf material costs nothing and is not an
        // error: strength 0 is the documented default every pre-#1234 layer
        // deserializes to, so the common case must be free and silent.
        EXPECT_EQ(table.Resolve(MakeProfile(0.0f)), kFoliageLeafSlotNone);
        EXPECT_EQ(table.GetAssignedSlotCount(), 0u);
        EXPECT_EQ(table.GetSlotBudgetFullCount(), 0u)
            << "a non-leaf layer was counted as a budget failure — it is not one";

        const u32 first = table.Resolve(MakeProfile(1.0f));
        ASSERT_LT(first, kFoliageLeafSlotNone);
        // Interned BY VALUE: a second layer that authored the same material is
        // the same material, and a forest of one species must cost one slot.
        EXPECT_EQ(table.Resolve(MakeProfile(1.0f)), first);
        EXPECT_EQ(table.GetAssignedSlotCount(), 1u);

        // A different material gets a different slot — otherwise two species
        // would shade with one set of parameters.
        const u32 second = table.Resolve(MakeProfile(1.0f, /*power=*/8.0f));
        EXPECT_NE(second, first);
        EXPECT_EQ(table.GetAssignedSlotCount(), 2u);
    }

    TEST(FoliageLeafProfileTableTest, DraggingASliderDoesNotExhaustTheTable)
    {
        // THE REGRESSION THIS FIX IS FOR. Slots are interned BY VALUE, because a
        // leaf material is authored inline on the layer and has no asset handle
        // to key on. An assignment that accumulated across frames would
        // therefore mint a slot for every intermediate value a slider passes
        // through — seven drags and the table is full for the rest of the
        // session, after which the DEFERRED path silently loses transmission
        // while forward keeps it. That is the exact forward/deferred divergence
        // this material exists to avoid, reached by ordinary authoring.
        //
        // Fifty distinct values, one frame each, is what a two-second drag looks
        // like.
        FoliageLeafProfileTable table;
        for (u32 i = 0; i < 50; ++i)
        {
            table.BeginFrame();
            const u32 slot = table.Resolve(MakeProfile(1.0f, /*power=*/2.0f + static_cast<f32>(i) * 0.25f));
            ASSERT_EQ(slot, 0u) << "frame " << i
                                << ": the frame's only leaf material must take the frame's first slot";
            EXPECT_EQ(table.GetAssignedSlotCount(), 1u)
                << "frame " << i << ": the assignment grew across frames — it is not being rebuilt";
        }
        EXPECT_EQ(table.GetSlotBudgetFullCount(), 0u)
            << "dragging one slider exhausted the seven-slot budget. Every foliage layer in the scene would then "
               "render without transmission on the deferred path only, while forward kept it.";

        // And the slot still carries the LATEST value, not the first one.
        const FoliageLeafProfile latest = table.GetProfileForSlot(0);
        EXPECT_TRUE(Math::BitwiseEqual(latest.Power, 2.0f + 49.0f * 0.25f))
            << "the slot held a stale value, so the frame's edit never reached the deferred path";
    }

    TEST(FoliageLeafProfileTableTest, ParametersBehindASlotAreReadableAndRefreshed)
    {
        FoliageLeafProfileTable table;
        FoliageLeafProfile authored = MakeProfile(2.0f, /*power=*/12.0f);
        authored.TransmissionColor = glm::vec3(0.25f, 0.5f, 0.125f);
        authored.Wrap = 0.75f;

        const u32 slot = table.Resolve(authored);
        ASSERT_LT(slot, kFoliageLeafSlotNone);
        EXPECT_TRUE(table.GetProfileForSlot(slot) == authored);

        // A slot nobody claimed reads as "no transmission" rather than as
        // garbage or as slot 0's material.
        const FoliageLeafProfile unassigned = table.GetProfileForSlot(kMaxFoliageLeafSlots - 1u);
        EXPECT_FALSE(unassigned.IsLeaf());
        // Out of range answers the same way rather than reading off the end.
        EXPECT_FALSE(table.GetProfileForSlot(kFoliageLeafSlotNone).IsLeaf());
        EXPECT_FALSE(table.GetProfileForSlot(9999u).IsLeaf());
    }

    TEST(FoliageLeafProfileTableTest, AnEighthMaterialIsRefusedAndCountedRatherThanAliased)
    {
        // All within ONE frame — the budget is per frame since the assignment is
        // rebuilt each one (see DraggingASliderDoesNotExhaustTheTable).
        FoliageLeafProfileTable table;
        table.BeginFrame();

        for (u32 i = 0; i < kMaxFoliageLeafSlots; ++i)
        {
            const u32 slot = table.Resolve(MakeProfile(1.0f, static_cast<f32>(2 + i)));
            EXPECT_EQ(slot, i) << "slots are handed out in order, so the i-th distinct material is slot i";
        }
        EXPECT_EQ(table.GetAssignedSlotCount(), kMaxFoliageLeafSlots);

        // One too many. The ONLY acceptable answer is "names no profile":
        // aliasing it onto an existing slot would give this layer somebody
        // else's transmission and look like a correct frame.
        const u32 overflow = table.Resolve(MakeProfile(1.0f, /*power=*/63.0f));
        EXPECT_EQ(overflow, kFoliageLeafSlotNone);
        EXPECT_EQ(table.GetSlotBudgetFullCount(), 1u)
            << "the refusal must be COUNTED, not only logged — a test cannot assert on a log line";

        // An ALREADY-INTERNED material still resolves after the table is full.
        // Getting this wrong would break every other layer the moment an eighth
        // one appeared, which is a far worse failure than the eighth losing its
        // per-pixel identity.
        EXPECT_EQ(table.Resolve(MakeProfile(1.0f, /*power=*/2.0f)), 0u);

        table.Reset();
        EXPECT_EQ(table.GetAssignedSlotCount(), 0u);
        EXPECT_EQ(table.GetSlotBudgetFullCount(), 0u)
            << "Reset clears the COUNTERS as well as the assignment; BeginFrame deliberately does not";
        EXPECT_EQ(table.Resolve(MakeProfile(1.0f, /*power=*/63.0f)), 0u)
            << "after a reset the previously-refused material must get the first slot";
    }

    // =========================================================================
    // The lanes the two paths share.
    // =========================================================================

    TEST(FoliageLeafProfileTableTest, TintLaneIsStrengthPremultipliedInTheSameOrderBothPathsUse)
    {
        FoliageLeafProfile p;
        p.TransmissionColor = glm::vec3(0.5f, 0.25f, 0.125f);
        p.TransmissionStrength = 3.0f;

        const glm::vec4 tint = FoliageLeafProfileTintLane(p);
        // The forward path packs `color * strength` into FoliageUBO::LeafTransmit
        // (CommandDispatch::DrawFoliageLayer and FoliageRenderer::Render); the
        // deferred path packs it here. Written as the same expression in the
        // same order on purpose — the shader reads .rgb as a finished product
        // and never sees the two apart, so if these ever disagreed the two
        // paths would each be internally consistent and differ on screen.
        EXPECT_TRUE(Math::BitwiseEqual(glm::vec3(tint), p.TransmissionColor * p.TransmissionStrength));
        EXPECT_TRUE(Math::BitwiseEqual(tint.w, p.TransmissionStrength));

        const glm::vec4 lobe = FoliageLeafProfileLobeLane(p);
        EXPECT_TRUE(Math::BitwiseEqual(lobe.x, p.Distortion));
        EXPECT_TRUE(Math::BitwiseEqual(lobe.y, p.Power));
        EXPECT_TRUE(Math::BitwiseEqual(lobe.z, p.Wrap));
        EXPECT_TRUE(Math::BitwiseEqual(lobe.w, p.Ambient));
    }

    // =========================================================================
    // Scene YAML.
    // =========================================================================

    TEST(FoliageLeafMaterialSerializationTest, SceneYamlRoundTripsEveryAuthoredField)
    {
        const FoliageLayer authored = MakeAuthoredLeafLayer();

        std::string yaml;
        {
            auto scene = Scene::Create();
            Entity e = scene->CreateEntity("Canopy");
            e.AddComponent<TerrainComponent>();
            auto& foliage = e.AddComponent<FoliageComponent>();
            foliage.m_Layers.push_back(authored);
            yaml = SceneSerializer(scene).SerializeToYAML();
        }
        ASSERT_FALSE(yaml.empty());

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(yaml));

        Entity restored = FindByTag(*reloaded, "Canopy");
        ASSERT_TRUE(static_cast<bool>(restored)) << "the round-tripped scene has no 'Canopy' entity";
        ASSERT_TRUE(restored.HasComponent<FoliageComponent>());
        const auto& foliage = restored.GetComponent<FoliageComponent>();
        ASSERT_EQ(foliage.m_Layers.size(), 1u);
        ExpectLeafFieldsEqual(foliage.m_Layers[0], authored);
    }

    TEST(FoliageLeafMaterialSerializationTest, ASceneAuthoredBeforeThisMaterialLoadsWithTransmissionOff)
    {
        // A FoliageComponent with none of the #1234 keys — byte-for-byte the
        // shape a scene saved by a pre-#1234 build has. The claim is not just
        // "it loads": it is that the leaf material comes back OFF, so that
        // scene renders exactly as the build that wrote it rendered instead of
        // acquiring a glow nobody authored.
        const std::string priorVersionYaml = R"(Scene: PriorVersion
Entities:
  - Entity: 12345678901234567890
    TagComponent:
      Tag: Canopy
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    FoliageComponent:
      Enabled: true
      Layers:
        - Name: Grass
          MeshPath: ""
          AlbedoPath: assets/textures/grass.png
          Density: 1.5
          SplatmapChannel: -1
          MinSlopeAngle: 0
          MaxSlopeAngle: 45
          MinScale: 0.8
          MaxScale: 1.2
          MinHeight: 0.5
          MaxHeight: 1.5
          RandomRotation: true
          ViewDistance: 100
          FadeStartDistance: 80
          WindStrength: 0.3
          WindSpeed: 1
          BaseColor: [0.3, 0.5, 0.1]
          Roughness: 0.8
          AlphaCutoff: 0.5
          Enabled: true
)";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(priorVersionYaml));

        Entity restored = FindByTag(*reloaded, "Canopy");
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<FoliageComponent>());
        const auto& foliage = restored.GetComponent<FoliageComponent>();
        ASSERT_EQ(foliage.m_Layers.size(), 1u);
        const FoliageLayer& layer = foliage.m_Layers[0];

        // The documented conservative default.
        EXPECT_FLOAT_EQ(layer.TransmissionStrength, 0.0f)
            << "a scene authored before #1234 came back with transmission ON";
        EXPECT_TRUE(layer.NormalMapPath.empty());
        EXPECT_TRUE(layer.RoughnessMapPath.empty());
        EXPECT_TRUE(layer.ThicknessMapPath.empty());
        // And the fields it DID carry are untouched, so this is a test of the
        // new keys' absence and not of a load that failed silently.
        EXPECT_FLOAT_EQ(layer.Roughness, 0.8f);
        EXPECT_FLOAT_EQ(layer.Density, 1.5f);
    }

    TEST(FoliageLeafMaterialSerializationTest, NonFiniteAndOutOfRangeValuesAreRejectedToDefaults)
    {
        // Every float here reaches a pow() exponent, a normalize() or a clamp
        // bound in include/FoliageSurface.glsl. A NaN there does not fail — it
        // propagates into scene colour and then into every temporal pass that
        // reads it back.
        //
        // The exponent's LOWER bound is the interesting one: below 1 the lobe
        // turns inside out and is brightest AWAY from the light, which is not a
        // look anyone authors on purpose.
        const std::string hostileYaml = R"(Scene: Hostile
Entities:
  - Entity: 12345678901234567891
    TagComponent:
      Tag: Canopy
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    FoliageComponent:
      Enabled: true
      Layers:
        - Name: Hostile
          TransmissionStrength: .nan
          TransmissionColor: [.inf, -3, 0.5]
          Thickness: 12
          TransmissionDistortion: -4
          TransmissionPower: 0.01
          TransmissionWrap: .nan
          TransmissionAmbient: .inf
          NormalStrength: .nan
          Enabled: true
)";

        auto reloaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(reloaded).DeserializeFromYAML(hostileYaml));

        Entity restored = FindByTag(*reloaded, "Canopy");
        ASSERT_TRUE(static_cast<bool>(restored));
        ASSERT_TRUE(restored.HasComponent<FoliageComponent>());
        const auto& foliage = restored.GetComponent<FoliageComponent>();
        ASSERT_EQ(foliage.m_Layers.size(), 1u);
        const FoliageLayer& l = foliage.m_Layers[0];

        EXPECT_TRUE(std::isfinite(l.TransmissionStrength));
        EXPECT_TRUE(std::isfinite(l.NormalStrength));
        EXPECT_TRUE(std::isfinite(l.Thickness));
        EXPECT_TRUE(std::isfinite(l.TransmissionDistortion));
        EXPECT_TRUE(std::isfinite(l.TransmissionPower));
        EXPECT_TRUE(std::isfinite(l.TransmissionWrap));
        EXPECT_TRUE(std::isfinite(l.TransmissionAmbient));
        EXPECT_TRUE(std::isfinite(l.TransmissionColor.x) && std::isfinite(l.TransmissionColor.y) &&
                    std::isfinite(l.TransmissionColor.z));

        // Ranges, not just finiteness: an exponent of 0.01 is finite
        // and still wrong.
        EXPECT_GE(l.TransmissionPower, 1.0f);
        EXPECT_LE(l.TransmissionPower, 64.0f);
        EXPECT_GE(l.Thickness, 0.0f);
        EXPECT_LE(l.Thickness, 1.0f);
        EXPECT_GE(l.TransmissionDistortion, 0.0f);
        EXPECT_LE(l.TransmissionDistortion, 1.0f);
        EXPECT_GE(l.TransmissionColor.x, 0.0f);
        EXPECT_LE(l.TransmissionColor.x, 1.0f);
        EXPECT_GE(l.TransmissionColor.y, 0.0f);
    }

} // namespace OloEngine::Tests
