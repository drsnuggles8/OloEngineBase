#include "OloEnginePCH.h"
#include "Physics3DSystem.h"

// Jolt includes
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

// Disable common warnings triggered by Jolt
JPH_SUPPRESS_WARNINGS

// Callback for traces, connect this to your own trace function if you have one
static void TraceImpl(const char* inFMT, ...)
{
    // Format the message
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);

    // Print to the TTY
    OLO_CORE_TRACE(buffer);
}

#ifdef JPH_ENABLE_ASSERTS

// Callback for asserts, connect this to your own assert handler if you have one
static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, JPH::uint inLine)
{
    // Print to the TTY
    OLO_CORE_ERROR("{0}:{1}: ({2}) {3}", inFile, inLine, inExpression, (inMessage != nullptr? inMessage : ""));

    // Breakpoint
    return true;
};

#endif // JPH_ENABLE_ASSERTS

namespace OloEngine {

Physics3DSystem::Physics3DSystem()
{
}

Physics3DSystem::~Physics3DSystem()
{
    if (m_Initialized)
    {
        Shutdown();
    }
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

    // Create a factory
    JPH::Factory::sInstance = new JPH::Factory();

    // Register all Jolt physics types
    JPH::RegisterTypes();

    // We need a temp allocator for temporary allocations during the physics update. We're
    // pre-allocating 10 MB to avoid having to do allocations during the physics update.
    // B.t.w. 10 MB is way too much for this example but it is a typical value you can use.
    // If you don't want to pre-allocate you can also use TempAllocatorMalloc to fall back to
    // malloc / free.
    m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    // We need a job system that will execute physics jobs on multiple threads. Typically
    // you would implement the JobSystem interface yourself and let Jolt Physics run on top
    // of your own job scheduler. JobSystemThreadPool is an example implementation.
    m_JobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::thread::hardware_concurrency() - 1);

    // Now we can create the actual physics system.
    m_PhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
    m_PhysicsSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, m_BroadPhaseLayerInterface, m_ObjectVsBroadPhaseLayerFilter, m_ObjectLayerPairFilter);

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

    // Destroy the physics system
    m_PhysicsSystem.reset();

    // Destroy the job system
    m_JobSystem.reset();

    // Destroy the temp allocator
    m_TempAllocator.reset();

    // Destroy the factory
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;

    m_Initialized = false;
    OLO_CORE_INFO("Physics3D system shut down");
}

void Physics3DSystem::Update(f32 deltaTime)
{
    if (!m_Initialized)
    {
        return;
    }

    // If you take larger steps than 1 / 60th of a second you need to do multiple collision steps in order to keep the simulation stable.
    // Do 1 step per 1/60th of a second (round up).
    const f32 cStepTime = 1.0f / 60.0f;
    i32 cCollisionSteps = static_cast<i32>(ceil(deltaTime / cStepTime));

    // Step the world
    m_PhysicsSystem->Update(deltaTime, cCollisionSteps, m_TempAllocator.get(), m_JobSystem.get());
}

JPH::BodyID Physics3DSystem::CreateBox(const JPH::RVec3& position, const JPH::Quat& rotation, const JPH::Vec3& halfExtent, bool isStatic)
{
    if (!m_Initialized)
    {
        OLO_CORE_ERROR("Physics3DSystem not initialized");
        return JPH::BodyID();
    }

    // Create a box shape
    JPH::RefConst<JPH::Shape> box_shape = new JPH::BoxShape(halfExtent);

    // Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
    JPH::BodyCreationSettings body_settings(box_shape, position, rotation, isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic, isStatic ? Layers::NON_MOVING : Layers::MOVING);

    // Create the actual rigid body
    JPH::Body* body = m_PhysicsSystem->GetBodyInterface().CreateBody(body_settings); // Note that if we run out of bodies this can return nullptr

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

    // Create a sphere shape
    JPH::RefConst<JPH::Shape> sphere_shape = new JPH::SphereShape(radius);

    // Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
    JPH::BodyCreationSettings body_settings(sphere_shape, position, JPH::Quat::sIdentity(), isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic, isStatic ? Layers::NON_MOVING : Layers::MOVING);

    // Create the actual rigid body
    JPH::Body* body = m_PhysicsSystem->GetBodyInterface().CreateBody(body_settings); // Note that if we run out of bodies this can return nullptr

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

} // namespace OloEngine