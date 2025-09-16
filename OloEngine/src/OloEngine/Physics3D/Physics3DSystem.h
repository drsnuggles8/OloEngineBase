#pragma once

#include "OloEngine/Core/Base.h"
#include <memory>

// Jolt includes
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>

// STL includes
#include <iostream>
#include <cstdarg>
#include <thread>

// Disable common warnings triggered by Jolt
JPH_SUPPRESS_WARNINGS

namespace OloEngine {

    /// Layer that objects can be in, determines which other objects it can collide with
    /// Typically you at least want to have 1 layer for moving objects and 1 layer for static objects
    namespace Layers
    {
        static constexpr JPH::ObjectLayer NON_MOVING = 0;
        static constexpr JPH::ObjectLayer MOVING = 1;
        static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
    };

    /// Class that determines if two object layers can collide
    class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
    {
    public:
        virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
        {
            switch (inObject1)
            {
            case Layers::NON_MOVING:
                return inObject2 == Layers::MOVING; // Non moving only collides with moving
            case Layers::MOVING:
                return true; // Moving collides with everything
            default:
                JPH_ASSERT(false);
                return false;
            }
        }
    };

    /// Each broadphase layer results in a separate bounding volume tree in the broad phase. You at least want to have
    /// a layer for non-moving and moving objects to avoid having to update a tree full of static objects every frame.
    /// You can have a 1-on-1 mapping between object layers and broadphase layers (like in this example) but you can
    /// also group multiple object layers that have the same collision behavior into a single broadphase layer.
    namespace BroadPhaseLayers
    {
        static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
        static constexpr JPH::BroadPhaseLayer MOVING(1);
        static constexpr JPH::uint NUM_LAYERS(2);
    };

    /// BroadPhaseLayerInterface implementation
    /// This defines a mapping between object and broadphase layers.
    class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        BPLayerInterfaceImpl()
        {
            // Create a mapping table from object to broad phase layer
            m_ObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
            m_ObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
        }

        virtual JPH::uint GetNumBroadPhaseLayers() const override
        {
            return BroadPhaseLayers::NUM_LAYERS;
        }

        virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
        {
            JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
            return m_ObjectToBroadPhase[inLayer];
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
        {
            switch ((JPH::BroadPhaseLayer::Type)inLayer)
            {
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:	return "NON_MOVING";
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:		return "MOVING";
            default:													JPH_ASSERT(false); return "INVALID";
            }
        }
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

    private:
        JPH::BroadPhaseLayer m_ObjectToBroadPhase[Layers::NUM_LAYERS];
    };

    /// Class that determines if an object layer can collide with a broadphase layer
    class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
        {
            switch (inLayer1)
            {
            case Layers::NON_MOVING:
                return inLayer2 == BroadPhaseLayers::MOVING;
            case Layers::MOVING:
                return true;
            default:
                JPH_ASSERT(false);
                return false;
            }
        }
    };

    /// A body activation listener gets notified when bodies activate and go to sleep
    /// Note that this is called from a job so whatever you do here needs to be thread safe.
    /// Registering one is entirely optional.
    class MyBodyActivationListener : public JPH::BodyActivationListener
    {
    public:
        virtual void OnBodyActivated(const JPH::BodyID& inBodyID, JPH::uint64 inBodyUserData) override
        {
            OLO_CORE_INFO("A body got activated");
        }

        virtual void OnBodyDeactivated(const JPH::BodyID& inBodyID, JPH::uint64 inBodyUserData) override
        {
            OLO_CORE_INFO("A body went to sleep");
        }
    };

    /// A contact listener gets notified when bodies (are about to) collide, and when they separate again.
    /// Note that this is called from a job so whatever you do here needs to be thread safe.
    /// Registering one is entirely optional.
    class MyContactListener : public JPH::ContactListener
    {
    public:
        // See: ContactListener
        virtual JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult& inCollisionResult) override
        {
            OLO_CORE_INFO("Contact validate callback");

            // Allows you to ignore a contact before it is created (using layers to not make objects collide is cheaper!)
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }

        virtual void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override
        {
            OLO_CORE_INFO("A contact was added");
        }

        virtual void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override
        {
            OLO_CORE_INFO("A contact was persisted");
        }

        virtual void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override
        {
            OLO_CORE_INFO("A contact was removed");
        }
    };

    class Physics3DSystem
    {
    public:
        Physics3DSystem();
        ~Physics3DSystem();

        // Initialize the physics system
        bool Initialize();
        
        // Shutdown the physics system
        void Shutdown();

        // Step the physics simulation
        void Update(f32 deltaTime);

        // Create a box body
        JPH::BodyID CreateBox(const JPH::RVec3& position, const JPH::Quat& rotation, const JPH::Vec3& halfExtent, bool isStatic = false);
        
        // Create a sphere body
        JPH::BodyID CreateSphere(const JPH::RVec3& position, f32 radius, bool isStatic = false);

        // Remove a body
        void RemoveBody(JPH::BodyID bodyID);

        // Get body interface for direct manipulation
        JPH::BodyInterface& GetBodyInterface() { return m_PhysicsSystem->GetBodyInterface(); }
        const JPH::BodyInterface& GetBodyInterface() const { return m_PhysicsSystem->GetBodyInterface(); }

        // Get the physics system for direct access
        JPH::PhysicsSystem* GetPhysicsSystem() { return m_PhysicsSystem.get(); }

    private:
        // Maximum number of bodies in the physics system
        static constexpr u32 cMaxBodies = 65536;

        // Maximum number of body pairs that can be queued at any time
        static constexpr u32 cMaxBodyPairs = 65536;

        // Maximum number of contact constraints that can be queued at any time
        static constexpr u32 cMaxContactConstraints = 10240;

        // Number of mutexes to allocate to protect rigid bodies from concurrent access
        static constexpr u32 cNumBodyMutexes = 0;

        // Create mapping table from object layer to broadphase layer
        BPLayerInterfaceImpl m_BroadPhaseLayerInterface;
        ObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBroadPhaseLayerFilter;
        ObjectLayerPairFilterImpl m_ObjectLayerPairFilter;

        // The physics system
        std::unique_ptr<JPH::PhysicsSystem> m_PhysicsSystem;

        // A temp allocator
        std::unique_ptr<JPH::TempAllocatorImpl> m_TempAllocator;

        // Job system that will execute physics jobs
        std::unique_ptr<JPH::JobSystemThreadPool> m_JobSystem;

        // Listeners
        MyBodyActivationListener m_BodyActivationListener;
        MyContactListener m_ContactListener;

        bool m_Initialized = false;
    };

} // namespace OloEngine