// OLO_TEST_LAYER: unit
// =============================================================================
// WorldPoseContractTest.cpp — a parent-local pose cannot reach a world-space
// placement consumer, and the composed world pose follows a written policy
// (issue #1797).
//
// THE BUG CLASS. #1714 and #1707: a world-space consumer was handed the
// authored TransformComponent value, which is parent-local, so a parented
// body, emitter or light was placed relative to the world origin instead of
// its parent. Both poses were a glm::vec3/glm::quat, so nothing stopped it.
// JoltBody::CreateBodySettings took TransformComponent::Translation until
// #1771, and after #1771 it still accepted any vec3.
//
// WHAT THIS PINS.
//   1. The type contract (compile time). WorldPose is not an aggregate, does
//      not convert from LocalPose or a matrix, and Scene::SetPhysicsWorldPose
//      no longer accepts a raw vec3/quat. JoltBody::CreateBodySettings takes
//      the same type; it is private, so its guard is these traits plus (3).
//   2. Twins. A parented entity and a root twin authored at the equivalent
//      world pose answer the same ComputeWorldTRS for identity, translated,
//      rotated and nonuniform-scaled parents.
//   3. The one public way to make a WorldPose, FromSimulation, is called only
//      from the two solver readback sites. A new caller fails this test and
//      has to justify itself here.
//   4. The written policy for what TRS cannot express: a sheared composed
//      basis, a parent that collapses one or all axes, and an entity read
//      before the first propagation pass.
//
// Headless, no GL, no physics world.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Pose.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // -------------------------------------------------------------------
        // 1. The type contract.
        // -------------------------------------------------------------------
        template<typename Pose>
        concept WritesBackAsWorldPose = requires(Scene& scene, entt::entity entity, const Pose& pose, const JoltScene& physics) {
            scene.SetPhysicsWorldPose(entity, pose, physics);
        };

        template<typename Position, typename Rotation>
        concept WritesBackRawVectors = requires(Scene& scene, entt::entity entity, const Position& position, const Rotation& rotation,
                                                const JoltScene& physics) {
            scene.SetPhysicsWorldPose(entity, position, rotation, physics);
        };

        // The authored values a consumer might reach for by mistake.
        static_assert(!std::is_aggregate_v<WorldPose>,
                      "An aggregate WorldPose accepts WorldPose{ tc.Translation, ... } and "
                      "{ tc.Translation, ... } at any call site.");
        static_assert(!std::is_constructible_v<WorldPose, glm::vec3, glm::quat, glm::vec3>);
        static_assert(!std::is_constructible_v<WorldPose, glm::vec3, glm::quat>);
        static_assert(!std::is_constructible_v<WorldPose, glm::vec3>);
        static_assert(!std::is_constructible_v<WorldPose, glm::mat4>);
        static_assert(!std::is_constructible_v<WorldPose, LocalPose>);
        static_assert(!std::is_convertible_v<LocalPose, WorldPose>);
        static_assert(!std::is_default_constructible_v<WorldPose>);

        // The writeback boundary.
        static_assert(!WritesBackRawVectors<glm::vec3, glm::quat>, "SetPhysicsWorldPose must not accept a raw vec3/quat.");
        static_assert(!WritesBackAsWorldPose<LocalPose>, "SetPhysicsWorldPose must not accept a LocalPose.");
        static_assert(WritesBackAsWorldPose<WorldPose>, "Anchor: the concept itself must be satisfiable.");

        // A WorldPose stays an ordinary value once it exists.
        static_assert(std::is_trivially_copyable_v<WorldPose>);
        static_assert(sizeof(WorldPose) == sizeof(LocalPose), "No hidden state: same bytes as the old WorldTRS.");

        // -------------------------------------------------------------------
        // Helpers.
        // -------------------------------------------------------------------
        void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, f32 tolerance, const char* what)
        {
            EXPECT_NEAR(actual.x, expected.x, tolerance) << what;
            EXPECT_NEAR(actual.y, expected.y, tolerance) << what;
            EXPECT_NEAR(actual.z, expected.z, tolerance) << what;
        }

        void ExpectSameRotation(const glm::quat& actual, const glm::quat& expected, const char* what)
        {
            // q and -q are the same rotation.
            EXPECT_NEAR(std::abs(glm::dot(actual, expected)), 1.0f, 1.0e-5f) << what;
        }

        void Author(Entity entity, const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
        {
            auto& tc = entity.GetComponent<TransformComponent>();
            tc.Translation = translation;
            tc.SetRotation(rotation);
            tc.Scale = scale;
        }

        // A parent and a child authored in its frame, plus a root twin
        // authored directly at the world pose the child should have.
        struct TwinCase
        {
            const char* Name;
            glm::vec3 ParentTranslation;
            glm::quat ParentRotation;
            glm::vec3 ParentScale;
            glm::vec3 ChildTranslation;
            glm::quat ChildRotation;
            glm::vec3 ChildScale;
        };

        // The world pose the child should have, derived by hand rather than by
        // the code under test. Valid when the composed basis is still TRS: the
        // parent scale is uniform, or the child has no rotation of its own.
        struct ExpectedPose
        {
            glm::vec3 Position;
            glm::quat Rotation;
            glm::vec3 Scale;
        };

        [[nodiscard]] ExpectedPose ExpectedWorld(const TwinCase& c)
        {
            return { c.ParentTranslation + c.ParentRotation * (c.ParentScale * c.ChildTranslation),
                     c.ParentRotation * c.ChildRotation, c.ParentScale * c.ChildScale };
        }

        [[nodiscard]] std::string ReadFile(const fs::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buffer;
            buffer << f.rdbuf();
            return buffer.str();
        }
    } // namespace

    // -----------------------------------------------------------------------
    // 2. Twins.
    // -----------------------------------------------------------------------
    TEST(WorldPoseContract, ParentedEntityMatchesItsRootTwinInEverySupportedCase)
    {
        const glm::quat yawPitch = glm::quat(glm::vec3(glm::radians(30.0f), glm::radians(90.0f), 0.0f));
        const glm::quat childTilt = glm::quat(glm::vec3(0.0f, 0.0f, glm::radians(20.0f)));
        const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);

        const TwinCase cases[] = {
            { "identity parent", glm::vec3(0.0f), identity, glm::vec3(1.0f), { 1.5f, 2.0f, -0.5f }, childTilt, { 1.0f, 2.0f, 0.5f } },
            { "translated parent", { -12.0f, 6.0f, 9.0f }, identity, glm::vec3(1.0f), { 1.5f, 2.0f, -0.5f }, childTilt, glm::vec3(1.0f) },
            { "rotated parent", { -12.0f, 6.0f, 9.0f }, yawPitch, glm::vec3(1.0f), { 1.5f, 2.0f, -0.5f }, childTilt, { 1.0f, 2.0f, 0.5f } },
            { "nonuniform-scaled parent", { 4.0f, -1.0f, 2.0f }, yawPitch, { 2.0f, 0.5f, 3.0f }, { 1.5f, 2.0f, -0.5f }, identity, { 1.5f, 1.0f, 2.0f } },
        };

        for (const TwinCase& c : cases)
        {
            SCOPED_TRACE(c.Name);
            auto scene = Ref<Scene>::Create();
            Entity parent = scene->CreateEntity("Parent");
            Author(parent, c.ParentTranslation, c.ParentRotation, c.ParentScale);
            Entity child = scene->CreateEntity("Child");
            child.SetParent(parent);
            Author(child, c.ChildTranslation, c.ChildRotation, c.ChildScale);

            const ExpectedPose expected = ExpectedWorld(c);
            Entity twin = scene->CreateEntity("RootTwin");
            Author(twin, expected.Position, expected.Rotation, expected.Scale);
            scene->PropagateWorldTransforms();

            const WorldPose parented = scene->ComputeWorldTRS(child);
            const WorldPose root = scene->ComputeWorldTRS(twin);

            ExpectVec3Near(parented.Position, root.Position, 1.0e-4f, "position");
            ExpectSameRotation(parented.Rotation, root.Rotation, "rotation");
            ExpectVec3Near(parented.Scale, root.Scale, 1.0e-4f, "scale");

            // The propagated matrix agrees with the root twin's too.
            const glm::mat4 parentedMatrix = scene->GetWorldTransform(child);
            const glm::mat4 rootMatrix = scene->GetWorldTransform(twin);
            for (int column = 0; column < 4; ++column)
            {
                ExpectVec3Near(glm::vec3(parentedMatrix[column]), glm::vec3(rootMatrix[column]), 1.0e-4f, "world matrix column");
            }
        }
    }

    TEST(WorldPoseContract, RootPoseIsTheAuthoredValueBitForBit)
    {
        auto scene = Ref<Scene>::Create();
        Entity root = scene->CreateEntity("Root");
        Author(root, { 0.1f, -7.3f, 1.0e5f }, glm::normalize(glm::quat(0.3f, 0.1f, -0.7f, 0.2f)), { -1.0f, 2.0f, 0.25f });
        const auto& tc = root.GetComponent<TransformComponent>();

        const WorldPose pose = scene->ComputeWorldTRS(root);
        EXPECT_EQ(0, std::memcmp(&pose.Position, &tc.Translation, sizeof(glm::vec3)));
        const glm::quat authored = tc.GetRotation();
        EXPECT_EQ(0, std::memcmp(&pose.Rotation, &authored, sizeof(glm::quat)));
        EXPECT_EQ(0, std::memcmp(&pose.Scale, &tc.Scale, sizeof(glm::vec3)));
    }

    // -----------------------------------------------------------------------
    // 3. The escape hatch has exactly the callers it was made for.
    // -----------------------------------------------------------------------
    TEST(WorldPoseContract, OnlySolverReadbackVouchesForAWorldPose)
    {
        const fs::path repo = fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
        const std::set<std::string, std::less<>> allowed = {
            "OloEngine/src/OloEngine/Physics3D/JoltScene.cpp", // rigid body readback
            "OloEngine/src/OloEngine/Scene/Scene.cpp",         // character controller readback
        };

        std::set<std::string, std::less<>> callers;
        u32 scanned = 0;
        for (const fs::path& root : { repo / "OloEngine" / "src", repo / "OloEditor" / "src" })
        {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
            {
                std::error_code entryEc;
                if (ec || !it->is_regular_file(entryEc) || entryEc)
                    continue;
                const std::string extension = it->path().extension().string();
                if (extension != ".cpp" && extension != ".h" && extension != ".inl")
                    continue;
                ++scanned;
                if (ReadFile(it->path()).find("WorldPose::FromSimulation(") != std::string::npos)
                    callers.insert(fs::relative(it->path(), repo).generic_string());
            }
        }

        // Floor guard: a wrong root would scan nothing and pass vacuously.
        ASSERT_GT(scanned, 1000u) << "the source scan found almost nothing; is OLO_TEST_EDITOR_ROOT right?";
        EXPECT_EQ(callers, allowed)
            << "WorldPose::FromSimulation vouches that a pose is already in world space. A new caller must "
               "read from a world-space source (a solver body), never from a TransformComponent; add it to "
               "the allowed set here with the reason.";
    }

    // -----------------------------------------------------------------------
    // 4. The written policy for what TRS cannot express.
    // -----------------------------------------------------------------------
    TEST(WorldPoseContract, ShearedBasisKeepsExactPositionAndAnXPreservingRotation)
    {
        // A nonuniform parent with a rotated child shears the grandchild's frame.
        auto scene = Ref<Scene>::Create();
        Entity parent = scene->CreateEntity("Stretched");
        Author(parent, { 2.0f, 0.0f, -1.0f }, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), { 3.0f, 1.0f, 1.0f });
        Entity middle = scene->CreateEntity("Rotated");
        middle.SetParent(parent);
        Author(middle, glm::vec3(0.0f), glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(1.0f));
        Entity leaf = scene->CreateEntity("Leaf");
        leaf.SetParent(middle);
        Author(leaf, { 1.0f, 0.5f, 0.25f }, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));

        const glm::mat4 world = scene->ComputeWorldTransform(leaf);
        const glm::vec3 x(world[0]);
        const glm::vec3 y(world[1]);
        ASSERT_GT(std::abs(glm::dot(glm::normalize(x), glm::normalize(y))), 0.1f) << "the fixture must actually shear";

        const WorldPose pose = scene->ComputeWorldTRS(leaf);
        ExpectVec3Near(pose.Position, glm::vec3(world[3]), 1.0e-5f, "position is exact under shear");
        ExpectVec3Near(pose.Rotation * glm::vec3(1.0f, 0.0f, 0.0f), glm::normalize(x), 1.0e-5f, "rotation keeps world X");
        ExpectVec3Near(pose.Rotation * glm::vec3(0.0f, 0.0f, 1.0f), glm::normalize(glm::cross(x, y)), 1.0e-5f,
                       "rotation Z is the basis normal");
        ExpectVec3Near(pose.Scale, { glm::length(x), glm::length(y), glm::length(glm::vec3(world[2])) }, 1.0e-5f,
                       "scale is the column lengths");

        // TRS cannot hold the shear, so rebuilding a matrix from the pose
        // loses it. A consumer that needs the exact frame uses the matrix.
        const glm::mat4 rebuilt = glm::translate(glm::mat4(1.0f), pose.Position) * glm::mat4_cast(pose.Rotation) *
                                  glm::scale(glm::mat4(1.0f), pose.Scale);
        EXPECT_GT(glm::length(glm::vec3(rebuilt[1]) - y), 0.1f) << "the lossy part of the policy";
    }

    TEST(WorldPoseContract, SingularParentsFollowTheCollapsedAxisPolicy)
    {
        const glm::quat yaw = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::quat childRotation = glm::angleAxis(glm::radians(15.0f), glm::vec3(1.0f, 0.0f, 0.0f));

        {
            SCOPED_TRACE("one collapsed axis: the two surviving axes define the rotation");
            auto scene = Ref<Scene>::Create();
            Entity parent = scene->CreateEntity("Flattened");
            Author(parent, { 5.0f, 1.0f, -2.0f }, yaw, { 1.0f, 0.0f, 1.0f });
            Entity child = scene->CreateEntity("Child");
            child.SetParent(parent);
            Author(child, { 1.0f, 2.0f, 3.0f }, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));

            const WorldPose pose = scene->ComputeWorldTRS(child);
            ExpectVec3Near(pose.Position, glm::vec3(5.0f, 1.0f, -2.0f) + yaw * glm::vec3(1.0f, 0.0f, 3.0f), 1.0e-5f,
                           "the collapsed axis drops out of the position");
            ExpectSameRotation(pose.Rotation, yaw, "rotation from the surviving X and Z");
            EXPECT_FLOAT_EQ(pose.Scale.y, 0.0f);
            EXPECT_TRUE(scene->HasSingularParentFrame(child, /*planar=*/false));
        }
        {
            SCOPED_TRACE("every axis collapsed: no world rotation exists, the authored one is kept");
            auto scene = Ref<Scene>::Create();
            Entity parent = scene->CreateEntity("Collapsed");
            Author(parent, { 5.0f, 1.0f, -2.0f }, yaw, glm::vec3(0.0f));
            Entity child = scene->CreateEntity("Child");
            child.SetParent(parent);
            Author(child, { 1.0f, 2.0f, 3.0f }, childRotation, glm::vec3(1.0f));

            const WorldPose pose = scene->ComputeWorldTRS(child);
            ExpectVec3Near(pose.Position, { 5.0f, 1.0f, -2.0f }, 1.0e-6f, "everything collapses onto the parent origin");
            ExpectSameRotation(pose.Rotation, childRotation, "authored local rotation");
            ExpectVec3Near(pose.Scale, glm::vec3(0.0f), 0.0f, "scale");

            // A world-space write cannot be expressed in a singular frame,
            // so it is refused and the authored local pose stays.
            const glm::vec3 before = child.GetComponent<TransformComponent>().Translation;
            EXPECT_FALSE(scene->TrySetWorldPosition(child, { 9.0f, 9.0f, 9.0f }));
            ExpectVec3Near(child.GetComponent<TransformComponent>().Translation, before, 0.0f, "local pose untouched");
        }
    }

    TEST(WorldPoseContract, ComputeIsValidBeforeTheFirstPropagationAndGetIsNot)
    {
        auto scene = Ref<Scene>::Create();
        Entity parent = scene->CreateEntity("Parent");
        Author(parent, { 10.0f, 0.0f, 0.0f }, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
        Entity child = scene->CreateEntity("NewChild");
        child.SetParent(parent);
        Author(child, { 0.0f, 3.0f, 0.0f }, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f));

        // Compute* composes on demand, so a placement consumer (body creation)
        // sees the right pose for an entity created this tick.
        ExpectVec3Near(scene->ComputeWorldTRS(child).Position, { 10.0f, 3.0f, 0.0f }, 1.0e-6f, "ComputeWorldTRS");
        ExpectVec3Near(scene->ComputeWorldPosition(child), { 10.0f, 3.0f, 0.0f }, 1.0e-6f, "ComputeWorldPosition");

        // Get* reads the propagation cache and falls back to the LOCAL matrix
        // until the first pass. This is the documented contract, pinned so a
        // change to it is deliberate.
        ExpectVec3Near(glm::vec3(scene->GetWorldTransform(child)[3]), { 0.0f, 3.0f, 0.0f }, 0.0f, "GetWorldTransform before propagation");
        ExpectVec3Near(scene->GetWorldPosition(child), { 0.0f, 3.0f, 0.0f }, 0.0f, "GetWorldPosition before propagation");

        scene->PropagateWorldTransforms();
        ExpectVec3Near(glm::vec3(scene->GetWorldTransform(child)[3]), { 10.0f, 3.0f, 0.0f }, 1.0e-6f, "GetWorldTransform after propagation");
        ExpectVec3Near(scene->GetWorldPosition(child), { 10.0f, 3.0f, 0.0f }, 1.0e-6f, "GetWorldPosition after propagation");
    }
} // namespace OloEngine::Tests
