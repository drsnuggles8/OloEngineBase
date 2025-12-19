#pragma once

// Standard library includes
#include <functional>
#include <unordered_set>

// Project includes
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Scene/Entity.h"
#include "Physics3DTypes.h"
#include "JoltUtils.h"

// Third-party includes
#include <Jolt/Core/Reference.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
    // Forward declarations
    class JoltScene;

    // Collision flags for character controller
    enum class ECollisionFlags : u8
    {
        None  = 0,
        Sides = OloBit8(0),
        Above = OloBit8(1),
        Below = OloBit8(2),
    };

    // @brief Jolt Physics character controller implementation
    // 
    // Provides character movement with proper collision detection, gravity,
    // jumping, and air control. Uses Jolt's CharacterVirtual system for
    // smooth movement and ground detection.
    class JoltCharacterController : public RefCounted, public JPH::CharacterContactListener
    {
    public:
        JoltCharacterController(Entity entity, JoltScene* scene, const ContactCallbackFn& contactCallback = nullptr);
        ~JoltCharacterController();

        // Gravity control
        void SetGravityEnabled(bool enableGravity) { m_HasGravity = enableGravity; }
        bool IsGravityEnabled() const { return m_HasGravity; }

        // Movement constraints
        void SetSlopeLimit(f32 slopeLimit);
        void SetStepOffset(f32 stepOffset);

        // Position and rotation (instant teleport)
        void SetTranslation(const glm::vec3& translation);
        void SetRotation(const glm::quat& rotation);
        glm::vec3 GetTranslation() const;
        glm::quat GetRotation() const;

        // Ground detection
        bool IsGrounded() const;

        // Air control settings
        void SetControlMovementInAir(bool controlMovementInAir) { m_ControlMovementInAir = controlMovementInAir; }
        bool CanControlMovementInAir() const { return m_ControlMovementInAir; }
        void SetControlRotationInAir(bool controlRotationInAir) { m_ControlRotationInAir = controlRotationInAir; }
        bool CanControlRotationInAir() const { return m_ControlRotationInAir; }

        // Collision information
        ECollisionFlags GetCollisionFlags() const { return m_CollisionFlags; }

        // Movement functions (incremental during physics simulation)
        void Move(const glm::vec3& displacement);
        void Rotate(const glm::quat& rotation);
        void Jump(f32 jumpPower);

        // Velocity control
        glm::vec3 GetLinearVelocity() const;
        void SetLinearVelocity(const glm::vec3& linearVelocity);
        glm::vec3 GetAngularVelocity() const;
        void SetAngularVelocity(const glm::vec3& angularVelocity);

        // Shape and collision layer
        void SetShape(const JPH::Ref<JPH::Shape>& shape);
        void SetCollisionLayer(u32 collisionLayer) { m_CollisionLayer = collisionLayer; }
        u32 GetCollisionLayer() const { return m_CollisionLayer; }

        // Collision filtering
        void SetIgnoreCollisionLayers(u32 layerMask) { m_IgnoreCollisionLayers = layerMask; }
        u32 GetIgnoreCollisionLayers() const { return m_IgnoreCollisionLayers; }

        // Internal Jolt access
        JPH::BodyID GetBodyID() const { return m_Controller ? m_Controller->GetInnerBodyID() : JPH::BodyID(); }
        JPH::CharacterVirtual* GetJoltController() const { return m_Controller.GetPtr(); }

    private:
        // Physics simulation callbacks
        void PreSimulate(f32 deltaTime);
        void Simulate(f32 deltaTime);
        void PostSimulate();

        void Create();
        bool UpdateShape();

        // JPH::CharacterContactListener interface
        void OnAdjustBodyVelocity(const JPH::CharacterVirtual* inCharacter, const JPH::Body& inBody2, 
                                 JPH::Vec3& ioLinearVelocity, JPH::Vec3& ioAngularVelocity) override;
        bool OnContactValidate(const JPH::CharacterVirtual* inCharacter, const JPH::BodyID& inBodyID2, 
                              const JPH::SubShapeID& inSubShapeID2) override;
        void OnContactAdded(const JPH::CharacterVirtual* inCharacter, const JPH::BodyID& inBodyID2, 
                           const JPH::SubShapeID& inSubShapeID2, JPH::Vec3Arg inContactPosition, 
                           JPH::Vec3Arg inContactNormal, JPH::CharacterContactSettings& ioSettings) override;
        void OnContactSolve(const JPH::CharacterVirtual* inCharacter, const JPH::BodyID& inBodyID2, 
                           const JPH::SubShapeID& inSubShapeID2, JPH::RVec3Arg inContactPosition, 
                           JPH::Vec3Arg inContactNormal, JPH::Vec3Arg inContactVelocity, 
                           const JPH::PhysicsMaterial* inContactMaterial, JPH::Vec3Arg inCharacterVelocity, 
                           JPH::Vec3& ioNewCharacterVelocity) override;

        // Contact event handling
        void HandleTrigger(const JPH::BodyID bodyID2);
        void HandleCollision(const JPH::BodyID bodyID2);

        // PreSimulate helper methods
        JPH::Vec3 CalculateDesiredVelocity(f32 deltaTime) const;
        JPH::Vec3 ApplyGravityAndJump(f32 deltaTime, const JPH::Vec3& desiredVelocity);
        void UpdateRotation(f32 deltaTime);

    private:
        // Entity reference
        Entity m_Entity;
        
        // Scene reference
        JoltScene* m_Scene;

        // Contact callback
        ContactCallbackFn m_ContactEventCallback;

        // Jolt character controller
        JPH::Ref<JPH::CharacterVirtual> m_Controller;
        JPH::Ref<JPH::Shape> m_Shape;

        // Movement state
        JPH::Quat m_Rotation = JPH::Quat::sIdentity();          // Rotation to apply next update
        JPH::Vec3 m_Displacement = JPH::Vec3::sZero();          // Displacement to apply next update
        JPH::Vec3 m_LinearVelocity = JPH::Vec3::sZero();        // Linear velocity to apply next update
        JPH::Vec3 m_AngularVelocityIn = JPH::Vec3::sZero();     // Angular velocity to apply next update
        JPH::Vec3 m_AngularVelocityOut = JPH::Vec3::sZero();    // Angular velocity after physics update
        JPH::Quat m_PreviousRotation = JPH::Quat::sIdentity();

        // Contact tracking - using unordered_set for O(1) membership checks
        std::unordered_set<JPH::BodyID> m_TriggeredBodies;
        std::unordered_set<JPH::BodyID> m_StillTriggeredBodies;
        std::unordered_set<JPH::BodyID> m_CollidedBodies;
        std::unordered_set<JPH::BodyID> m_StillCollidedBodies;

        // Character properties
        f32 m_JumpPower = 0.0f;
        bool m_JumpRequested = false;
        f32 m_StepOffset = 0.4f;
        f32 m_AngularVelocityDeltaTime = 0.0f;

        u32 m_CollisionLayer = 0;
        u32 m_IgnoreCollisionLayers = 0;
        ECollisionFlags m_CollisionFlags = ECollisionFlags::None;

        // Settings
        bool m_HasGravity = true;
        bool m_ControlMovementInAir = false;
        bool m_ControlRotationInAir = false;

        friend class JoltScene;
    };

} // namespace OloEngine
