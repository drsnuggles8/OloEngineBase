// OLO_TEST_LAYER: unit
// =============================================================================
// SceneBinarySidecarTest.cpp
//
// Covers the binary scene sidecar cache (issue #525): SceneSerializer writes a
// `<scene>.scenebin` next to a scene's `.olo` on first load and restores from it
// on subsequent loads, skipping yaml-cpp. These tests pin the two properties
// that make the cache safe:
//
//   1. Fidelity — a scene loaded from the binary sidecar is field-for-field
//      identical to the same scene loaded from YAML (entities *and* scene-level
//      settings). Silent divergence here is the cardinal sin of a binary cache.
//   2. Fallback — a missing / stale / version-mismatched / corrupt sidecar is
//      ignored, and the load transparently falls back to YAML with the correct
//      result. The cache is never allowed to produce a *wrong* scene.
//
// Not a Functional test: no Scene::OnUpdateRuntime ticks — this is serializer
// symmetry, same as ComponentRoundTripTest.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/SceneBinaryFormat.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kEpsilon = 1e-4f;

        std::filesystem::path TestDir()
        {
#ifdef _WIN32
            const auto pid = static_cast<long long>(_getpid());
#else
            const auto pid = static_cast<long long>(::getpid());
#endif
            return std::filesystem::temp_directory_path() / ("olo_scenebin_test_" + std::to_string(pid));
        }

        // A recognisable per-entity expectation captured before serialize.
        struct EntityExpectation
        {
            std::string Tag;
            glm::vec3 Translation;
            glm::vec3 RotationEuler;
            glm::vec3 Scale;
        };

        // Builds a transform-only scene with `count` entities whose fields are all
        // distinct (so a swapped/dropped field is detectable), records the
        // expected values keyed by UUID, and sets a couple of non-default
        // scene-level post-process settings to exercise the settings snapshot.
        Ref<Scene> MakeTransformScene(u32 count, std::unordered_map<u64, EntityExpectation>& outExpected)
        {
            auto scene = Scene::Create();

            auto& pp = scene->GetPostProcessSettings();
            pp.Exposure = 2.5f;
            pp.Gamma = 1.8f;
            pp.BloomEnabled = true;

            for (u32 i = 0; i < count; ++i)
            {
                const std::string tag = "Ent_" + std::to_string(i);
                Entity e = scene->CreateEntity(tag);

                auto& tc = e.GetComponent<TransformComponent>();
                const f32 f = static_cast<f32>(i);
                tc.Translation = { f * 1.5f, -f * 2.25f, f + 0.125f };
                tc.SetRotationEuler({ 0.1f * f, -0.2f * f, 0.3f + 0.05f * f });
                tc.Scale = { 1.0f + f, 2.0f + f, 0.5f + 0.25f * f };

                outExpected[static_cast<u64>(e.GetUUID())] =
                    EntityExpectation{ tag, tc.Translation, tc.GetRotationEuler(), tc.Scale };
            }
            return scene;
        }

        // Verifies every entity of `scene` matches the recorded expectations.
        void ExpectSceneMatches(Scene& scene, const std::unordered_map<u64, EntityExpectation>& expected)
        {
            u32 seen = 0;
            for (auto handle : scene.GetAllEntitiesWith<IDComponent>())
            {
                Entity e{ handle, &scene };
                const u64 uuid = static_cast<u64>(e.GetComponent<IDComponent>().ID);
                auto it = expected.find(uuid);
                ASSERT_NE(it, expected.end()) << "Unexpected entity uuid " << uuid;
                const auto& exp = it->second;

                EXPECT_EQ(e.GetComponent<TagComponent>().Tag, exp.Tag);

                const auto& tc = e.GetComponent<TransformComponent>();
                EXPECT_NEAR(tc.Translation.x, exp.Translation.x, kEpsilon);
                EXPECT_NEAR(tc.Translation.y, exp.Translation.y, kEpsilon);
                EXPECT_NEAR(tc.Translation.z, exp.Translation.z, kEpsilon);
                const glm::vec3 euler = tc.GetRotationEuler();
                EXPECT_NEAR(euler.x, exp.RotationEuler.x, kEpsilon);
                EXPECT_NEAR(euler.y, exp.RotationEuler.y, kEpsilon);
                EXPECT_NEAR(euler.z, exp.RotationEuler.z, kEpsilon);
                EXPECT_NEAR(tc.Scale.x, exp.Scale.x, kEpsilon);
                EXPECT_NEAR(tc.Scale.y, exp.Scale.y, kEpsilon);
                EXPECT_NEAR(tc.Scale.z, exp.Scale.z, kEpsilon);
                ++seen;
            }
            EXPECT_EQ(seen, expected.size());
        }

        class SceneBinarySidecarTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                std::filesystem::create_directories(TestDir());
            }
            static void TearDownTestSuite()
            {
                std::error_code ec;
                std::filesystem::remove_all(TestDir(), ec);
            }

            std::filesystem::path ScenePath(const std::string& name) const
            {
                return TestDir() / name;
            }
        };
    } // namespace

    // The sidecar is written on first load and, on the second load, reproduces a
    // scene field-for-field identical to the YAML load — entities and settings.
    TEST_F(SceneBinarySidecarTest, SidecarRoundTripMatchesYaml)
    {
        const auto path = ScenePath("roundtrip.olo");
        const auto sidecar = std::filesystem::path(path).concat(".scenebin");

        std::unordered_map<u64, EntityExpectation> expected;
        {
            auto scene = MakeTransformScene(5, expected);
            SceneSerializer(scene).Serialize(path);
        }
        ASSERT_TRUE(std::filesystem::exists(path));
        ASSERT_FALSE(std::filesystem::exists(sidecar)) << "Serialize must not write a sidecar itself.";

        // First load: YAML path, which writes the sidecar.
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
            ExpectSceneMatches(*scene, expected);
        }
        ASSERT_TRUE(std::filesystem::exists(sidecar)) << "First load should have written a sidecar.";

        // Second load: fast path from the sidecar. Must match identically, and
        // the non-default post-process settings must survive the settings blob.
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
            ExpectSceneMatches(*scene, expected);
            EXPECT_NEAR(scene->GetPostProcessSettings().Exposure, 2.5f, kEpsilon);
            EXPECT_NEAR(scene->GetPostProcessSettings().Gamma, 1.8f, kEpsilon);
            EXPECT_TRUE(scene->GetPostProcessSettings().BloomEnabled);
        }
    }

    // Proves the fast path actually reads the binary sidecar and not the YAML:
    // after the sidecar exists we corrupt the `.olo` in place (same byte size,
    // same modification time) so a YAML load would fail — yet the scene still
    // loads correctly, which can only happen through the sidecar.
    TEST_F(SceneBinarySidecarTest, FastPathReadsBinaryNotYaml)
    {
        const auto path = ScenePath("fastpath.olo");

        std::unordered_map<u64, EntityExpectation> expected;
        {
            auto scene = MakeTransformScene(4, expected);
            SceneSerializer(scene).Serialize(path);
        }
        const auto originalMtime = std::filesystem::last_write_time(path);
        const auto originalSize = std::filesystem::file_size(path);

        // Prime the sidecar via a normal load.
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
        }

        // Overwrite the `.olo` with garbage of the exact same length, then restore
        // its modification time so the staleness key still matches the sidecar.
        {
            std::string garbage(static_cast<sizet>(originalSize), '@');
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
        }
        std::filesystem::last_write_time(path, originalMtime);
        ASSERT_EQ(std::filesystem::file_size(path), originalSize);

        // A YAML load of "@@@..." would fail; success proves the binary path ran.
        auto scene = Scene::Create();
        ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
        ExpectSceneMatches(*scene, expected);
    }

    // Changing the source invalidates the sidecar: the next load falls back to
    // YAML and reflects the new source content.
    TEST_F(SceneBinarySidecarTest, SourceChangeInvalidatesSidecar)
    {
        const auto path = ScenePath("stale.olo");

        std::unordered_map<u64, EntityExpectation> firstExpected;
        {
            auto scene = MakeTransformScene(3, firstExpected);
            SceneSerializer(scene).Serialize(path);
        }
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(path)); // writes sidecar
        }

        // Re-serialize a different (larger) scene over the same path — size and
        // mtime both change, so the sidecar is stale.
        std::unordered_map<u64, EntityExpectation> secondExpected;
        {
            auto scene = MakeTransformScene(6, secondExpected);
            SceneSerializer(scene).Serialize(path);
        }

        auto scene = Scene::Create();
        ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
        ExpectSceneMatches(*scene, secondExpected); // NOT the stale first scene
    }

    // A corrupt sidecar (bad magic) is ignored and the load falls back to YAML.
    TEST_F(SceneBinarySidecarTest, CorruptSidecarFallsBackToYaml)
    {
        const auto path = ScenePath("corrupt.olo");
        const auto sidecar = std::filesystem::path(path).concat(".scenebin");

        std::unordered_map<u64, EntityExpectation> expected;
        {
            auto scene = MakeTransformScene(3, expected);
            SceneSerializer(scene).Serialize(path);
        }

        // Hand-write a garbage sidecar (wrong magic, junk body).
        {
            std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
            const std::string junk = "not a real scenebin file, just noise noise noise";
            out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        }

        auto scene = Scene::Create();
        ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
        ExpectSceneMatches(*scene, expected);
    }

    // A sidecar whose format version is newer than this build understands is
    // rejected (its layout is unknowable) and the load falls back to YAML.
    TEST_F(SceneBinarySidecarTest, FutureVersionSidecarFallsBackToYaml)
    {
        const auto path = ScenePath("future.olo");
        const auto sidecar = std::filesystem::path(path).concat(".scenebin");

        std::unordered_map<u64, EntityExpectation> expected;
        {
            auto scene = MakeTransformScene(3, expected);
            SceneSerializer(scene).Serialize(path);
        }

        // Valid magic + matching staleness key, but a version beyond CurrentVersion.
        {
            OSceneFormat::FileHeader header;
            header.Version = OSceneFormat::CurrentVersion + 1;
            header.SceneSchemaVersion = SceneSerializer::CurrentVersion;
            header.SourceFileSize = std::filesystem::file_size(path);
            header.SourceTimestamp =
                static_cast<u64>(std::filesystem::last_write_time(path).time_since_epoch().count());
            std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        }

        auto scene = Scene::Create();
        ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
        ExpectSceneMatches(*scene, expected);
    }

    // Manual benchmark (DISABLED — run explicitly with
    // --gtest_also_run_disabled_tests --gtest_filter=*Benchmark*). Not a CI test;
    // it exists to reproduce the #525 YAML-vs-binary scene-load numbers on demand.
    TEST_F(SceneBinarySidecarTest, DISABLED_LargeSceneLoadBenchmark)
    {
        constexpr u32 kEntities = 100'000;
        const auto path = ScenePath("bench_100k.olo");

        {
            std::unordered_map<u64, EntityExpectation> expected;
            auto scene = MakeTransformScene(kEntities, expected);
            SceneSerializer(scene).Serialize(path);
        }

        auto timeLoad = [&](const char* label)
        {
            auto scene = Scene::Create();
            const auto start = std::chrono::steady_clock::now();
            const bool ok = SceneSerializer(scene).Deserialize(path);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
            EXPECT_TRUE(ok);
            std::printf("  [bench] %-12s %5lld ms  (%u entities)\n", label, static_cast<long long>(ms), kEntities);
            return ms;
        };

        // Break down the YAML cost: pure tokenize (YAML::LoadFile → node tree)
        // vs. the full deserialize (tokenize + node-walk + component construct).
        {
            const auto start = std::chrono::steady_clock::now();
            YAML::Node node = YAML::LoadFile(path.string());
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
            std::printf("  [bench] %-12s %5lld ms  (tokenize only, %zu top-level keys)\n",
                        "YAML::Load", static_cast<long long>(ms), node.size());
        }

        const auto yamlMs = timeLoad("YAML");  // first load parses YAML + writes sidecar
        const auto binMs = timeLoad("binary"); // second load hits the sidecar
        std::printf("  [bench] speedup: %.1fx\n", binMs > 0 ? static_cast<double>(yamlMs) / static_cast<double>(binMs) : 0.0);
        EXPECT_LT(binMs, yamlMs) << "Binary sidecar load should be faster than YAML.";
    }

    // A binary-COVERED non-transform component (CircleRenderer: a vec4 + two
    // floats, in the OloHeaderTool-generated set) round-trips through the fast
    // binary path — proves the codegen'd component blocks work end-to-end.
    TEST_F(SceneBinarySidecarTest, CoveredComponentRoundTripsViaBinary)
    {
        const auto path = ScenePath("covered.olo");
        const glm::vec4 expectedColor{ 0.1f, 0.2f, 0.3f, 0.4f };
        u64 uuid = 0;
        {
            auto scene = Scene::Create();
            Entity e = scene->CreateEntity("Circle");
            auto& cr = e.AddComponent<CircleRendererComponent>();
            cr.Color = expectedColor;
            cr.Thickness = 0.55f;
            cr.Fade = 0.021f;
            uuid = static_cast<u64>(e.GetUUID());
            SceneSerializer(scene).Serialize(path);
        }

        // First load writes the sidecar; second load reads it (fast binary path).
        for (int pass = 0; pass < 2; ++pass)
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(path)) << "pass " << pass;
            Entity found;
            for (auto h : scene->GetAllEntitiesWith<CircleRendererComponent>())
                found = Entity{ h, scene.get() };
            ASSERT_TRUE(static_cast<bool>(found)) << "pass " << pass;
            EXPECT_EQ(static_cast<u64>(found.GetComponent<IDComponent>().ID), uuid);
            const auto& cr = found.GetComponent<CircleRendererComponent>();
            EXPECT_NEAR(cr.Color.r, expectedColor.r, kEpsilon);
            EXPECT_NEAR(cr.Color.a, expectedColor.a, kEpsilon);
            EXPECT_NEAR(cr.Thickness, 0.55f, kEpsilon);
            EXPECT_NEAR(cr.Fade, 0.021f, kEpsilon);
        }
    }

    // Strongest fidelity guarantee: a scene loaded through the binary fast path
    // must re-serialize to EXACTLY the same YAML as one loaded through the YAML
    // path. If any covered component's binary read/write dropped or mangled a
    // field, the re-emitted YAML would diverge. Exercises vec3/vec4/float and
    // std::vector<std::string> covered-component field types in one shot.
    TEST_F(SceneBinarySidecarTest, BinaryLoadReserializesIdenticallyToYamlLoad)
    {
        const auto path = ScenePath("fidelity.olo");
        {
            auto scene = Scene::Create();
            Entity e = scene->CreateEntity("Rich");
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = { 3.5f, -1.25f, 8.0f };
            tc.SetRotationEuler({ 0.3f, -0.7f, 1.4f });
            tc.Scale = { 2.0f, 0.5f, 1.5f };

            auto& cr = e.AddComponent<CircleRendererComponent>();
            cr.Color = { 0.9f, 0.1f, 0.4f, 0.8f };
            cr.Thickness = 0.33f;
            cr.Fade = 0.007f;

            auto& qg = e.AddComponent<QuestGiverComponent>();
            qg.OfferedQuestIDs = { "quest_a", "quest_bb", "quest_ccc" };
            qg.TurnInQuestIDs = { "turnin_1" };

            SceneSerializer(scene).Serialize(path);
        }

        // Load via YAML (first load also writes the sidecar), re-serialize.
        std::string yamlPathResult;
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
            yamlPathResult = SceneSerializer(scene).SerializeToYAML();
        }
        // Load via the binary sidecar, re-serialize.
        std::string binaryPathResult;
        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));
            binaryPathResult = SceneSerializer(scene).SerializeToYAML();
        }

        ASSERT_FALSE(binaryPathResult.empty());
        EXPECT_EQ(binaryPathResult, yamlPathResult)
            << "Binary fast-path load diverged from the YAML load — a covered "
               "component's binary (de)serialization dropped or mangled a field.";
    }

    // The hybrid format covers ALL components: an entity with a component outside
    // the binary-covered set (a camera) is stored as YAML text inside the same
    // sidecar, alongside binary transform entities. Every entity must round-trip,
    // and the sidecar IS written (unlike the old whole-scene fallback).
    TEST_F(SceneBinarySidecarTest, MixedSceneRoundTripsBinaryAndYamlEntities)
    {
        const auto path = ScenePath("mixed.olo");
        const auto sidecar = std::filesystem::path(path).concat(".scenebin");

        std::unordered_map<u64, EntityExpectation> transforms;
        u64 camUuid = 0;
        {
            auto scene = MakeTransformScene(4, transforms); // 4 binary entities
            Entity cam = scene->CreateEntity("Camera");     // 1 yaml-kind entity
            auto& cc = cam.AddComponent<CameraComponent>();
            cc.Primary = true;
            cc.Camera.SetPerspectiveVerticalFOV(1.111f);
            camUuid = static_cast<u64>(cam.GetUUID());
            SceneSerializer(scene).Serialize(path);
        }

        {
            auto scene = Scene::Create();
            ASSERT_TRUE(SceneSerializer(scene).Deserialize(path)); // writes sidecar
        }
        ASSERT_TRUE(std::filesystem::exists(sidecar)) << "Hybrid format caches every scene.";

        // Second load takes the fast path and must reproduce both kinds exactly.
        auto scene = Scene::Create();
        ASSERT_TRUE(SceneSerializer(scene).Deserialize(path));

        // The 4 transform entities.
        u32 transformSeen = 0;
        Entity camera;
        for (auto h : scene->GetAllEntitiesWith<IDComponent>())
        {
            Entity e{ h, scene.get() };
            const u64 id = static_cast<u64>(e.GetComponent<IDComponent>().ID);
            if (id == camUuid)
                camera = e;
            else if (auto it = transforms.find(id); it != transforms.end())
                ++transformSeen;
        }
        EXPECT_EQ(transformSeen, transforms.size());

        // The yaml-kind camera entity.
        ASSERT_TRUE(static_cast<bool>(camera));
        ASSERT_TRUE(camera.HasComponent<CameraComponent>());
        const auto& cc = camera.GetComponent<CameraComponent>();
        EXPECT_TRUE(cc.Primary);
        EXPECT_NEAR(cc.Camera.GetPerspectiveVerticalFOV(), 1.111f, kEpsilon);
    }
} // namespace OloEngine::Tests
