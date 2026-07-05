// OLO_TEST_LAYER: unit
// =============================================================================
// SceneVersionMigrationTest.cpp
//
// Locks in the scene-YAML versioning + forward-compat behaviour added for
// issue #454: scenes now carry a top-level "Version" key, and a scene file
// written before this scheme existed (no "Version" key at all) must still
// load cleanly rather than being rejected — that's the entire pre-#454
// scene library on disk.
//
// What this test does
// --------------------
//   1. Serializes a scene and asserts the emitted YAML carries
//      "Version: <SceneSerializer::CurrentVersion>" so future PRs bumping
//      the version don't have to touch this file.
//   2. Feeds a hand-written "v0" fixture — a minimal scene document with no
//      Version key, mirroring every scene saved before this feature existed
//      — through DeserializeFromYAML and asserts it loads successfully with
//      the expected entity data intact.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <string>

namespace OloEngine::Tests
{
    namespace
    {
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
    } // namespace

    TEST(SceneVersionMigration, SerializedSceneCarriesCurrentVersionKey)
    {
        auto scene = Scene::Create();
        scene->CreateEntity("VersionedEntity");

        SceneSerializer serializer(scene);
        const std::string yaml = serializer.SerializeToYAML();

        const std::string expectedKey =
            "Version: " + std::to_string(SceneSerializer::CurrentVersion);
        EXPECT_NE(yaml.find(expectedKey), std::string::npos)
            << "Expected serialized scene to carry '" << expectedKey << "'\nYAML:\n"
            << yaml;
    }

    TEST(SceneVersionMigration, V0SceneWithNoVersionKeyStillLoads)
    {
        // Mirrors the shape SceneSerializer::SerializeToYAML() produced
        // before #454 added the "Version" key — no Version field at all.
        constexpr const char* kV0Yaml = R"(
Scene: LegacyScene
Entities:
  - Entity: 12345
    TagComponent:
      Tag: LegacyEntity
    TransformComponent:
      Translation: [1.0, 2.0, 3.0]
      Rotation: [0.0, 0.0, 0.0]
      Scale: [1.0, 1.0, 1.0]
)";

        auto scene = Scene::Create();
        SceneSerializer serializer(scene);

        ASSERT_TRUE(serializer.DeserializeFromYAML(kV0Yaml));

        Entity entity = FindByTag(*scene, "LegacyEntity");
        ASSERT_TRUE(entity);

        const auto& transform = entity.GetComponent<TransformComponent>();
        EXPECT_FLOAT_EQ(transform.Translation.x, 1.0f);
        EXPECT_FLOAT_EQ(transform.Translation.y, 2.0f);
        EXPECT_FLOAT_EQ(transform.Translation.z, 3.0f);
    }
} // namespace OloEngine::Tests
