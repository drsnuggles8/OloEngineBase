#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// TilemapPersistenceRoundTripTest — Functional Test.
//
// Cross-subsystem seam under test:
//   TilemapComponent × SceneSerializer (YAML) × SaveGameSerializer (binary) ×
//   Scene::Copy. A new ECS component has to survive all three or it fails
//   silently in a different way at each seam (CLAUDE.md, *Definition of done*
//   item 3):
//     - scene YAML: the generated block is missing -> the map is empty on reload;
//     - save-game: the hand-written Serialize overload or its RegisterAll entry
//       is missing -> the map survives a scene load but vanishes from a
//       quicksave, with nothing logged;
//     - Scene::Copy: the component is missing from the AllComponents tuple ->
//       the authored map reverts to defaults the moment Play is pressed.
//
// The interesting field is `Layers`, a std::vector<TileLayer>. Issue #646 noted
// that the #451 std::vector<struct> scene-serialization codegen "may need to
// land first"; it has, so the scene YAML block is generated rather than
// hand-written. This test is what proves that claim end to end.
//
// Scenario: one entity with a two-layer 6x4 tilemap carrying distinctly
// non-default values in every field, pushed through each of the three paths.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <yaml-cpp/yaml.h>

#include <string>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr const char* kEntityName = "TileWorld";

    constexpr u32 kWidth = 6;
    constexpr u32 kHeight = 4;
    constexpr f32 kTileSize = 0.75f;
    constexpr f32 kFriction = 0.31f;
    constexpr f32 kRestitution = 0.17f;
    constexpr u64 kTilesetHandle = 0x0102030405060708ull;

    // Fills the component with values that differ from every constructor default,
    // so a dropped field shows up as "still the default" rather than passing by
    // coincidence.
    void Author(TilemapComponent& map)
    {
        map.TilesetHandle = AssetHandle(kTilesetHandle);
        map.Width = kWidth;
        map.Height = kHeight;
        map.TileSize = kTileSize;
        map.Color = glm::vec4(0.25f, 0.5f, 0.75f, 0.5f);
        map.GenerateColliders = true;
        map.ColliderFriction = kFriction;
        map.ColliderRestitution = kRestitution;

        const sizet ground = map.AddLayer("Ground");
        map.Layers[ground].Solid = true;
        map.Layers[ground].Opacity = 0.9f;
        map.Layers[ground].ZOffset = -0.01f;
        for (u32 x = 0; x < kWidth; ++x)
            map.SetTile(ground, x, 0, x + 1);

        const sizet decor = map.AddLayer("Decor");
        map.Layers[decor].Visible = false;
        map.Layers[decor].Opacity = 0.4f;
        map.Layers[decor].ZOffset = 0.02f;
        map.SetTile(decor, 3, 2, 9);
    }

    // The assertions shared by all three paths. `where` names the seam so a
    // failure message says which one broke.
    void ExpectAuthoredValues(const TilemapComponent& map, const char* where)
    {
        EXPECT_EQ(static_cast<u64>(map.TilesetHandle), kTilesetHandle) << where;
        EXPECT_EQ(map.Width, kWidth) << where;
        EXPECT_EQ(map.Height, kHeight) << where;
        EXPECT_NEAR(map.TileSize, kTileSize, 1e-5f) << where;
        EXPECT_NEAR(map.Color.b, 0.75f, 1e-5f) << where;
        EXPECT_NEAR(map.Color.a, 0.5f, 1e-5f) << where;
        EXPECT_TRUE(map.GenerateColliders) << where;
        EXPECT_NEAR(map.ColliderFriction, kFriction, 1e-5f) << where;
        EXPECT_NEAR(map.ColliderRestitution, kRestitution, 1e-5f) << where;

        ASSERT_EQ(map.Layers.size(), 2u) << where;

        EXPECT_EQ(map.Layers[0].Name, "Ground") << where;
        EXPECT_TRUE(map.Layers[0].Visible) << where;
        EXPECT_TRUE(map.Layers[0].Solid) << where;
        EXPECT_NEAR(map.Layers[0].Opacity, 0.9f, 1e-5f) << where;
        EXPECT_NEAR(map.Layers[0].ZOffset, -0.01f, 1e-5f) << where;
        for (u32 x = 0; x < kWidth; ++x)
            EXPECT_EQ(map.GetTile(0, x, 0), x + 1) << where << " tile x=" << x;
        EXPECT_EQ(map.GetTile(0, 0, 1), TilemapComponent::kEmptyTile) << where;

        EXPECT_EQ(map.Layers[1].Name, "Decor") << where;
        EXPECT_FALSE(map.Layers[1].Visible) << where;
        EXPECT_FALSE(map.Layers[1].Solid) << where;
        EXPECT_NEAR(map.Layers[1].Opacity, 0.4f, 1e-5f) << where;
        EXPECT_NEAR(map.Layers[1].ZOffset, 0.02f, 1e-5f) << where;
        EXPECT_EQ(map.GetTile(1, 3, 2), 9u) << where;
    }
} // namespace

class TilemapPersistenceRoundTripTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Entity = GetScene().CreateEntity(kEntityName);
        Author(m_Entity.AddComponent<TilemapComponent>());
    }

    Entity m_Entity;
};

TEST_F(TilemapPersistenceRoundTripTest, SurvivesSceneYAMLRoundTrip)
{
    SceneSerializer serializer(GetSceneRef());
    const std::string yaml = serializer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty());

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SceneSerializer(restored).DeserializeFromYAML(yaml))
        << "DeserializeFromYAML rejected a scene containing a TilemapComponent.";

    Entity entity = restored->FindEntityByName(kEntityName);
    ASSERT_TRUE(entity);
    ASSERT_TRUE(entity.HasComponent<TilemapComponent>())
        << "TilemapComponent dropped by the scene serializer — the generated "
           "block is missing or the component was excluded from codegen.";

    ExpectAuthoredValues(entity.GetComponent<TilemapComponent>(), "scene YAML");
}

