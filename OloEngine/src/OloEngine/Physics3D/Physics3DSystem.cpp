#include "OloEnginePCH.h"
#include "Physics3DSystem.h"
#include "PhysicsLayer.h"
#include "JoltLayerInterface.h"
#include "JoltUtils.h"

// Jolt includes
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

// Standard includes
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdarg>
#include <array>

// Disable common warnings triggered by Jolt
JPH_SUPPRESS_WARNINGS

// Callback for traces, connect this to your own trace function if you have one
static void TraceImpl(const char* inFMT, ...)
{
    va_list list;
    va_start(list, inFMT);
    
    // Create a copy for potential fallback use before consuming the original
    va_list fallbackList;
    va_copy(fallbackList, list);
    
    // First pass: determine required buffer size
    va_list sizeCheckList;
    va_copy(sizeCheckList, list);
    int requiredSize = vsnprintf(nullptr, 0, inFMT, sizeCheckList);
    va_end(sizeCheckList);
    
    if (requiredSize < 0)
    {
        // Handle vsnprintf error - fallback to truncated static buffer
        char fallbackBuffer[1024];
        vsnprintf(fallbackBuffer, sizeof(fallbackBuffer), inFMT, list);
        va_end(list);
        va_end(fallbackList);  // Clean up the fallback copy
        OLO_CORE_TRACE("{}", fallbackBuffer);
        return;
    }
    
    try
    {
        // Allocate string with required size (+1 for null terminator)
        std::string message(static_cast<size_t>(requiredSize), '\0');
        
        // Second pass: format into the allocated buffer
        vsnprintf(message.data(), static_cast<size_t>(requiredSize) + 1, inFMT, list);
        va_end(list);
        va_end(fallbackList);  // Clean up the unused fallback copy
        
        // Print the complete message
        OLO_CORE_TRACE("{}", message);
    }
    catch (const std::bad_alloc&)
    {
        // Handle allocation failure - use the fallback copy we prepared earlier
        va_end(list);  // Clean up the original list first
        char fallbackBuffer[1024];
        vsnprintf(fallbackBuffer, sizeof(fallbackBuffer), inFMT, fallbackList);
        va_end(fallbackList);
        OLO_CORE_TRACE("{}", fallbackBuffer);
    }
}

#ifdef JPH_ENABLE_ASSERTS

// Callback for asserts, connect this to your own assert handler if you have one
static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, JPH::uint inLine)
{
    // Print to the TTY
    OLO_CORE_ERROR("{0}:{1}: ({2}) {3}", inFile, inLine, inExpression, (inMessage != nullptr? inMessage : ""));

    // Breakpoint
    return true;
}

#endif // JPH_ENABLE_ASSERTS

namespace OloEngine {

// Physics simulation constants
constexpr f32 kMinPhysicsExtent = 1e-3f;  // Minimum practical extent for stable physics simulation (1mm)

// ================================================================================================
// Layer Interface Implementations
// ================================================================================================

OloBPLayerInterfaceImpl::OloBPLayerInterfaceImpl()
{
    // Initialize the layer mapping
    UpdateLayers();
}

JPH::uint OloBPLayerInterfaceImpl::GetNumBroadPhaseLayers() const
{
    // Include the built-in object layers plus custom layers, clamped to s_MaxLayers
    u32 total = PhysicsLayerManager::GetLayerCount() + ObjectLayers::NUM_LAYERS;
    return std::min(total, static_cast<u32>(s_MaxLayers));
}

JPH::BroadPhaseLayer OloBPLayerInterfaceImpl::GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const
{
    // For now, map object layers directly to broadphase layers (1:1 mapping)
    u32 layerIndex = static_cast<u32>(inLayer);
    if (layerIndex < GetNumBroadPhaseLayers())
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layerIndex));
    
    // Default to first layer if invalid
    return JPH::BroadPhaseLayer(0);
}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
const char* OloBPLayerInterfaceImpl::GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const
{
    u32 layerIndex = inLayer.GetValue();
    
    // First check if it's a built-in broad phase layer
    static const std::array<const char*, BroadPhaseLayers::NUM_LAYERS> builtInLayerNames = {
        "NON_MOVING",  // BroadPhaseLayers::NON_MOVING (0)
        "MOVING"       // BroadPhaseLayers::MOVING (1)
    };
    
    if (layerIndex < builtInLayerNames.size())
    {
        return builtInLayerNames[layerIndex];
    }
    
    // Check if it's a custom layer from PhysicsLayerManager
    const auto& layerNames = PhysicsLayerManager::GetLayerNames();
    u32 customLayerIndex = layerIndex - BroadPhaseLayers::NUM_LAYERS;
    
    if (customLayerIndex < layerNames.size())
    {
        return layerNames[customLayerIndex].c_str();
    }
    
    // Fallback for any other out-of-range values
    return "Unknown";
}
#endif // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

