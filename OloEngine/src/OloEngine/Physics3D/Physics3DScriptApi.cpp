#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/Physics3DScriptApi.h"

#include "OloEngine/Physics3D/JoltBody.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <cmath>
#include <mutex>
#include <unordered_set>

namespace OloEngine::Physics3DScriptApi
{
    namespace
    {
        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        [[nodiscard]] bool IsFinite(const glm::quat& q)
        {
            return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
        }

        // Resolve the live body, or say why there is none.
        [[nodiscard]] Result ResolveBody(Scene& scene, UUID entityID, Ref<JoltBody>& outBody)
        {
            JoltScene* physics = scene.GetPhysicsScene();
            if (physics == nullptr || !physics->IsInitialized())
            {
                return Result::NoScene;
            }
            if (!scene.TryGetEntityWithUUID(entityID))
            {
                return Result::NoEntity;
            }
            outBody = physics->GetBodyByEntityID(entityID);
            if (!outBody || !outBody->IsValid())
            {
                return Result::NoBody;
            }
            return Result::Ok;
        }

        // One warning per (call, result) pair and entity would still flood at 60 Hz
        // from a script that calls every frame, so the log names the first refusal
        // per call site and entity only.
        void WarnOnce(const char* call, UUID entityID, Result result)
        {
            static std::mutex s_Mutex;
            static std::unordered_set<u64> s_Warned;
            const u64 key = static_cast<u64>(entityID) ^ (std::hash<std::string_view>{}(call) * 31u) ^ static_cast<u64>(result);
            {
                std::scoped_lock lock(s_Mutex);
                if (!s_Warned.insert(key).second)
                {
                    return;
                }
            }
            OLO_CORE_WARN("[Physics3D script] {} on entity {} did nothing: {}", call, static_cast<u64>(entityID), ToString(result));
        }

        [[nodiscard]] Result Report(const char* call, UUID entityID, Result result)
        {
            if (result != Result::Ok)
            {
                WarnOnce(call, entityID, result);
            }
            return result;
        }
    } // namespace

    const char* ToString(Result result)
    {
        switch (result)
        {
            case Result::Ok:
                return "ok";
            case Result::NoScene:
                return "3D physics is not running";
            case Result::NoEntity:
                return "no such entity";
            case Result::NoBody:
                return "the entity has no live Jolt body (no Rigidbody3DComponent, or Play has not started)";
            case Result::NotDynamic:
                return "the body is not dynamic";
            case Result::NotFinite:
                return "a non-finite argument";
            case Result::Unsupported:
                return "the body was created Static and cannot change motion type";
            case Result::InvalidArgument:
                return "an invalid argument";
        }
        return "unknown";
    }

    Result ApplyLinearImpulse(Scene& scene, UUID entityID, const glm::vec3& impulse)
    {
        if (!IsFinite(impulse))
        {
            return Report("ApplyLinearImpulse", entityID, Result::NotFinite);
        }
        Ref<JoltBody> body;
        if (const Result r = ResolveBody(scene, entityID, body); r != Result::Ok)
        {
            return Report("ApplyLinearImpulse", entityID, r);
        }
        if (!body->IsDynamic())
        {
            return Report("ApplyLinearImpulse", entityID, Result::NotDynamic);
        }
        body->AddForce(impulse, EForceMode::Impulse, true);
        return Result::Ok;
    }

    Result GetLinearVelocity(Scene& scene, UUID entityID, glm::vec3& outVelocity)
    {
        Ref<JoltBody> body;
        if (const Result r = ResolveBody(scene, entityID, body); r != Result::Ok)
        {
            return Report("GetLinearVelocity", entityID, r);
        }
        // A carried body is teleported, so Jolt reports no velocity for it; the
        // velocity a script means is the one it is carried at.
        const JoltScene* physics = scene.GetPhysicsScene();
        outVelocity = physics->IsCarried(entityID) ? physics->GetCarriedVelocity(entityID) : body->GetLinearVelocity();
        return Result::Ok;
    }

    Result SetLinearVelocity(Scene& scene, UUID entityID, const glm::vec3& velocity)
    {
        if (!IsFinite(velocity))
        {
            return Report("SetLinearVelocity", entityID, Result::NotFinite);
        }
        Ref<JoltBody> body;
        if (const Result r = ResolveBody(scene, entityID, body); r != Result::Ok)
        {
            return Report("SetLinearVelocity", entityID, r);
        }
        if (body->IsStatic())
        {
            return Report("SetLinearVelocity", entityID, Result::NotDynamic);
        }
        body->SetLinearVelocity(velocity);
        return Result::Ok;
    }

