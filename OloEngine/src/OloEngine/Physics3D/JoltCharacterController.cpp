#include "OloEnginePCH.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltShapes.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Core/Log.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

namespace OloEngine
{
    JoltCharacterController::JoltCharacterController(Entity entity, JoltScene* scene, const ContactCallbackFn& contactCallback)
        : m_Entity(entity), m_Scene(scene), m_ContactEventCallback(contactCallback)
    {
        Create();
    }

    JoltCharacterController::~JoltCharacterController()
    {
        // Make sure controller is destroyed before the rest of the class (in particular before m_Shape)
        if (m_Controller)
        {
            m_Controller = nullptr;
        }
    }

    void JoltCharacterController::SetSlopeLimit(f32 slopeLimit)
    {
        if (m_Controller)
        {
            m_Controller->SetMaxSlopeAngle(glm::radians(slopeLimit));
        }
    }

    void JoltCharacterController::SetStepOffset(f32 stepOffset)
    {
        m_StepOffset = stepOffset;
    }

    void JoltCharacterController::SetTranslation(const glm::vec3& translation)
    {
        if (m_Controller)
        {
            m_Controller->SetPosition(JoltUtils::ToJoltVector(translation));
        }
    }

    void JoltCharacterController::SetRotation(const glm::quat& rotation)
    {
        if (m_Controller)
        {
            m_Controller->SetRotation(JoltUtils::ToJoltQuat(rotation));
        }
    }

    glm::vec3 JoltCharacterController::GetTranslation() const
    {
        if (m_Controller)
        {
            return JoltUtils::FromJoltVector(m_Controller->GetPosition());
        }
        return glm::vec3(0.0f);
    }

