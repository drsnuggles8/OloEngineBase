#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    class Scene;

    // An entity's pose in its parent's frame: what TransformComponent authors,
    // or a world pose converted back into that frame for writeback. A root
    // entity's parent frame is the world, but only Scene may turn a LocalPose
    // into a WorldPose (see below).
    struct LocalPose
    {
        glm::vec3 Position{ 0.0f };
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale{ 1.0f };
    };

    // An entity's rigid pose in world space, after composing its parent chain
    // (issue #1797). A consumer that places something in the world takes this
    // type rather than a raw vec3/quat, so an authored TransformComponent value
    // cannot reach it by accident: WorldPose is not an aggregate, it does not
    // convert from LocalPose or a matrix, and only Scene (which owns the
    // composition) and FromSimulation construct one. The fields stay public
    // for reading, so a consumer reads pose.Position as before.
    //
    // How Scene fills it (docs/agent-rules/world-space-consumers-take-a-worldpose.md):
    //   * Position is the composed translation, exact under any affine parent.
    //   * Rotation is the composed basis made orthonormal with X kept, Y
    //     orthogonalised and Z = X x Y. Under shear this is an approximation;
    //     a consumer that needs the exact affine frame uses
    //     Scene::ComputeWorldTransform.
    //   * Scale is the composed column lengths, with Z negated for a mirror.
    //     A root keeps its authored values, signs included, so a mirrored root
    //     and a mirrored parented twin rebuild the same frame from different
    //     Rotation/Scale pairs.
    //   * Under a parent that collapses one axis, the two surviving axes still
    //     define Rotation. When fewer survive, the basis holds no rotation, so
    //     Rotation composes the chain's rotations alone.
    //   * A non-finite parent frame leaves Position and Scale non-finite.
    //     Placement consumers check Math::IsFinite and refuse such a pose.
    class WorldPose
    {
      public:
        // A pose read back from a solver that simulates in world space (a Jolt
        // body or character controller). Solver bodies are rigid, so Scale is 1.
        // This is the one public way to make a WorldPose; WorldPoseContractTest
        // pins the files that call it.
        [[nodiscard]] static WorldPose FromSimulation(const glm::vec3& position, const glm::quat& rotation)
        {
            return WorldPose(position, rotation, glm::vec3(1.0f));
        }

        glm::vec3 Position;
        glm::quat Rotation;
        glm::vec3 Scale;

      private:
        friend class Scene;

        WorldPose(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale)
            : Position(position), Rotation(rotation), Scale(scale)
        {
        }
    };
} // namespace OloEngine
