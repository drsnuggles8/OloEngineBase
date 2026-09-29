// OLO_TEST_LAYER: unit
// =============================================================================
// SceneVersionTest.cpp
//
// Pins the scene-YAML version contract (issues #454, #1496): a scene carries a
// required top-level "Version" key, and this build reads exactly
// SceneSerializer::CurrentVersion. A scene with no Version key, a non-integer
// one, or any other number is rejected on both deserialize entry points (file
// and string) instead of being guessed at
// (docs/agent-rules/binary-format-versioning.md).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <filesystem>
#include <fstream>
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

        // A minimal scene document; `versionLine` is inserted verbatim after the
        // Scene key (where the writer puts Version), or omitted when empty.
        std::string MakeSceneYaml(const std::string& versionLine)
        {
            std::string yaml = "Scene: VersionedScene\n";
            if (!versionLine.empty())
                yaml += versionLine + "\n";
            yaml += "Entities:\n"
                    "  - Entity: 12345\n"
                    "    TagComponent:\n"
                    "      Tag: VersionedEntity\n"
                    "    TransformComponent:\n"
                    "      Translation: [1.0, 2.0, 3.0]\n"
                    "      Rotation: [0.0, 0.0, 0.0]\n"
                    "      Scale: [1.0, 1.0, 1.0]\n";
            return yaml;
        }

        std::string CurrentVersionLine()
        {
            return "Version: " + std::to_string(SceneSerializer::CurrentVersion);
        }

        bool DeserializeViaFile(const std::string& yaml, Ref<Scene>& scene)
        {
            const auto dir = std::filesystem::temp_directory_path() / "OloSceneVersionTest";
            std::filesystem::create_directories(dir);
            const auto path = dir / "scene.olo";
            std::filesystem::remove(path.string() + ".scenebin");
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out << yaml;
            }
            SceneSerializer serializer(scene);
            const bool ok = serializer.Deserialize(path);
            std::filesystem::remove_all(dir);
            return ok;
        }
    } // namespace

    TEST(SceneVersion, SerializedSceneCarriesCurrentVersionKey)
    {
        auto scene = Scene::Create();
        scene->CreateEntity("VersionedEntity");

        SceneSerializer serializer(scene);
        const std::string yaml = serializer.SerializeToYAML();

        EXPECT_NE(yaml.find(CurrentVersionLine()), std::string::npos)
            << "Expected serialized scene to carry '" << CurrentVersionLine() << "'\nYAML:\n"
            << yaml;
    }

    TEST(SceneVersion, CurrentVersionLoadsFromString)
    {
        auto scene = Scene::Create();
        SceneSerializer serializer(scene);

        ASSERT_TRUE(serializer.DeserializeFromYAML(MakeSceneYaml(CurrentVersionLine())));

        Entity entity = FindByTag(*scene, "VersionedEntity");
        ASSERT_TRUE(entity);
        const auto& transform = entity.GetComponent<TransformComponent>();
        EXPECT_FLOAT_EQ(transform.Translation.x, 1.0f);
        EXPECT_FLOAT_EQ(transform.Translation.y, 2.0f);
        EXPECT_FLOAT_EQ(transform.Translation.z, 3.0f);
    }

    TEST(SceneVersion, CurrentVersionLoadsFromFile)
    {
        auto scene = Scene::Create();
        ASSERT_TRUE(DeserializeViaFile(MakeSceneYaml(CurrentVersionLine()), scene));
        EXPECT_TRUE(FindByTag(*scene, "VersionedEntity"));
    }

    TEST(SceneVersion, WrongOrMissingVersionIsRejectedFromString)
    {
        const std::string bad[] = {
            "",                                                                // no Version key
            "Version: " + std::to_string(SceneSerializer::CurrentVersion + 1), // newer
            "Version: 0",                                                      // older
            "Version: notanumber",                                             // not an integer
            "Version: [1]",                                                    // not a scalar
        };
        for (const auto& line : bad)
        {
            auto scene = Scene::Create();
            SceneSerializer serializer(scene);
            EXPECT_FALSE(serializer.DeserializeFromYAML(MakeSceneYaml(line))) << "accepted: '" << line << "'";
            EXPECT_FALSE(FindByTag(*scene, "VersionedEntity")) << "rejected scene still created entities: '" << line << "'";
        }
    }

    TEST(SceneVersion, WrongOrMissingVersionIsRejectedFromFile)
    {
        const std::string bad[] = {
            "",
            "Version: " + std::to_string(SceneSerializer::CurrentVersion + 1),
        };
        for (const auto& line : bad)
        {
            auto scene = Scene::Create();
            EXPECT_FALSE(DeserializeViaFile(MakeSceneYaml(line), scene)) << "accepted: '" << line << "'";
            EXPECT_FALSE(FindByTag(*scene, "VersionedEntity")) << "rejected scene still created entities: '" << line << "'";
        }
    }
} // namespace OloEngine::Tests