    glm::quat JoltCharacterController::GetRotation() const
    {
        if (m_Controller)
        {
            return JoltUtils::FromJoltQuat(m_Controller->GetRotation());
        }
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    bool JoltCharacterController::IsGrounded() const
    {
        return m_Controller ? m_Controller->IsSupported() : false;
    }

    void JoltCharacterController::Move(const glm::vec3& displacement)
    {
        if (IsGrounded() || m_ControlMovementInAir)
        {
            m_Displacement += JoltUtils::ToJoltVector(displacement);
        }
    }

    void JoltCharacterController::Rotate(const glm::quat& rotation)
    {
        // Avoid quat multiplication if rotation is close to identity
        if ((IsGrounded() || m_ControlRotationInAir) && fabs(rotation.w - 1.0f) > 0.000001f)
        {
            m_Rotation = m_Rotation * JoltUtils::ToJoltQuat(rotation);
        }
    }

    void JoltCharacterController::Jump(f32 jumpPower)
    {
        m_JumpPower = jumpPower;
    }

    glm::vec3 JoltCharacterController::GetLinearVelocity() const
    {
        if (m_Controller)
        {
            return JoltUtils::FromJoltVector(m_Controller->GetLinearVelocity());
        }
        return glm::vec3(0.0f);
    }

    void JoltCharacterController::SetLinearVelocity(const glm::vec3& linearVelocity)
    {
        // This is what we would like the velocity to be, and we will try to
        // set it to this at next physics update.
        m_LinearVelocity = JoltUtils::ToJoltVector(linearVelocity);
    }

    glm::vec3 JoltCharacterController::GetAngularVelocity() const
    {
        return JoltUtils::FromJoltVector(m_AngularVelocityOut);
    }

    void JoltCharacterController::SetAngularVelocity(const glm::vec3& angularVelocity)
    {
        // This is what we would like the angular velocity to be, and we will try to
        // set it to this at next physics update.
        m_AngularVelocityIn = JoltUtils::ToJoltVector(angularVelocity);
    }

    void JoltCharacterController::SetShape(const JPH::Ref<JPH::Shape>& shape)
    {
        m_Shape = shape;
        UpdateShape();
    }

    void JoltCharacterController::PreSimulate(f32 deltaTime)
    {
        if (!m_Controller || deltaTime <= 0.0f)
            return;

        m_Controller->UpdateGroundVelocity();

        JPH::Vec3 desiredVelocity = m_LinearVelocity + m_Displacement / deltaTime;
        JPH::Vec3 currentVerticalVelocity = JPH::Vec3(0, m_Controller->GetLinearVelocity().GetY(), 0);

        JPH::Vec3 newVelocity;
        if (IsGravityEnabled())
        {
            if (m_Controller->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround && 
                (!m_Controller->IsSlopeTooSteep(m_Controller->GetGroundNormal())))
            {
                // When grounded, acquire velocity of ground
                newVelocity = m_Controller->GetGroundVelocity();

                // Jump
                bool jumping = (currentVerticalVelocity.GetY() - newVelocity.GetY()) >= 0.1f;
                if (m_JumpPower > 0.0f && !jumping)
                {
                    newVelocity += JPH::Vec3(0, m_JumpPower, 0);
                    m_JumpPower = 0.0f;
                }
            }
            else
            {
                // Apply gravity when not grounded or on steep slope
                glm::vec3 gravity = glm::vec3(0.0f, -9.81f, 0.0f); // Default gravity, should get from scene
                newVelocity = currentVerticalVelocity + JoltUtils::ToJoltVector(gravity) * deltaTime;
            }
        }
        else
        {
            newVelocity = JPH::Vec3::sZero();
        }

        if (IsGrounded() || m_ControlMovementInAir)
        {
            newVelocity += desiredVelocity;
        }
        else
        {
            // Preserve current horizontal velocity
            JPH::Vec3 currentHorizontalVelocity = m_Controller->GetLinearVelocity() - currentVerticalVelocity;
            newVelocity += currentHorizontalVelocity;
        }

        m_AngularVelocityDeltaTime = deltaTime;
        m_PreviousRotation = m_Controller->GetRotation();

        m_Controller->SetLinearVelocity(newVelocity);

        if (m_AngularVelocityIn.LengthSq() < 0.000001f)
        {
            if (fabs(m_Rotation.GetW() - 1.0f) > 0.000001f)
            {
                m_Controller->SetRotation((m_Controller->GetRotation() * m_Rotation).Normalized());
            }
        }
        else
        {
            JPH::Vec3 axis = m_AngularVelocityIn.Normalized();
            f32 angle = m_AngularVelocityIn.Length() * deltaTime;
            JPH::Quat scaledRotation = JPH::Quat::sRotation(axis, angle);
            m_Controller->SetRotation((m_Controller->GetRotation() * m_Rotation * scaledRotation).Normalized());
        }

        m_AllowSliding = !IsGrounded() || !desiredVelocity.IsNearZero();
    }

    void JoltCharacterController::Simulate(f32 deltaTime)
    {
        if (!m_Controller || !m_Scene)
            return;

        // Get gravity from the scene
        glm::vec3 gravity = m_Scene->GetGravity();
        JPH::Vec3 joltGravity = JoltUtils::ToJoltVector(gravity);

        // Get physics system for layer filters
        JPH::PhysicsSystem* physicsSystem = m_Scene->GetPhysicsSystem();
        if (!physicsSystem)
            return;

        // Get proper layer filters from physics system
        JPH::ObjectLayer layer = JPH::ObjectLayer(m_CollisionLayer);
        JPH::DefaultBroadPhaseLayerFilter broadPhaseLayerFilter = physicsSystem->GetDefaultBroadPhaseLayerFilter(layer);
        JPH::DefaultObjectLayerFilter objectLayerFilter = physicsSystem->GetDefaultLayerFilter(layer);

        // Use temp allocator from physics system
        JPH::TempAllocatorImpl tempAllocator(10 * 1024 * 1024);

        // Perform character update with step-up functionality
        JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
        updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, m_StepOffset, 0.0f);
        updateSettings.mWalkStairsStepForwardTest = m_Controller->GetShape()->GetInnerRadius();

        m_Controller->ExtendedUpdate(deltaTime, joltGravity, updateSettings, broadPhaseLayerFilter, objectLayerFilter, {}, {}, tempAllocator);
    }