void OloBPLayerInterfaceImpl::UpdateLayers()
{
    // This method can be called when the layer configuration changes
    // Currently using direct mapping, but could be extended for more complex mapping strategies
}

bool OloObjectVsBroadPhaseLayerFilterImpl::ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const
{
    // Convert broadphase layer back to object layer for collision checking
    // Since we use 1:1 mapping, this is straightforward
    JPH::ObjectLayer objectLayer2 = static_cast<JPH::ObjectLayer>(inLayer2.GetValue());
    
    // If both layers are user-defined physics layers, map to custom layer IDs and check
    if (inLayer1 >= OloEngine::ObjectLayers::NUM_LAYERS && objectLayer2 >= OloEngine::ObjectLayers::NUM_LAYERS)
    {
        u32 layer1 = static_cast<u32>(inLayer1) - OloEngine::ObjectLayers::NUM_LAYERS;
        u32 layer2 = static_cast<u32>(objectLayer2) - OloEngine::ObjectLayers::NUM_LAYERS;
        return PhysicsLayerManager::ShouldCollide(layer1, layer2);
    }
    
    // For built-in layers, use default Jolt collision logic (always allow)
    return true;
}

// ================================================================================================
// Physics3DSystem Implementation
// ================================================================================================

Physics3DSystem::Physics3DSystem()
{
    // Set up static pointers for global access
    s_Instance = this;
    s_BroadPhaseLayerInterface = &m_BroadPhaseLayerInterface;
}

Physics3DSystem::~Physics3DSystem()
{
    if (m_Initialized)
    {
        Shutdown();
    }
    
    // Clear static pointers
    if (s_Instance == this)
        s_Instance = nullptr;
    if (s_BroadPhaseLayerInterface == &m_BroadPhaseLayerInterface)
        s_BroadPhaseLayerInterface = nullptr;
}

bool Physics3DSystem::Initialize()
{
    if (m_Initialized)
    {
        OLO_CORE_WARN("Physics3DSystem already initialized");
        return true;
    }

    // Register allocation hook
    JPH::RegisterDefaultAllocator();

    // Install callbacks
    JPH::Trace = TraceImpl;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)

    // Create a factory - ensure clean state and proper ownership using RAII
    // Capture old factory for automatic cleanup and create custom deleter that nulls sInstance
    std::unique_ptr<JPH::Factory, void(*)(JPH::Factory*)> oldFactory(
        JPH::Factory::sInstance,
        [](JPH::Factory* factory) {
            JPH::Factory::sInstance = nullptr;
            delete factory;
        }
    );
    
    // Exception-safe factory creation using RAII
    auto newFactory = std::make_unique<JPH::Factory>();
    
    // Register all Jolt physics types (this might throw)
    JPH::RegisterTypes();
    
    // Only assign to sInstance after all operations that might throw have succeeded
    JPH::Factory::sInstance = newFactory.release();
    // Old factory will be automatically cleaned up when oldFactory goes out of scope

    // We need a temp allocator for temporary allocations during the physics update. We're
    // pre-allocating 10 MB to avoid having to do allocations during the physics update.
    // B.t.w. 10 MB is way too much for this example but it is a typical value you can use.
    // If you don't want to pre-allocate you can also use TempAllocatorMalloc to fall back to
    // malloc / free.
    m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    // We need a job system that will execute physics jobs on multiple threads. Typically
    // you would implement the JobSystem interface yourself and let Jolt Physics run on top
    // of your own job scheduler. JobSystemThreadPool is an example implementation.
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 1; // Treat 0 as 1
    unsigned workerThreads = (hc > 1 ? hc - 1 : 1);
    m_JobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerThreads);

    // Now we can create the actual physics system.
    m_PhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
    m_PhysicsSystem->Init(s_PhysicsSettings.m_MaxBodies, s_NumBodyMutexes, s_PhysicsSettings.m_MaxBodyPairs, s_PhysicsSettings.m_MaxContactConstraints, m_BroadPhaseLayerInterface, m_ObjectVsBroadPhaseLayerFilter, OloEngine::JoltLayerInterface::GetObjectLayerPairFilter());

    // Apply physics settings to the Jolt system
    UpdatePhysicsSystemSettings();

    // A body activation listener gets notified when bodies activate and go to sleep
    // Note that this is called from a job so whatever you do here needs to be thread safe.
    // Registering one is entirely optional.
    m_PhysicsSystem->SetBodyActivationListener(&m_BodyActivationListener);

    // A contact listener gets notified when bodies (are about to) collide, and when they separate again.
    // Note that this is called from a job so whatever you do here needs to be thread safe.
    // Registering one is entirely optional.
    m_PhysicsSystem->SetContactListener(&m_ContactListener);

    // The main way to interact with the bodies in the physics system is through the body interface.
    // You can use this to create and remove bodies, change their position, and apply impulses, etc.
    // Note that if you know that you're always accessing the body interface from the same thread or that you're
    // doing a lot of reads or writes you can use the PhysicsSystem::GetBodyInterfaceNoLock() to avoid locking.

    m_Initialized = true;
    OLO_CORE_INFO("Physics3D system initialized successfully");

    return true;
}

