#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string_view>

namespace OloEngine
{
    class Scene;

    // The 3D physics surface scripts reach (issue #1559), shared by the C# and the
    // Lua glue so the two languages cannot drift: each binding is a thin call into
    // one of these. Every call names its entity by UUID, works on the live Jolt body
    // (not the component's authored fields), validates its floats, and says through
    // its result and the log why it did nothing. A script that pushes a body that
    // does not exist yet, or a static one, is told so rather than ignored.
    namespace Physics3DScriptApi
    {
        enum class Result : u8
        {
            Ok = 0,
            NoScene,     // no scene context, or 3D physics is not running
            NoEntity,    // the UUID names no entity in the scene
            NoBody,      // the entity has no live Jolt body (no Rigidbody3DComponent, or Play has not started)
            NotDynamic,  // the call moves a body by impulse/velocity and this one is static or kinematic
            NotFinite,   // a NaN or infinite argument
            Unsupported, // the body cannot take that type (a body created Static cannot become dynamic)
            InvalidArgument
        };

        [[nodiscard]] const char* ToString(Result result);

        // Linear impulse through the centre of mass (N*s). Dynamic bodies only.
        Result ApplyLinearImpulse(Scene& scene, UUID entityID, const glm::vec3& impulse);

        // Velocity of the live body (m/s, rad/s). Setters need a dynamic or kinematic body.
        Result GetLinearVelocity(Scene& scene, UUID entityID, glm::vec3& outVelocity);
        Result SetLinearVelocity(Scene& scene, UUID entityID, const glm::vec3& velocity);
        Result GetAngularVelocity(Scene& scene, UUID entityID, glm::vec3& outVelocity);
        Result SetAngularVelocity(Scene& scene, UUID entityID, const glm::vec3& velocity);

        // Switch the live body's motion type (and the component's m_Type with it).
        // Kinematic <-> Dynamic always works; a body that was created Static has no
        // motion properties in Jolt and answers Unsupported.
        Result SetBodyType(Scene& scene, UUID entityID, EBodyType type);

        // Every entity `entityID` touches now or began touching in the steps since the
        // scripts last ran (JoltScene::GetContacts). Sensors included.
        Result GetContacts(Scene& scene, UUID entityID, TArray<UUID>& outContacts);

        // Carry an entity on another one, optionally on one of its skeleton's bones
        // (Scene::AttachEntity), and release it back to physics (Scene::DetachEntity).
        Result AttachTo(Scene& scene, UUID entityID, UUID parentID, std::string_view boneName,
                        const glm::vec3& localTranslation, const glm::quat& localRotation);
        // AttachTo, keeping the entity where it is in the world (Scene::AttachEntityInPlace).
        Result AttachInPlace(Scene& scene, UUID entityID, UUID parentID, std::string_view boneName);
        Result Detach(Scene& scene, UUID entityID);
    } // namespace Physics3DScriptApi
} // namespace OloEngine