    void JoltCharacterController::PostSimulate()
    {
        if (!m_Controller)
            return;

        if (IsGrounded() || m_ControlMovementInAir)
        {
            m_Displacement = JPH::Vec3::sZero();
        }
        if (IsGrounded() || m_ControlRotationInAir)
        {
            m_Rotation = JPH::Quat::sIdentity();
        }

        // Compute angular velocity (Jolt character virtual does not provide this)
        if (m_AngularVelocityDeltaTime > 0.0f)
        {
            JPH::Quat rotation = m_Controller->GetRotation() * m_PreviousRotation.Conjugated();
            if (rotation.IsClose(JPH::Quat::sIdentity()))
            {
                m_AngularVelocityOut = JPH::Vec3::sZero();
            }
            else
            {
                // Note: this cannot account for more than one complete revolution in a single frame
                JPH::Vec3 axis;
                f32 angle;
                rotation.GetAxisAngle(axis, angle);
                m_AngularVelocityOut = axis * angle / m_AngularVelocityDeltaTime;
            }
        }

        // Handle contact events
        if (m_ContactEventCallback)
        {
            // Process trigger end events
            for (const auto& bodyID : m_TriggeredBodies)
            {
                if (std::find(m_StillTriggeredBodies.begin(), m_StillTriggeredBodies.end(), bodyID) == m_StillTriggeredBodies.end())
                {
                    // Trigger end event - would need entity lookup from bodyID
                    // m_ContactEventCallback(m_Entity, otherEntity);
                }
            }

            // Process collision end events
            for (const auto& bodyID : m_CollidedBodies)
            {
                if (std::find(m_StillCollidedBodies.begin(), m_StillCollidedBodies.end(), bodyID) == m_StillCollidedBodies.end())
                {
                    // Collision end event - would need entity lookup from bodyID
                    // m_ContactEventCallback(m_Entity, otherEntity);
                }
            }

            std::swap(m_TriggeredBodies, m_StillTriggeredBodies);
            std::swap(m_CollidedBodies, m_StillCollidedBodies);

            m_StillTriggeredBodies.clear();
            m_StillCollidedBodies.clear();
        }
    }

    void JoltCharacterController::Create()
    {
        // Create character controller settings
        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mMaxSlopeAngle = glm::radians(45.0f); // Default 45 degree slope limit
        
        // Create a default capsule shape if no shape is specified
        if (!m_Shape)
        {
            // Default capsule: radius 0.5, height 2.0
            m_Shape = new JPH::CapsuleShape(1.0f, 0.5f);
        }

        settings->mShape = m_Shape;
        settings->mInnerBodyShape = m_Shape; // Required for character vs character collision

        // Get initial transform from entity (would need component system integration)
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        // TODO: Create the character controller with proper physics system integration
        // For now, we'll create a placeholder that can be properly initialized later
        // m_Controller = new JPH::CharacterVirtual(settings, JoltUtils::ToJoltVector(position), JoltUtils::ToJoltQuat(rotation), &physicsSystem);
        
        // Set this as the contact listener when controller is created
        // if (m_Controller)
        // {
        //     m_Controller->SetListener(this);
        // }

        OLO_CORE_INFO("Character controller created for entity (awaiting physics system integration)");
    }

    void JoltCharacterController::UpdateShape()
    {
        if (m_Controller && m_Shape)
        {
            // Note: Jolt doesn't support changing shapes after creation
            // Would need to recreate the controller
            OLO_CORE_WARN("Character controller shape update not implemented - requires recreation");
        }
    }