void Physics3DSystem::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    // Remove the contact listener
    m_PhysicsSystem->SetContactListener(nullptr);

    // Remove the body activation listener
    m_PhysicsSystem->SetBodyActivationListener(nullptr);

    // Shutdown capture manager
    // Note: JoltCaptureManager instances are managed independently

    // Destroy the physics system
    m_PhysicsSystem.reset();

    // Destroy the job system
    m_JobSystem.reset();

    // Destroy the temp allocator
    m_TempAllocator.reset();

    // Destroy the factory - only if we own it
    if (JPH::Factory::sInstance != nullptr)
    {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    m_Initialized = false;
    OLO_CORE_INFO("Physics3D system shut down");
}

void Physics3DSystem::Update(f32 deltaTime)
{
    if (!m_Initialized)
    {
        return;
    }

    // Guard against non-positive deltaTime
    if (deltaTime <= 0.0f)
    {
        return; // Skip update for invalid deltaTime
    }

    // Validate step time configuration
    const f32 stepTime = s_PhysicsSettings.m_FixedTimestep;
    if (stepTime <= 0.0f)
    {
        OLO_CORE_ERROR("Physics3DSystem::Update: Invalid fixed timestep configuration ({} <= 0)", stepTime);
        return;
    }

    // If you take larger steps than the fixed timestep you need to do multiple collision steps in order to keep the simulation stable.
    // Do 1 step per fixed timestep (round up), but ensure at least 1 step.
    i32 collisionSteps = std::max(1, static_cast<i32>(std::ceil(deltaTime / stepTime)));

    // Step the world
    m_PhysicsSystem->Update(deltaTime, collisionSteps, m_TempAllocator.get(), m_JobSystem.get());
}

void Physics3DSystem::ProcessActivationEvents()
{
    if (!m_Initialized)
    {
        return;
    }

    // Process any pending body activation events on the main thread
    m_BodyActivationListener.ProcessEvents();
}

