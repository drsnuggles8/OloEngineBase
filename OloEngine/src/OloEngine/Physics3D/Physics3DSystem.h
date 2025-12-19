#pragma once

#include "OloEngine/Core/Base.h"
#include "PhysicsSettings.h"
#include "PhysicsLayer.h"
#include "JoltLayerInterface.h"
#include <stdexcept>
#include <cassert>

// Forward declarations
namespace OloEngine {
    // Forward declarations to reduce compile dependencies
    class OloBPLayerInterfaceImpl;
    class OloObjectVsBroadPhaseLayerFilterImpl;
    class PhysicsBodyActivationListener;
    class JoltPhysicsSystemContactListener;
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

#include <memory>
#include <atomic>
#include <mutex>
#include <queue>

// Disable common warnings triggered by Jolt
JPH_SUPPRESS_WARNINGS

namespace OloEngine {

    /// Each broadphase layer results in a separate bounding volume tree in the broad phase. You at least want to have
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
        static constexpr JPH::uint sMaxLayers = 32; // Maximum supported physics layers
        JPH::BroadPhaseLayer m_ObjectToBroadPhase[sMaxLayers] = {};
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
    class PhysicsBodyActivationListener : public JPH::BodyActivationListener
    {
    public:
        struct ActivationEvent
        {
            enum Type { Activated, Deactivated };
            Type EventType;
            JPH::BodyID BodyID;
            JPH::uint64 UserData;
        };

        virtual void OnBodyActivated(const JPH::BodyID& inBodyID, JPH::uint64 inBodyUserData) override;
        virtual void OnBodyDeactivated(const JPH::BodyID& inBodyID, JPH::uint64 inBodyUserData) override;

        // Process queued events on main thread
        void ProcessEvents();

        // Get the number of pending events
        sizet GetPendingEventCount() const noexcept;

    private:
        void EnqueueEvent(const ActivationEvent& event)
        {
            std::scoped_lock lock(m_QueueMutex);
            m_EventQueue.push(event);
            m_QueueSize.fetch_add(1, std::memory_order_relaxed);
        }

        bool TryDequeueEvent(ActivationEvent& outEvent)
        {
            std::scoped_lock lock(m_QueueMutex);
            if (m_EventQueue.empty())
                return false;

            outEvent = m_EventQueue.front();
            m_EventQueue.pop();
            m_QueueSize.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }

        mutable std::mutex m_QueueMutex;
        std::queue<ActivationEvent> m_EventQueue;
        std::atomic<sizet> m_QueueSize{ 0 };
    };

    /// A contact listener gets notified when bodies (are about to) collide, and when they separate again.
    /// Note that this is called from a job so whatever you do here needs to be thread safe.
    /// Registering one is entirely optional.
    class JoltPhysicsSystemContactListener : public JPH::ContactListener
    {
    public:
        // See: ContactListener
        virtual JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult& inCollisionResult) override;

        virtual void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override;

        virtual void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override;

        virtual void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override;
    };

    class Physics3DSystem
    {
    public:
        // Singleton creation and destruction methods
        static void CreateInstance()
        {
            std::lock_guard<std::mutex> lock(s_InstanceMutex);
            if (s_Instance != nullptr)
            {
                throw std::runtime_error("Physics3DSystem: Instance already exists - cannot create multiple instances");
            }
            s_Instance = new Physics3DSystem();
        }

        static void DestroyInstance()
        {
            std::lock_guard<std::mutex> lock(s_InstanceMutex);
            if (s_Instance == nullptr)
            {
                throw std::runtime_error("Physics3DSystem: No instance to destroy - already destroyed or never created");
            }
            delete s_Instance;
            s_Instance = nullptr;
        }

        // Singleton access
        static Physics3DSystem& GetInstance()
        {
            if (s_Instance == nullptr)
            {
                throw std::runtime_error("Physics3DSystem: No instance available - call CreateInstance() first");
            }
            return *s_Instance;
        }

        // Initialize the physics system
        bool Initialize();
        
        // Shutdown the physics system
        void Shutdown();

        // Step the physics simulation
        void Update(f32 deltaTime);

        // Process activation events from the body activation listener (call on main thread)
        void ProcessActivationEvents();