    Result GetAngularVelocity(Scene& scene, UUID entityID, glm::vec3& outVelocity)
    {
        Ref<JoltBody> body;
        if (const Result r = ResolveBody(scene, entityID, body); r != Result::Ok)
        {
            return Report("GetAngularVelocity", entityID, r);
        }
        outVelocity = body->GetAngularVelocity();
        return Result::Ok;
    }

    Result SetAngularVelocity(Scene& scene, UUID entityID, const glm::vec3& velocity)
    {
        if (!IsFinite(velocity))
        {
            return Report("SetAngularVelocity", entityID, Result::NotFinite);
        }
        Ref<JoltBody> body;
        if (const Result r = ResolveBody(scene, entityID, body); r != Result::Ok)
        {
            return Report("SetAngularVelocity", entityID, r);
        }
        if (body->IsStatic())
        {
            return Report("SetAngularVelocity", entityID, Result::NotDynamic);
        }
        body->SetAngularVelocity(velocity);
        return Result::Ok;
    }

    Result SetBodyType(Scene& scene, UUID entityID, EBodyType type)
    {
        if (type != EBodyType::Static && type != EBodyType::Dynamic && type != EBodyType::Kinematic)
        {
            return Report("SetBodyType", entityID, Result::InvalidArgument);
        }
        Ref<JoltBody> body;
        if (const Result r = ResolveBody(scene, entityID, body); r != Result::Ok)
        {
            return Report("SetBodyType", entityID, r);
        }
        if (type != EBodyType::Static && !body->CanBecomeDynamic())
        {
            return Report("SetBodyType", entityID, Result::Unsupported);
        }
        body->SetBodyType(type);
        return Result::Ok;
    }

    Result GetContacts(Scene& scene, UUID entityID, TArray<UUID>& outContacts)
    {
        outContacts.Reset();
        JoltScene* physics = scene.GetPhysicsScene();
        if (physics == nullptr || !physics->IsInitialized())
        {
            return Report("GetContacts", entityID, Result::NoScene);
        }
        if (!scene.TryGetEntityWithUUID(entityID))
        {
            return Report("GetContacts", entityID, Result::NoEntity);
        }
        physics->GetContacts(entityID, outContacts);
        return Result::Ok;
    }

    Result AttachTo(Scene& scene, UUID entityID, UUID parentID, std::string_view boneName,
                    const glm::vec3& localTranslation, const glm::quat& localRotation)
    {
        if (!IsFinite(localTranslation) || !IsFinite(localRotation))
        {
            return Report("AttachTo", entityID, Result::NotFinite);
        }
        const auto child = scene.TryGetEntityWithUUID(entityID);
        const auto parent = scene.TryGetEntityWithUUID(parentID);
        if (!child || !parent)
        {
            return Report("AttachTo", entityID, Result::NoEntity);
        }
        const glm::quat rotation = glm::length(localRotation) > 0.0f ? glm::normalize(localRotation) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (!scene.AttachEntity(*child, *parent, boneName, localTranslation, rotation))
        {
            return Report("AttachTo", entityID, Result::InvalidArgument);
        }
        return Result::Ok;
    }

    Result AttachInPlace(Scene& scene, UUID entityID, UUID parentID, std::string_view boneName)
    {
        const auto child = scene.TryGetEntityWithUUID(entityID);
        const auto parent = scene.TryGetEntityWithUUID(parentID);
        if (!child || !parent)
        {
            return Report("AttachInPlace", entityID, Result::NoEntity);
        }
        if (!scene.AttachEntityInPlace(*child, *parent, boneName))
        {
            return Report("AttachInPlace", entityID, Result::InvalidArgument);
        }
        return Result::Ok;
    }

    Result Detach(Scene& scene, UUID entityID)
    {
        const auto child = scene.TryGetEntityWithUUID(entityID);
        if (!child)
        {
            return Report("Detach", entityID, Result::NoEntity);
        }
        if (!scene.DetachEntity(*child))
        {
            return Report("Detach", entityID, Result::InvalidArgument);
        }
        return Result::Ok;
    }
} // namespace OloEngine::Physics3DScriptApi