JPH::BodyID Physics3DSystem::CreateBox(const JPH::RVec3& position, const JPH::Quat& rotation, const JPH::Vec3& halfExtent, bool isStatic)
{
    if (!m_Initialized)
    {
        OLO_CORE_ERROR("Physics3DSystem not initialized");
        return JPH::BodyID();
    }

    // Validate box halfExtent components
    if (halfExtent.GetX() <= kMinPhysicsExtent || halfExtent.GetY() <= kMinPhysicsExtent || halfExtent.GetZ() <= kMinPhysicsExtent)
    {
        OLO_CORE_ERROR("Physics3DSystem::CreateBox: Invalid halfExtent vector ({}, {}, {}) - all components must be > {} (minimum physics extent)",
                       halfExtent.GetX(), halfExtent.GetY(), halfExtent.GetZ(), kMinPhysicsExtent);
        return JPH::BodyID();
    }

    // Create a box shape
    JPH::RefConst<JPH::Shape> box_shape = new JPH::BoxShape(halfExtent);

    // Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
    JPH::BodyCreationSettings body_settings(box_shape, position, rotation, isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic, isStatic ? OloEngine::ObjectLayers::NON_MOVING : OloEngine::ObjectLayers::MOVING);

    // Create the actual rigid body
    JPH::Body* body = m_PhysicsSystem->GetBodyInterface().CreateBody(body_settings); // Note that if we run out of bodies this can return nullptr
    if (body == nullptr)
    {
        OLO_CORE_ERROR("Failed to create box body - physics system may be out of bodies");
        return JPH::BodyID();
    }

    // Add it to the world
    JPH::BodyID body_id = body->GetID();
    m_PhysicsSystem->GetBodyInterface().AddBody(body_id, JPH::EActivation::Activate);

    return body_id;
}

JPH::BodyID Physics3DSystem::CreateSphere(const JPH::RVec3& position, f32 radius, bool isStatic)
{
    if (!m_Initialized)
    {
        OLO_CORE_ERROR("Physics3DSystem not initialized");
        return JPH::BodyID();
    }

    // Validate sphere radius
    if (radius < kMinPhysicsExtent)
    {
        OLO_CORE_ERROR("Physics3DSystem::CreateSphere: Invalid radius ({}) - must be >= {} (minimum physics extent)", radius, kMinPhysicsExtent);
        return JPH::BodyID();
    }

    // Create a sphere shape
    JPH::RefConst<JPH::Shape> sphere_shape = new JPH::SphereShape(radius);

    // Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
    JPH::BodyCreationSettings body_settings(sphere_shape, position, JPH::Quat::sIdentity(), isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic, isStatic ? OloEngine::ObjectLayers::NON_MOVING : OloEngine::ObjectLayers::MOVING);

    // Create the actual rigid body
    JPH::Body* body = m_PhysicsSystem->GetBodyInterface().CreateBody(body_settings); // Note that if we run out of bodies this can return nullptr
    if (body == nullptr)
    {
        OLO_CORE_ERROR("Failed to create sphere body - physics system may be out of bodies");
        return JPH::BodyID();
    }

    // Add it to the world
    JPH::BodyID body_id = body->GetID();
    m_PhysicsSystem->GetBodyInterface().AddBody(body_id, JPH::EActivation::Activate);

    return body_id;
}

void Physics3DSystem::RemoveBody(JPH::BodyID bodyID)
{
    if (!m_Initialized)
    {
        return;
    }

    JPH::BodyInterface& body_interface = m_PhysicsSystem->GetBodyInterface();
    
    // Remove the body from the physics system. Note that the body itself keeps all of its state and can be re-added at any time.
    body_interface.RemoveBody(bodyID);

    // Destroy the body. After this the body ID is no longer valid.
    body_interface.DestroyBody(bodyID);
}

void Physics3DSystem::SetSettings(const PhysicsSettings& settings)
{
    s_PhysicsSettings = settings;
    ApplySettings();
}

void Physics3DSystem::ApplySettings()
{
    // Apply settings if there's an active physics system instance
    if (s_Instance)
    {
        s_Instance->UpdatePhysicsSystemSettings();
    }
}