        // Settings management
        static PhysicsSettings& GetSettings() { return s_PhysicsSettings; }
        static void SetSettings(const PhysicsSettings& settings);
        static void ApplySettings(); // Apply current settings to the physics system

        // Layer management
        static void UpdateLayerConfiguration(); // Update layer configuration when PhysicsLayerManager changes

        // Create a box body
        [[nodiscard]] JPH::BodyID CreateBox(const JPH::RVec3& position, const JPH::Quat& rotation, const JPH::Vec3& halfExtent, bool isStatic = false);
        
        // Create a sphere body
        [[nodiscard]] JPH::BodyID CreateSphere(const JPH::RVec3& position, f32 radius, bool isStatic = false);

        // Remove a body
        void RemoveBody(JPH::BodyID bodyID);

        // Get body interface for direct manipulation
        [[nodiscard]] JPH::BodyInterface* GetBodyInterface() 
        { 
            return m_PhysicsSystem ? &m_PhysicsSystem->GetBodyInterface() : nullptr; 
        }
        [[nodiscard]] const JPH::BodyInterface* GetBodyInterface() const 
        { 
            return m_PhysicsSystem ? &m_PhysicsSystem->GetBodyInterface() : nullptr; 
        }

        // Get the physics system for direct access
        [[nodiscard]] JPH::PhysicsSystem* GetPhysicsSystem() noexcept { return m_PhysicsSystem.get(); }

    private:
        // ====================================================================
        // Static Configuration & State
        // ====================================================================
        
        // Physics settings - now configurable instead of hard-coded
        inline static PhysicsSettings s_PhysicsSettings;

        // Static access to current instance for settings application
        inline static Physics3DSystem* s_Instance = nullptr;

        // Static access to layer interfaces for global layer management
        inline static OloBPLayerInterfaceImpl* s_BroadPhaseLayerInterface = nullptr;

        // Static mutex to protect singleton instance assignments
        inline static std::mutex s_InstanceMutex;

        // ====================================================================
        // Interfaces & Mappers
        // ====================================================================
        
        // Create mapping table from object layer to broadphase layer
        OloBPLayerInterfaceImpl m_BroadPhaseLayerInterface;
        OloObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBroadPhaseLayerFilter;

        // ====================================================================
        // Core Systems & Allocators
        // ====================================================================
        
        // The physics system
        std::unique_ptr<JPH::PhysicsSystem> m_PhysicsSystem;

        // A temp allocator
        std::unique_ptr<JPH::TempAllocatorImpl> m_TempAllocator;

        // Job system that will execute physics jobs
        std::unique_ptr<JPH::JobSystemThreadPool> m_JobSystem;

        // ====================================================================
        // Listeners & Flags
        // ====================================================================
        
        // Listeners
        PhysicsBodyActivationListener m_BodyActivationListener;
        JoltPhysicsSystemContactListener m_ContactListener;

        bool m_Initialized = false;

        // ====================================================================
        // Helper Methods
        // ====================================================================
        
        // Helper methods
        void UpdatePhysicsSystemSettings();

        // ====================================================================
        // Singleton Implementation
        // ====================================================================
        
        // Private constructor and destructor to prevent direct instantiation
        Physics3DSystem();
        ~Physics3DSystem();
        
        // Singleton pattern enforcement - prevent copying and moving
        Physics3DSystem(const Physics3DSystem&) = delete;
        Physics3DSystem& operator=(const Physics3DSystem&) = delete;
        Physics3DSystem(Physics3DSystem&&) = delete;
        Physics3DSystem& operator=(Physics3DSystem&&) = delete;

        // ====================================================================
        // Compile-time Constants
        // ====================================================================
        
        // Number of mutexes to allocate to protect rigid bodies from concurrent access
        // Can be overridden at compile time via -DOLO_PHYSICS_BODY_MUTEXES=N
        #ifndef OLO_PHYSICS_BODY_MUTEXES
        static constexpr u32 s_NumBodyMutexes = 8; // Default: 8 mutexes for reasonable concurrency protection
        #else
        static constexpr u32 s_NumBodyMutexes = OLO_PHYSICS_BODY_MUTEXES;
        #endif
    };

} // namespace OloEngine