TEST_F(TilemapPersistenceRoundTripTest, SurvivesSaveGameRoundTrip)
{
    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity entity = restored->FindEntityByName(kEntityName);
    ASSERT_TRUE(entity);
    ASSERT_TRUE(entity.HasComponent<TilemapComponent>())
        << "TilemapComponent dropped by the save-game path — the Serialize "
           "overload or its RegisterAll entry is missing. This failure is silent "
           "in the editor: the scene loads fine and only quicksaves lose the map.";

    ExpectAuthoredValues(entity.GetComponent<TilemapComponent>(), "save game");
}

TEST_F(TilemapPersistenceRoundTripTest, SurvivesSceneCopyAsPlayModeEntryDoes)
{
    // Scene::Copy runs on every Play entry. A component missing from the
    // generated AllComponents tuple reverts to its defaults right there, which
    // reads to the author as "pressing Play wiped my tilemap".
    // Scene::Copy takes a non-const lvalue reference, so the source has to be a
    // named local rather than GetSceneRef()'s returned temporary.
    Ref<Scene> source = GetSceneRef();
    Ref<Scene> copy = Scene::Copy(source);
    ASSERT_TRUE(copy);

    Entity entity = copy->FindEntityByName(kEntityName);
    ASSERT_TRUE(entity);
    ASSERT_TRUE(entity.HasComponent<TilemapComponent>());
    ExpectAuthoredValues(entity.GetComponent<TilemapComponent>(), "Scene::Copy");
}

TEST_F(TilemapPersistenceRoundTripTest, EveryFieldIsVisibleToUndoEquality)
{
    // The editor's undo tier for this component is `operator==`. A field missing
    // from it means the inspector records no change and Ctrl+Z cannot revert it.
    // Mutating each field in turn must make the copy compare unequal.
    const TilemapComponent original = m_Entity.GetComponent<TilemapComponent>();

    auto expectDiffers = [&original](auto mutate, const char* field)
    {
        TilemapComponent mutated = original;
        mutate(mutated);
        EXPECT_FALSE(mutated == original) << "operator== ignores " << field;
    };

    expectDiffers([](TilemapComponent& c)
                  { c.TilesetHandle = AssetHandle(99); }, "TilesetHandle");
    expectDiffers([](TilemapComponent& c)
                  { c.Width += 1; }, "Width");
    expectDiffers([](TilemapComponent& c)
                  { c.Height += 1; }, "Height");
    expectDiffers([](TilemapComponent& c)
                  { c.TileSize += 1.0f; }, "TileSize");
    expectDiffers([](TilemapComponent& c)
                  { c.Color.r += 0.1f; }, "Color");
    expectDiffers([](TilemapComponent& c)
                  { c.GenerateColliders = !c.GenerateColliders; }, "GenerateColliders");
    expectDiffers([](TilemapComponent& c)
                  { c.ColliderFriction += 0.1f; }, "ColliderFriction");
    expectDiffers([](TilemapComponent& c)
                  { c.ColliderRestitution += 0.1f; }, "ColliderRestitution");
    expectDiffers([](TilemapComponent& c)
                  { c.SetTile(0, 1, 1, 5); }, "Layers[].Tiles");
    expectDiffers([](TilemapComponent& c)
                  { c.Layers[0].Name = "Renamed"; }, "Layers[].Name");
    expectDiffers([](TilemapComponent& c)
                  { c.Layers[0].Visible = !c.Layers[0].Visible; }, "Layers[].Visible");
    expectDiffers([](TilemapComponent& c)
                  { c.Layers[0].Solid = !c.Layers[0].Solid; }, "Layers[].Solid");
    expectDiffers([](TilemapComponent& c)
                  { c.Layers[0].Opacity *= 0.5f; }, "Layers[].Opacity");
    expectDiffers([](TilemapComponent& c)
                  { c.Layers[0].ZOffset += 1.0f; }, "Layers[].ZOffset");
}

TEST_F(TilemapPersistenceRoundTripTest, ACorruptGridExtentFallsBackToTheDefault)
{
    // Width/Height carry OLO_SERIALIZE(Reject, ...) rather than Clamp: saturating
    // a corrupt width to 4096 would produce a valid-but-wrong grid that
    // reinterprets every row of the layer data. Rejecting leaves the constructor
    // default, which is visibly wrong instead of subtly wrong.
    SceneSerializer serializer(GetSceneRef());
    std::string yaml = serializer.SerializeToYAML();
    ASSERT_NE(yaml.find("Width: 6"), std::string::npos)
        << "The emitted key changed; this test's edit no longer targets the grid width.";
    yaml.replace(yaml.find("Width: 6"), std::string("Width: 6").size(), "Width: 999999");

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SceneSerializer(restored).DeserializeFromYAML(yaml));

    Entity entity = restored->FindEntityByName(kEntityName);
    ASSERT_TRUE(entity);
    ASSERT_TRUE(entity.HasComponent<TilemapComponent>());
    const auto& map = entity.GetComponent<TilemapComponent>();
    EXPECT_EQ(map.Width, TilemapComponent{}.Width)
        << "An out-of-range Width was accepted or clamped instead of rejected.";
    EXPECT_EQ(map.Height, kHeight) << "Rejecting Width must not disturb Height.";
}