void Physics3DSystem::UpdatePhysicsSystemSettings()
{
    if (!m_PhysicsSystem)
        return;

    // Apply gravity directly to the physics system
    m_PhysicsSystem->SetGravity(JoltUtils::ToJoltVector(s_PhysicsSettings.m_Gravity));

    // Create Jolt physics settings from our settings
    JPH::PhysicsSettings joltSettings;
    
    // Basic simulation settings
    joltSettings.mNumVelocitySteps = s_PhysicsSettings.m_VelocitySolverIterations;
    joltSettings.mNumPositionSteps = s_PhysicsSettings.m_PositionSolverIterations;
    
    // Advanced Jolt settings
    joltSettings.mBaumgarte = s_PhysicsSettings.m_Baumgarte;
    joltSettings.mSpeculativeContactDistance = s_PhysicsSettings.m_SpeculativeContactDistance;
    joltSettings.mPenetrationSlop = s_PhysicsSettings.m_PenetrationSlop;
    joltSettings.mLinearCastThreshold = s_PhysicsSettings.m_LinearCastThreshold;
    joltSettings.mMinVelocityForRestitution = s_PhysicsSettings.m_MinVelocityForRestitution;
    joltSettings.mTimeBeforeSleep = s_PhysicsSettings.m_TimeBeforeSleep;
    joltSettings.mPointVelocitySleepThreshold = s_PhysicsSettings.m_PointVelocitySleepThreshold;
    
    // Boolean settings
    joltSettings.mDeterministicSimulation = s_PhysicsSettings.m_DeterministicSimulation;
    joltSettings.mConstraintWarmStart = s_PhysicsSettings.m_ConstraintWarmStart;
    joltSettings.mUseBodyPairContactCache = s_PhysicsSettings.m_UseBodyPairContactCache;
    joltSettings.mUseManifoldReduction = s_PhysicsSettings.m_UseManifoldReduction;
    joltSettings.mUseLargeIslandSplitter = s_PhysicsSettings.m_UseLargeIslandSplitter;
    joltSettings.mAllowSleeping = s_PhysicsSettings.m_AllowSleeping;

    // Apply settings to the physics system
    m_PhysicsSystem->SetPhysicsSettings(joltSettings);
    
    OLO_CORE_INFO("Physics settings applied successfully");
}

void Physics3DSystem::UpdateLayerConfiguration()
{
    // Update the broadphase layer interface when layer configuration changes
    if (s_BroadPhaseLayerInterface)
    {
        s_BroadPhaseLayerInterface->UpdateLayers();
    }
    
    // Note: In a real implementation, you might need to recreate the physics system
    // or update the collision filters if the layer configuration changes significantly
    // For now, this provides the foundation for dynamic layer management
}

// PhysicsBodyActivationListener implementations
void PhysicsBodyActivationListener::OnBodyActivated(const JPH::BodyID& inBodyID, JPH::uint64 inBodyUserData)
{
    // Thread-safe: enqueue event for main thread processing
    ActivationEvent event{ ActivationEvent::Activated, inBodyID, inBodyUserData };
    EnqueueEvent(event);
}

void PhysicsBodyActivationListener::OnBodyDeactivated(const JPH::BodyID& inBodyID, JPH::uint64 inBodyUserData)
{
    // Thread-safe: enqueue event for main thread processing
    ActivationEvent event{ ActivationEvent::Deactivated, inBodyID, inBodyUserData };
    EnqueueEvent(event);
}

void PhysicsBodyActivationListener::ProcessEvents()
{
    ActivationEvent event;
    while (TryDequeueEvent(event))
    {
        switch (event.EventType)
        {
            case ActivationEvent::Activated:
                OLO_CORE_INFO("Body {} got activated", event.BodyID.GetIndex());
                break;
            case ActivationEvent::Deactivated:
                OLO_CORE_INFO("Body {} went to sleep", event.BodyID.GetIndex());
                break;
        }
    }
}

sizet PhysicsBodyActivationListener::GetPendingEventCount() const noexcept
{
    return m_QueueSize.load(std::memory_order_relaxed);
}

// ====================================================================
// JoltPhysicsSystemContactListener Implementation
// ====================================================================

// Compile-time flag to enable/disable contact logging
// Define OLO_ENABLE_CONTACT_LOGGING to enable verbose contact logging
#ifdef OLO_ENABLE_CONTACT_LOGGING
    #define OLO_CONTACT_LOG(message) OLO_CORE_INFO(message)
#else
    #define OLO_CONTACT_LOG(message) ((void)0)
#endif

JPH::ValidateResult JoltPhysicsSystemContactListener::OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult& inCollisionResult)
{
    OLO_CONTACT_LOG("Contact validate callback");
    
    // Allows you to ignore a contact before it is created (using layers to not make objects collide is cheaper!)
    return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
}

void JoltPhysicsSystemContactListener::OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings)
{
    OLO_CONTACT_LOG("A contact was added");
}

void JoltPhysicsSystemContactListener::OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings)
{
    OLO_CONTACT_LOG("A contact was persisted");
}

void JoltPhysicsSystemContactListener::OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair)
{
    OLO_CONTACT_LOG("A contact was removed");
}

} // namespace OloEngine