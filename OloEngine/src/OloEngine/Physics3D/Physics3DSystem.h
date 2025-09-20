#pragma once

#include "OloEngine/Core/Base.h"
#include "PhysicsSettings.h"
#include "PhysicsLayer.h"
#include <memory>
#include <stdexcept>
#include <cassert>

// Forward declarations
namespace OloEngine {
    // Class forward declarations can go here if needed
}

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
    /// Now integrated with PhysicsLayerManager for dynamic layer configuration
    class OloObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
    {
    public:
        virtual bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override;
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
    /// Now integrated with PhysicsLayerManager for dynamic layer configuration
    class OloBPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        OloBPLayerInterfaceImpl();
        void UpdateLayers(); // Update layer mappings when PhysicsLayerManager changes

        virtual JPH::uint GetNumBroadPhaseLayers() const override;
        virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override;

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override;
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

    private:
        static constexpr JPH::uint MAX_LAYERS = 32; // Maximum supported physics layers
        JPH::BroadPhaseLayer m_ObjectToBroadPhase[MAX_LAYERS];
        JPH::uint m_NumLayers = 2; // Start with default layers
    };

    /// Class that determines if an object layer can collide with a broadphase layer
    /// Now integrated with PhysicsLayerManager for dynamic layer configuration
    class OloObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override;
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
        
        // Singleton pattern enforcement - prevent copying
        Physics3DSystem(const Physics3DSystem&) = delete;
        Physics3DSystem& operator=(const Physics3DSystem&) = delete;
        Physics3DSystem(Physics3DSystem&&) = delete;
        Physics3DSystem& operator=(Physics3DSystem&&) = delete;

        // Initialize the physics system
        bool Initialize();
        
        // Shutdown the physics system
        void Shutdown();

        // Step the physics simulation
        void Update(f32 deltaTime);

        // Settings management
        static PhysicsSettings& GetSettings() { return s_PhysicsSettings; }
        static void SetSettings(const PhysicsSettings& settings);
        static void ApplySettings(); // Apply current settings to the physics system

        // Layer management
        static void UpdateLayerConfiguration(); // Update layer configuration when PhysicsLayerManager changes

        // Create a box body
        JPH::BodyID CreateBox(const JPH::RVec3& position, const JPH::Quat& rotation, const JPH::Vec3& halfExtent, bool isStatic = false);
        
        // Create a sphere body
        JPH::BodyID CreateSphere(const JPH::RVec3& position, f32 radius, bool isStatic = false);

        // Remove a body
        void RemoveBody(JPH::BodyID bodyID);

        // Get body interface for direct manipulation
        JPH::BodyInterface& GetBodyInterface() 
        { 
            if (!m_PhysicsSystem) 
                throw std::runtime_error("PhysicsSystem not initialized in Physics3DSystem::GetBodyInterface");
            return m_PhysicsSystem->GetBodyInterface(); 
        }
        const JPH::BodyInterface& GetBodyInterface() const 
        { 
            if (!m_PhysicsSystem) 
                throw std::runtime_error("PhysicsSystem not initialized in Physics3DSystem::GetBodyInterface");
            return m_PhysicsSystem->GetBodyInterface(); 
        }

        // Get the physics system for direct access
        JPH::PhysicsSystem* GetPhysicsSystem() { return m_PhysicsSystem.get(); }

        // Static access to the Jolt system (for utilities)
        static JPH::PhysicsSystem& GetJoltSystem();

    private:
        // Physics settings - now configurable instead of hard-coded
        inline static PhysicsSettings s_PhysicsSettings;

        // Static access to current instance for settings application
        inline static Physics3DSystem* s_Instance = nullptr;

        // Static access to layer interfaces for global layer management
        inline static OloBPLayerInterfaceImpl* s_BroadPhaseLayerInterface = nullptr;

        // Create mapping table from object layer to broadphase layer
        OloBPLayerInterfaceImpl m_BroadPhaseLayerInterface;
        OloObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBroadPhaseLayerFilter;
        OloObjectLayerPairFilterImpl m_ObjectLayerPairFilter;

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

        // Helper methods
        void UpdatePhysicsSystemSettings();

        // Number of mutexes to allocate to protect rigid bodies from concurrent access
        // Can be overridden at compile time via -DOLO_PHYSICS_BODY_MUTEXES=N
        #ifndef OLO_PHYSICS_BODY_MUTEXES
        static constexpr u32 cNumBodyMutexes = 8; // Default: 8 mutexes for reasonable concurrency protection
        #else
        static constexpr u32 cNumBodyMutexes = OLO_PHYSICS_BODY_MUTEXES;
        #endif
    };

} // namespace OloEngine