    void JoltCharacterController::HandleTrigger(const JPH::BodyID bodyID2)
    {
        if (std::find(m_TriggeredBodies.begin(), m_TriggeredBodies.end(), bodyID2) == m_TriggeredBodies.end())
        {
            // Trigger begin event - would need entity lookup from bodyID
            if (m_ContactEventCallback)
            {
                // m_ContactEventCallback(m_Entity, otherEntity);
            }
        }
        m_StillTriggeredBodies.push_back(bodyID2);
    }

    void JoltCharacterController::HandleCollision(const JPH::BodyID bodyID2)
    {
        if (std::find(m_CollidedBodies.begin(), m_CollidedBodies.end(), bodyID2) == m_CollidedBodies.end())
        {
            // Collision begin event - would need entity lookup from bodyID
            if (m_ContactEventCallback)
            {
                // m_ContactEventCallback(m_Entity, otherEntity);
            }
        }
        m_StillCollidedBodies.push_back(bodyID2);
    }

    // JPH::CharacterContactListener implementation
    void JoltCharacterController::OnAdjustBodyVelocity(const JPH::CharacterVirtual* inCharacter, const JPH::Body& inBody2, 
                                                      JPH::Vec3& ioLinearVelocity, JPH::Vec3& ioAngularVelocity)
    {
        // TODO: Marshal this call out to gameplay and get result back for contacted inBody2
    }

    bool JoltCharacterController::OnContactValidate(const JPH::CharacterVirtual* inCharacter, const JPH::BodyID& inBodyID2, 
                                                   const JPH::SubShapeID& inSubShapeID2)
    {
        // For now, allow all contacts - can add layer filtering later
        return true;
    }

    void JoltCharacterController::OnContactAdded(const JPH::CharacterVirtual* inCharacter, const JPH::BodyID& inBodyID2, 
                                               const JPH::SubShapeID& inSubShapeID2, JPH::Vec3Arg inContactPosition, 
                                               JPH::Vec3Arg inContactNormal, JPH::CharacterContactSettings& ioSettings)
    {
        m_CollisionFlags = ECollisionFlags::None;

        // Determine collision flags based on contact normal
        JPH::Vec3 up = JPH::Vec3(0, 1, 0);
        f32 dotUp = inContactNormal.Dot(up);
        
        if (dotUp > 0.7f) // Roughly 45 degrees
        {
            m_CollisionFlags = static_cast<ECollisionFlags>(static_cast<u8>(m_CollisionFlags) | static_cast<u8>(ECollisionFlags::Below));
        }
        else if (dotUp < -0.7f)
        {
            m_CollisionFlags = static_cast<ECollisionFlags>(static_cast<u8>(m_CollisionFlags) | static_cast<u8>(ECollisionFlags::Above));
        }
        else
        {
            m_CollisionFlags = static_cast<ECollisionFlags>(static_cast<u8>(m_CollisionFlags) | static_cast<u8>(ECollisionFlags::Sides));
        }

        // Check if it's a sensor/trigger
        bool isSensor = false;
        bool isStatic = true;
        
        // Would need physics system integration to properly check body properties
        // For now, assume it's a solid collision
        
        if (isSensor)
        {
            HandleTrigger(inBodyID2);
        }
        else
        {
            HandleCollision(inBodyID2);
        }
    }

    void JoltCharacterController::OnContactSolve(const JPH::CharacterVirtual* inCharacter, const JPH::BodyID& inBodyID2, 
                                               const JPH::SubShapeID& inSubShapeID2, JPH::RVec3Arg inContactPosition, 
                                               JPH::Vec3Arg inContactNormal, JPH::Vec3Arg inContactVelocity, 
                                               const JPH::PhysicsMaterial* inContactMaterial, JPH::Vec3Arg inCharacterVelocity, 
                                               JPH::Vec3& ioNewCharacterVelocity)
    {
        // Default implementation - no velocity modification
        // This can be extended to handle special materials, moving platforms, etc.
    }

} // namespace OloEngine