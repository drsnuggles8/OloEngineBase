// OLO_TEST_LAYER: L8
// =============================================================================
// DriftBoatDeliveryTest.cpp
//
// Drift's boat is generated: Assets/Models/DriftBoat/build_drift_boat.py writes
// the meshes, the maps and DriftBoat.rig.json, whose "physics" block is the rigid
// body the drawn hull implies. Three things then have to agree, and nothing but
// this file makes them:
//
//   * Drift.olo, which authors the body, the meshes and their levels of detail;
//   * DriftBoat.rig.json, which is where those numbers come from;
//   * DriftScenePresets.h, the copy of the body SailTest's Drift cases sail.
//
// The first case reads all three and compares them, with no renderer. A boat
// regenerated without re-stamping the scene, or a scene number tidied by hand,
// fails here instead of becoming a hull that floats at the wrong waterline.
//
// The second packs the scene with the boat's assets and loads the pack cold, as
// the shipped runtime does. The boat is drawn through MeshComponent with an
// authored LODGroupComponent whose levels are .olomesh assets, and Drift is the
// first scene to ship one: a level that names an asset the pack does not carry
// loads as nothing at all and the boat simply stops being drawn past 30 m.
// =============================================================================
#include "OloEnginePCH.h"

#include "DriftScenePresets.h"
#include "RendererAttachedTest.h"
#include "TestTempDir.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Build/GameBuildPipeline.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr f32 kEps = 1.0e-3f;

        [[nodiscard]] fs::path Sandbox()
        {
            return fs::path(OLO_TEST_EDITOR_ROOT) / "SandboxProject";
        }

        [[nodiscard]] nlohmann::json LoadRig()
        {
            std::ifstream in(Sandbox() / "Assets/Models/DriftBoat/DriftBoat.rig.json");
            EXPECT_TRUE(in.good()) << "DriftBoat.rig.json is missing";
            return nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
        }

        // The entity with this tag in the authored scene file, or a null node.
        [[nodiscard]] YAML::Node SceneEntity(const YAML::Node& scene, const std::string& tag)
        {
            for (const auto& entity : scene["Entities"])
                if (entity["TagComponent"] && entity["TagComponent"]["Tag"].as<std::string>("") == tag)
                    return entity;
            return {};
        }

        void ExpectVec3(const YAML::Node& node, const nlohmann::json& expected, const char* what)
        {
            ASSERT_TRUE(node && node.IsSequence() && node.size() == 3) << what;
            ASSERT_TRUE(expected.is_array() && expected.size() == 3) << what;
            for (sizet i = 0; i < 3; ++i)
                EXPECT_NEAR(node[i].as<f32>(), expected[i].get<f32>(), kEps) << what << "[" << i << "]";
        }

        void ExpectVec3(const YAML::Node& node, const glm::vec3& expected, const char* what)
        {
            ASSERT_TRUE(node && node.IsSequence() && node.size() == 3) << what;
            for (int i = 0; i < 3; ++i)
                EXPECT_NEAR(node[i].as<f32>(), expected[i], kEps) << what << "[" << i << "]";
        }

        // The drawn parts, by the scene's tag and the rig file's key.
        constexpr std::pair<const char*, const char*> kParts[] = {
            { "Boat Hull", "hull" },
            { "Boat Sail", "sail" },
            { "Boat Rudder", "rudder" },
        };
    } // namespace

    TEST(DriftBoatDelivery, TheSceneIsBuiltFromTheGeneratedRig)
    {
        const YAML::Node scene = YAML::LoadFile((Sandbox() / "Assets/Scenes/Drift.olo").string());
        const nlohmann::json rig = LoadRig();
        ASSERT_FALSE(rig.is_discarded()) << "DriftBoat.rig.json does not parse";
        const auto& physics = rig["physics"];

        // --- The rigid body: scene == rig == the presets the sailing tests use ---
        const YAML::Node boat = SceneEntity(scene, "Boat");
        ASSERT_TRUE(boat) << "Drift.olo has no Boat entity";
        EXPECT_NEAR(boat["TransformComponent"]["Translation"][1].as<f32>(), physics["originAboveWaterline"].get<f32>(), kEps)
            << "the boat is not authored at the height she floats at";
        EXPECT_NEAR(boat["Rigidbody3DComponent"]["Mass"].as<f32>(), physics["mass"].get<f32>(), kEps);
        EXPECT_NEAR(boat["Rigidbody3DComponent"]["Mass"].as<f32>(), DriftBoat::kMass, kEps);
        ExpectVec3(boat["BoxCollider3DComponent"]["HalfExtents"], physics["colliderHalfExtents"], "collider (rig)");
        ExpectVec3(boat["BoxCollider3DComponent"]["HalfExtents"], DriftBoat::kColliderHalfExtents, "collider (presets)");
        const glm::vec3 colliderOffset(0.0f, physics["colliderOffsetY"].get<f32>(), 0.0f);
        ExpectVec3(boat["BoxCollider3DComponent"]["Offset"], colliderOffset, "collider offset (rig)");
        ExpectVec3(boat["BoxCollider3DComponent"]["Offset"], DriftBoat::kColliderOffset, "collider offset (presets)");

        BuoyancyComponent buoyancy;
        ApplyDriftBuoyancy(buoyancy);
        const YAML::Node b = boat["BuoyancyComponent"];
        ExpectVec3(b["ProbeExtents"], physics["probeExtents"], "probe box (rig)");
        ExpectVec3(b["ProbeExtents"], buoyancy.m_ProbeExtents, "probe box (presets)");
        EXPECT_NEAR(b["LinearDrag"].as<f32>(), physics["buoyancyLinearDrag"].get<f32>(), kEps);
        EXPECT_NEAR(b["LinearDrag"].as<f32>(), buoyancy.m_LinearDrag, kEps);
        EXPECT_NEAR(b["AngularDrag"].as<f32>(), physics["buoyancyAngularDrag"].get<f32>(), kEps);
        EXPECT_NEAR(b["AngularDrag"].as<f32>(), buoyancy.m_AngularDrag, kEps);
        EXPECT_NEAR(b["SubmergenceRamp"].as<f32>(), physics["submergenceRamp"].get<f32>(), kEps);
        EXPECT_NEAR(b["SubmergenceRamp"].as<f32>(), buoyancy.m_SubmergenceRamp, kEps);
        EXPECT_NEAR(b["FluidDensity"].as<f32>(), rig["hull"]["waterDensity"].get<f32>(), kEps);
        EXPECT_NEAR(b["FluidDensity"].as<f32>(), buoyancy.m_FluidDensity, kEps);
        EXPECT_NEAR(b["BuoyancyScale"].as<f32>(), buoyancy.m_BuoyancyScale, kEps);

        // The derivation. A probe box carrying this mass floats with its bottom
        // corners `immersion` under the sea and its centre, the entity's origin,
        // `rise` above it; the mesh hangs that far down, and so does the collider,
        // whose middle is the middle of the hull and the body's centre of mass.
        // The sail's force acts a lever above that centre which is a multiple of
        // the half-beam, not the drawn sail's height. Roll stiffness over that
        // lever is the heel the game was tuned for (62.1 kN per radian on the hull
        // before this one), and weight x half-beam over it is the side force that
        // knocks her down (17 kN on that hull).
        {
            const f32 mass = physics["mass"].get<f32>();
            const f32 ramp = physics["submergenceRamp"].get<f32>();
            const f32 density = rig["hull"]["waterDensity"].get<f32>();
            const f32 x = physics["probeExtents"][0].get<f32>();
            const f32 y = physics["probeExtents"][1].get<f32>();
            const f32 z = physics["probeExtents"][2].get<f32>();
            const f32 immersion = mass * ramp / (4.0f * density * x * y * z);
            const f32 rise = y - immersion;
            EXPECT_NEAR(x, 0.5f * rig["hull"]["beam"].get<f32>(), kEps) << "the probe box is not the hull's beam";
            EXPECT_NEAR(z, 0.5f * rig["hull"]["lengthWaterline"].get<f32>(), kEps) << "nor her waterline length";
            EXPECT_NEAR(physics["hullMeshOffsetY"].get<f32>(), -rise, 2.0e-3f)
                << "the hull mesh no longer hangs where the box floats: its waterline is not the sea";
            EXPECT_NEAR(physics["originAboveWaterline"].get<f32>(), rise, 2.0e-3f);
            EXPECT_NEAR(physics["thrustOffsetY"].get<f32>(), rig["rudder"]["bladeBottomY"].get<f32>() - rise, 6.0e-3f)
                << "the rudder's immersion is not sampled at the foot of its blade";

            // The collider is the hull where the hull is: keel to gunwale about the waterline.
            const f32 draft = rig["hull"]["draft"].get<f32>();
            const f32 freeboard = rig["hull"]["freeboardAmidships"].get<f32>();
            const f32 centreOfMass = 0.5f * (freeboard - draft);
            EXPECT_NEAR(physics["colliderHalfExtents"][1].get<f32>(), 0.5f * (freeboard + draft), 6.0e-3f)
                << "the collider is not the hull's depth, keel to gunwale";
            EXPECT_NEAR(physics["centreOfMassAboveWaterline"].get<f32>(), centreOfMass, 2.0e-3f);
            EXPECT_NEAR(physics["colliderOffsetY"].get<f32>(), centreOfMass - rise, 2.0e-3f)
                << "the collider is not where the hull is drawn: she would ground early or late";

            const f32 lever = physics["heelLever"].get<f32>();
            EXPECT_NEAR(physics["centreOfEffortY"].get<f32>(), centreOfMass + lever - rise, 6.0e-3f)
                << "the sail's force is not applied the heel lever above the centre of mass";
            EXPECT_NEAR(physics["drawnSailAboveCentreOfMass"].get<f32>(), rig["sail"]["centreOfArea"][1].get<f32>() - centreOfMass, 2.0e-3f);
            EXPECT_LT(lever, physics["drawnSailAboveCentreOfMass"].get<f32>())
                << "the game's sail force has more leverage than the drawn sail itself would give it";

            const f32 probesBelowCentreOfMass = centreOfMass + immersion;
            const f32 heelStiffness = mass * 9.81f * (x * x / immersion - probesBelowCentreOfMass) / lever;
            EXPECT_NEAR(heelStiffness, 62141.0f, 0.015f * 62141.0f)
                << "she no longer heels as far in a given wind as the game was tuned for";
            const f32 knockdownForce = mass * 9.81f * x / lever;
            EXPECT_NEAR(knockdownForce, 2600.0f * 9.81f * 1.35f / 2.0f, 0.02f * 17217.0f)
                << "the side force that lifts her windward bilge out is not what the game was tuned with: "
                   "she will lie over further, or less far, when a storm overpowers her";
            EXPECT_NEAR(physics["knockdownForce"].get<f32>(), knockdownForce, 20.0f);
            EXPECT_GT(immersion, physics["immersionDepth"].get<f32>())
                << "the hull's drag does not bite fully at rest, so every speed in the game moves";

            // The sail's force acts abaft the centre of mass by a lever that scales with the body's
            // yaw inertia (Jolt's, from the box collider), so a gust turns her into the wind as hard
            // as it always has. The rest of that lever is the hull's lead, which has to stay a lead.
            const f32 cx = physics["colliderHalfExtents"][0].get<f32>();
            const f32 cz = physics["colliderHalfExtents"][2].get<f32>();
            const f32 yawInertia = mass / 12.0f * (4.0f * cx * cx + 4.0f * cz * cz);
            const f32 weatherHelm = -physics["centreOfEffortZ"].get<f32>() / yawInertia;
            EXPECT_NEAR(weatherHelm, 5.73e-5f, 0.03f * 5.73e-5f)
                << "a loaded sail no longer turns her into the wind as hard as the game was tuned for";
            const f32 lead = rig["sail"]["centreOfArea"][2].get<f32>() - physics["centreOfEffortZ"].get<f32>();
            EXPECT_NEAR(physics["lateralResistanceLead"].get<f32>(), lead, 6.0e-3f);
            EXPECT_GT(lead, 0.0f) << "the hull's lateral resistance would have to act abaft her centre of mass";
            EXPECT_LT(lead, 0.15f * rig["hull"]["lengthWaterline"].get<f32>())
                << "no hull carries her lateral resistance that far forward: the mast is stepped too far forward";
        }

        BoatComponent hull;
        ApplyDriftHull(hull);
        const YAML::Node h = boat["BoatComponent"];
        EXPECT_NEAR(h["MaxThrust"].as<f32>(), hull.m_MaxThrust, kEps);
        EXPECT_NEAR(h["ThrustOffsetZ"].as<f32>(), physics["thrustOffsetZ"].get<f32>(), kEps);
        EXPECT_NEAR(h["ThrustOffsetZ"].as<f32>(), hull.m_ThrustOffsetZ, kEps);
        EXPECT_NEAR(h["ThrustOffsetY"].as<f32>(), physics["thrustOffsetY"].get<f32>(), kEps);
        EXPECT_NEAR(h["ThrustOffsetY"].as<f32>(), hull.m_ThrustOffsetY, kEps);
        EXPECT_NEAR(h["MaxRudderTorque"].as<f32>(), physics["maxRudderTorque"].get<f32>(), 0.5f);
        EXPECT_NEAR(h["MaxRudderTorque"].as<f32>(), hull.m_MaxRudderTorque, 0.5f);
        EXPECT_NEAR(h["YawDrag"].as<f32>(), physics["yawDrag"].get<f32>(), kEps);
        EXPECT_NEAR(h["YawDrag"].as<f32>(), hull.m_YawDrag, kEps);
        EXPECT_NEAR(h["RudderAuthoritySpeed"].as<f32>(), hull.m_RudderAuthoritySpeed, kEps);
        EXPECT_NEAR(h["LateralDrag"].as<f32>(), hull.m_LateralDrag, kEps);
        EXPECT_NEAR(h["ForwardDrag"].as<f32>(), hull.m_ForwardDrag, kEps);
        EXPECT_NEAR(h["ImmersionDepth"].as<f32>(), physics["immersionDepth"].get<f32>(), kEps);
        EXPECT_NEAR(h["ImmersionDepth"].as<f32>(), hull.m_ImmersionDepth, kEps);

        SailComponent sailRig;
        ApplyDriftRig(sailRig);
        const YAML::Node s = boat["SailComponent"];
        EXPECT_NEAR(s["CentreOfEffortY"].as<f32>(), physics["centreOfEffortY"].get<f32>(), kEps);
        EXPECT_NEAR(s["CentreOfEffortY"].as<f32>(), sailRig.m_CentreOfEffortY, kEps);
        EXPECT_NEAR(s["CentreOfEffortZ"].as<f32>(), physics["centreOfEffortZ"].get<f32>(), kEps);
        EXPECT_NEAR(s["CentreOfEffortZ"].as<f32>(), sailRig.m_CentreOfEffortZ, kEps);
        EXPECT_NEAR(s["SailArea"].as<f32>(), sailRig.m_SailArea, kEps);
        EXPECT_NEAR(s["MaxNormalCoefficient"].as<f32>(), sailRig.m_MaxNormalCoefficient, kEps);
        EXPECT_NEAR(s["MaxYardAngleDeg"].as<f32>(), sailRig.m_MaxYardAngleDeg, kEps);
        EXPECT_NEAR(s["TrimRateDeg"].as<f32>(), sailRig.m_TrimRateDeg, kEps);
        EXPECT_NEAR(s["AirDensity"].as<f32>(), sailRig.m_AirDensity, kEps);

        // --- Where the parts hang ------------------------------------------------
        const YAML::Node hullMesh = SceneEntity(scene, "Boat Hull");
        ASSERT_TRUE(hullMesh);
        ExpectVec3(hullMesh["TransformComponent"]["Translation"],
                   glm::vec3(0.0f, physics["hullMeshOffsetY"].get<f32>(), 0.0f), "hull mesh offset");
        ExpectVec3(hullMesh["TransformComponent"]["Scale"], glm::vec3(1.0f), "the hull is authored at true scale");

        const YAML::Node sail = SceneEntity(scene, "Boat Sail");
        ASSERT_TRUE(sail);
        ExpectVec3(sail["TransformComponent"]["Translation"], rig["sail"]["origin"], "the sail's origin is the mast");

        const YAML::Node rudder = SceneEntity(scene, "Boat Rudder");
        ASSERT_TRUE(rudder);
        ExpectVec3(rudder["TransformComponent"]["Translation"], rig["rudder"]["pivot"], "rudder pivot");
        EXPECT_NEAR(rudder["TransformComponent"]["Rotation"][2].as<f32>(),
                    glm::radians(rig["rudder"]["cantDeg"].get<f32>()), kEps)
            << "the rudder's cant";

        // Full helm is the one rig number the scene cannot carry: the controller
        // script holds it, in radians.
        {
            std::ifstream in(Sandbox() / "Assets/Scripts/LuaScripts/DriftBoatController.lua");
            ASSERT_TRUE(in.good()) << "DriftBoatController.lua is missing";
            const std::string script((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::smatch match;
            ASSERT_TRUE(std::regex_search(script, match, std::regex(R"re(kRudderMaxAngle\s*=\s*([0-9.]+))re")))
                << "DriftBoatController.lua no longer names kRudderMaxAngle";
            EXPECT_NEAR(std::stof(match[1].str()), glm::radians(rig["rudder"]["maxAngleDeg"].get<f32>()), 5.0e-3f)
                << "the drawn rudder does not swing as far as the generated rig says it can";
        }

        // The cloth. Every level of the sail carries the same morph targets, in the
        // order the script addresses them by (the engine names an imported target by
        // its index), and the scene gives the sail the component that lets the
        // engine deform it. A level without them would snap back to the file's own
        // shape as the camera pulled away.
        {
            const auto& names = rig["sail"]["morphTargets"];
            ASSERT_TRUE(names.is_array() && !names.empty()) << "the rig file names no morph targets for the sail";
            for (const auto& file : rig["files"]["sail"])
            {
                const std::string fileName = file["file"].get<std::string>();
                std::ifstream gltfFile(Sandbox() / "Assets/Models/DriftBoat" / fileName);
                const nlohmann::json gltf = nlohmann::json::parse(gltfFile, nullptr, /*allow_exceptions*/ false);
                ASSERT_FALSE(gltf.is_discarded()) << fileName;
                const auto& mesh = gltf["meshes"][0];
                ASSERT_TRUE(mesh["primitives"][0].contains("targets")) << fileName << " has no morph targets";
                EXPECT_EQ(mesh["primitives"][0]["targets"].size(), names.size()) << fileName;
                EXPECT_EQ(mesh["extras"]["targetNames"], names) << fileName << " holds its targets in another order";
            }
            EXPECT_TRUE(sail["MorphTargetComponent"]) << "the sail has no MorphTargetComponent: the engine will not deform it";

            std::ifstream in(Sandbox() / "Assets/Scripts/LuaScripts/DriftBoatController.lua");
            ASSERT_TRUE(in.good());
            const std::string script((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::smatch list;
            ASSERT_TRUE(std::regex_search(script, list, std::regex(R"re(kSailMorphs\s*=\s*\{([^}]*)\})re")))
                << "DriftBoatController.lua no longer lists kSailMorphs";
            const std::string body = list[1].str();
            std::vector<std::string> scriptNames;
            const std::regex quoted(R"re("([A-Za-z0-9]+)")re");
            for (std::sregex_iterator it(body.begin(), body.end(), quoted), end; it != end; ++it)
                scriptNames.push_back((*it)[1].str());
            EXPECT_EQ(scriptNames, names.get<std::vector<std::string>>())
                << "the script addresses the sail's shapes by index, in an order the files do not hold them in";

            for (const auto& [tag, key] : { std::pair{ "Boat Clew Port Furled", "clewPortFurled" },
                                            std::pair{ "Boat Clew Starboard Furled", "clewStarboardFurled" } })
            {
                const YAML::Node marker = SceneEntity(scene, tag);
                ASSERT_TRUE(marker) << tag;
                ExpectVec3(marker["TransformComponent"]["Translation"], rig["sail"][key], tag);
            }
        }

        // The running rigging: each line sits at its lead, each marker at the
        // sail's own corner. These are the names DriftBoatController.lua looks up.
        constexpr std::pair<const char*, const char*> kLeads[] = {
            { "Boat Sheet Port", "sheetLeadPort" },
            { "Boat Sheet Starboard", "sheetLeadStarboard" },
            { "Boat Brace Port", "braceLeadPort" },
            { "Boat Brace Starboard", "braceLeadStarboard" },
        };
        for (const auto& [tag, key] : kLeads)
        {
            const YAML::Node line = SceneEntity(scene, tag);
            ASSERT_TRUE(line) << tag;
            ExpectVec3(line["TransformComponent"]["Translation"], rig["lines"][key], tag);
        }
        constexpr std::pair<const char*, const char*> kMarkers[] = {
            { "Boat Clew Port", "clewPort" },
            { "Boat Clew Starboard", "clewStarboard" },
            { "Boat Yardarm Port", "yardArmPort" },
            { "Boat Yardarm Starboard", "yardArmStarboard" },
        };
        for (const auto& [tag, key] : kMarkers)
        {
            const YAML::Node marker = SceneEntity(scene, tag);
            ASSERT_TRUE(marker) << tag;
            ExpectVec3(marker["TransformComponent"]["Translation"], rig["sail"][key], tag);
        }

        // --- Levels of detail: the scene's chain is the generator's files --------
        AssetRegistry registry;
        ASSERT_TRUE(registry.Deserialize(Sandbox() / "AssetRegistry.oar"));
        for (const auto& [tag, key] : kParts)
        {
            const YAML::Node entity = SceneEntity(scene, tag);
            ASSERT_TRUE(entity) << tag;
            const auto& files = rig["files"][key];
            const YAML::Node levels = entity["LODGroupComponent"]["Levels"];
            ASSERT_TRUE(levels && levels.IsSequence()) << tag << " has no authored LOD levels";
            ASSERT_EQ(levels.size(), files.size()) << tag << ": one level per generated file";

            // Every coarser level was judged by the generator, and kept the shape.
            const auto& scores = rig["levelsOfDetail"][key];
            ASSERT_EQ(scores.size() + 1, files.size()) << tag << ": a coarser level was written without its silhouette being scored";
            for (const auto& score : scores)
            {
                for (const char* view : { "side", "end", "above" })
                    EXPECT_GE(score[view].get<f32>(), 0.90f) << tag << " level " << score["level"] << " loses the silhouette from " << view;
            }

            f32 previousDistance = 0.0f;
            u32 previousTriangles = 0;
            for (sizet i = 0; i < levels.size(); ++i)
            {
                const std::string file = files[i]["file"].get<std::string>();
                const u32 triangles = levels[i]["TriangleCount"].as<u32>();
                EXPECT_EQ(triangles, files[i]["triangles"].get<u32>()) << file << ": stale triangle count in the scene";
                if (i > 0)
                {
                    EXPECT_LT(triangles, previousTriangles) << file << " is not coarser than the level before it";
                    EXPECT_GT(levels[i]["MaxDistance"].as<f32>(), previousDistance) << file;
                }
                previousTriangles = triangles;
                previousDistance = levels[i]["MaxDistance"].as<f32>();
                // A coarser level takes over where the generator measured its silhouette.
                if (const auto& distances = rig["levelsOfDetail"]["distances"]; i + 1 < levels.size() && i < distances.size())
                    EXPECT_NEAR(previousDistance, distances[i].get<f32>(), kEps) << file << ": switch distance";

                // The level names a .olomesh, which names the glTF the generator wrote.
                const AssetMetadata mesh = registry.GetMetadata(AssetHandle(levels[i]["MeshHandle"].as<u64>()));
                ASSERT_TRUE(mesh.IsValid()) << tag << " level " << i << " names an unregistered asset";
                ASSERT_EQ(mesh.FilePath.extension(), ".olomesh") << mesh.FilePath.string();
                const YAML::Node olomesh = YAML::LoadFile((Sandbox() / mesh.FilePath).string());
                const AssetMetadata source = registry.GetMetadata(AssetHandle(olomesh["Mesh"]["MeshSource"].as<u64>()));
                ASSERT_TRUE(source.IsValid()) << mesh.FilePath.string() << " names an unregistered mesh source";
                EXPECT_EQ(source.FilePath.filename().string(), file) << mesh.FilePath.string();
                if (i == 0)
                {
                    EXPECT_EQ(static_cast<u64>(source.Handle), entity["MeshComponent"]["MeshSourceHandle"].as<u64>())
                        << tag << ": level 0 is not the mesh the entity draws";
                }
            }
        }
    }

    class DriftBoatDeliveryPack : public RendererAttachedTest
    {
        void BuildScene() override {}
    };

    TEST_F(DriftBoatDeliveryPack, TheBoatAndEveryLevelOfDetailSurvivePackaging)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const auto editorRoot = fs::path(OLO_TEST_EDITOR_ROOT);
        const auto sandbox = Sandbox();
        const auto projectRoot = TempDir("drift-boat-delivery");
        const auto scenePath = fs::path("Assets/Scenes/Drift.olo");
        const std::string modelDir = "Assets/Models/DriftBoat/";

        struct RestoreProject
        {
            Ref<Project> Previous = Project::GetActive();
            Ref<AssetManagerBase> Assets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            fs::path WorkingDirectory = fs::current_path();
            ~RestoreProject()
            {
                fs::current_path(WorkingDirectory);
                Project::Unload();
                if (Previous)
                    Project::NewInMemory(Previous->GetDirectory(), Previous->GetConfig());
                if (Assets)
                    Project::SetAssetManager(Assets);
            }
        } restore;

        // The boat out of the authored scene and nothing else of it: her entities
        // as Drift.olo has them, under the scene's own handle. Then everything
        // they name by path (the same walker Build Game uses) and the boat's
        // registered assets under their own handles.
        fs::create_directories(projectRoot / scenePath.parent_path());
        {
            const YAML::Node authored = YAML::LoadFile((sandbox / scenePath).string());
            YAML::Node boatOnly;
            for (const auto& entry : authored)
            {
                if (entry.first.as<std::string>() != "Entities")
                    boatOnly[entry.first.as<std::string>()] = entry.second;
            }
            sizet kept = 0;
            for (const auto& entity : authored["Entities"])
            {
                if (entity["TagComponent"] && entity["TagComponent"]["Tag"].as<std::string>("").starts_with("Boat"))
                {
                    boatOnly["Entities"].push_back(entity);
                    ++kept;
                }
            }
            ASSERT_EQ(kept, 14u) << "the boat is not the fourteen entities the guide lists";
            std::ofstream out(projectRoot / scenePath);
            out << boatOnly;
            ASSERT_TRUE(out.good());
        }
        sizet copied = 0;
        std::vector<std::string> unresolved;
        std::string error;
        ASSERT_TRUE(StageSceneReferencedContent({ projectRoot / scenePath }, sandbox, "Assets", editorRoot, projectRoot,
                                                copied, unresolved, error))
            << error;
        ASSERT_TRUE(unresolved.empty()) << (unresolved.empty() ? "" : unresolved.front());
        fs::create_directories(projectRoot / modelDir);
        for (const auto& file : fs::directory_iterator(sandbox / modelDir))
        {
            // A .gltf's buffer is not an asset, but the mesh is nothing without it.
            if (file.path().extension() == ".bin")
                fs::copy_file(file.path(), projectRoot / modelDir / file.path().filename(), fs::copy_options::overwrite_existing);
        }

        AssetRegistry shipped;
        ASSERT_TRUE(shipped.Deserialize(sandbox / "AssetRegistry.oar"));
        AssetRegistry selected;
        for (const auto& metadata : shipped.GetAllAssets())
        {
            if (metadata.FilePath != scenePath && !metadata.FilePath.generic_string().starts_with(modelDir))
                continue;
            selected.AddAsset(metadata);
            if (metadata.FilePath == scenePath)
                continue; // written above, boat only
            fs::create_directories((projectRoot / metadata.FilePath).parent_path());
            fs::copy_file(sandbox / metadata.FilePath, projectRoot / metadata.FilePath, fs::copy_options::overwrite_existing);
        }
        const AssetHandle sceneHandle = selected.GetHandleFromPath(scenePath);
        ASSERT_NE(static_cast<u64>(sceneHandle), 0u);
        ASSERT_TRUE(selected.Serialize(projectRoot / "AssetRegistry.oar"));
        ProjectConfig config;
        config.Name = "DriftBoat";
        config.AssetDirectory = "Assets";
        config.StartScene = "Scenes/Drift.olo";
        ASSERT_TRUE(Project::NewInMemory(projectRoot, config));
        auto editorAssets = Ref<EditorAssetManager>::Create();
        editorAssets->Initialize(false);
        Project::SetAssetManager(editorAssets);

        const fs::path package = projectRoot / "Package";
        std::atomic<f32> progress{ 0.0f };
        AssetPackBuilder::BuildSettings settings;
        settings.m_OutputPath = package / "Assets/AssetPack.olopack";
        settings.m_IncludeScriptModule = false;
        settings.m_IncludeLocalizationFiles = false;
        const auto packed = AssetPackBuilder::BuildFromRegistry(selected, settings, progress);
        ASSERT_TRUE(packed.m_Success) << packed.m_ErrorMessage;
        EXPECT_EQ(packed.m_FailedAssetCount, 0u);
        EXPECT_EQ(packed.m_SceneCount, 1u);
        ASSERT_TRUE(StageSceneReferencedContent({ projectRoot / scenePath }, projectRoot, "Assets", editorRoot, package,
                                                copied, unresolved, error))
            << error;
        EXPECT_TRUE(unresolved.empty()) << (unresolved.empty() ? "" : unresolved.front());

        // Load the pack cold, with the source project's copy of the boat gone. A packed
        // material opens each texture's recorded source path first and the pack's copy
        // only when that file is missing, so with the folder in place every map below
        // would come from the project and none from the pack.
        fs::remove_all(projectRoot / modelDir);
        ASSERT_TRUE(Project::NewInMemory(package, config));
        fs::current_path(package);
        auto runtime = Ref<RuntimeAssetManager>::Create(false);
        ASSERT_TRUE(runtime->LoadAssetPack(package / "Assets/AssetPack.olopack"));
        Project::SetAssetManager(runtime);
        auto scene = runtime->GetAsset(sceneHandle).As<Scene>();
        ASSERT_TRUE(scene);
        for (const auto& metadata : selected.GetAllAssets())
        {
            const auto asset = runtime->GetAsset(metadata.Handle);
            ASSERT_TRUE(asset) << metadata.FilePath.string();
            EXPECT_FALSE(PlaceholderAssetManager::IsPlaceholderAsset(asset)) << metadata.FilePath.string();
        }

        // Each drawn part has its mesh, one submesh on one imported material, and
        // a chain of levels that resolve and get coarser.
        const nlohmann::json rig = LoadRig();
        ASSERT_FALSE(rig.is_discarded());
        for (const auto& [tag, key] : kParts)
        {
            Entity entity = scene->FindEntityByName(tag);
            ASSERT_TRUE(entity) << tag;
            ASSERT_TRUE(entity.HasComponent<MeshComponent>()) << tag;
            const Ref<MeshSource> source = entity.GetComponent<MeshComponent>().m_MeshSource;
            ASSERT_TRUE(source) << tag << " lost its mesh in the pack";
            EXPECT_EQ(source->GetSubmeshes().Num(), 1) << tag << ": the LOD chain swaps one submesh";
            ASSERT_EQ(source->GetImportedMaterials().size(), 1u) << tag;
            const Ref<Material> material = source->GetImportedMaterials()[0];
            ASSERT_TRUE(material) << tag;
            const std::pair<const char*, Ref<Texture2D>> maps[] = {
                { "base colour", material->GetAlbedoMap() },
                { "normal", material->GetNormalMap() },
                { "occlusion, roughness and metallic", material->GetMetallicRoughnessMap() },
            };
            for (const auto& [slot, map] : maps)
            {
                ASSERT_TRUE(map && map->IsLoaded()) << tag << ": the pack did not deliver the " << slot << " map";
                EXPECT_GE(map->GetWidth(), 1024u) << tag << ": the " << slot << " map is not the generated atlas";
            }

            ASSERT_TRUE(entity.HasComponent<LODGroupComponent>()) << tag;
            const auto& lod = entity.GetComponent<LODGroupComponent>();
            EXPECT_TRUE(lod.m_Enabled);
            EXPECT_FALSE(lod.m_AutoGenerated) << tag << ": the levels are the generator's, not a simplifier's";
            const auto& files = rig["files"][key];
            ASSERT_EQ(static_cast<sizet>(lod.m_LODGroup.Levels.Num()), files.size()) << tag;
            for (i32 i = 0; i < lod.m_LODGroup.Levels.Num(); ++i)
            {
                const auto& level = lod.m_LODGroup.Levels[i];
                const auto mesh = runtime->GetAsset(level.MeshHandle).As<Mesh>();
                ASSERT_TRUE(mesh) << tag << " level " << i << " did not load from the pack";
                EXPECT_EQ(mesh->GetIndexCount() / 3u, files[static_cast<sizet>(i)]["triangles"].get<u32>())
                    << tag << " level " << i << " is not the mesh the generator wrote";
                // The sail's shapes ship with every level, or a packaged sail is a board.
                if (files[static_cast<sizet>(i)].contains("morphTargets"))
                {
                    const Ref<MeshSource> levelSource = mesh->GetMeshSource();
                    ASSERT_TRUE(levelSource && levelSource->HasMorphTargets())
                        << tag << " level " << i << " lost its morph targets in the pack";
                    EXPECT_EQ(levelSource->GetMorphTargets()->GetTargetCount(),
                              files[static_cast<sizet>(i)]["morphTargets"].get<u32>())
                        << tag << " level " << i;
                }
            }
            // In Play the chase camera is 9-14 m off: always the full boat.
            EXPECT_EQ(lod.m_LODGroup.SelectLOD(14.0f), 0) << tag;
            EXPECT_EQ(lod.m_LODGroup.SelectLOD(1000.0f), lod.m_LODGroup.Levels.Num() - 1) << tag;
        }
    }
} // namespace OloEngine::Tests
