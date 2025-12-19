#pragma once

#include "Physics3DTypes.h"
#include "JoltUtils.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/Entity.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>

#include <mutex>
#include <deque>
#include <atomic>
#include <unordered_map>

namespace OloEngine
{

    class JoltScene; // Forward declaration

    class JoltContactListener : public JPH::ContactListener
    {
      public:
        JoltContactListener(JoltScene& scene);
        virtual ~JoltContactListener() noexcept = default;

        // Called when a contact is detected
        [[nodiscard]] virtual JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult& inCollisionResult) override;

        // Called when a contact is added (first frame of contact)
        virtual void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override;

        // Called when a contact persists (second and subsequent frames of contact)
        virtual void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override;

        // Called when a contact is removed (last frame of contact)
        virtual void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override;

        // Process queued contact events (call this from the main thread)
        void ProcessContactEvents();

        // Get the number of pending contact events
        [[nodiscard]] sizet GetPendingContactEventCount() const noexcept
        {
            return m_QueueSize.load(std::memory_order_relaxed);
        }

      private:
        struct ContactEvent
        {
            ContactType m_Type = ContactType::None;
            UUID m_EntityA = 0;
            UUID m_EntityB = 0;
            glm::vec3 m_ContactPoint = glm::vec3(0.0f);
            glm::vec3 m_ContactNormal = glm::vec3(0.0f);
            f32 m_ContactDepth = 0.0f;
            f32 m_ContactImpulse = 0.0f;

            ContactEvent() = default;
            ContactEvent(ContactType type, UUID entityA, UUID entityB)
                : m_Type(type), m_EntityA(entityA), m_EntityB(entityB) {}
            ContactEvent(ContactType type, UUID entityA, UUID entityB, const glm::vec3& point, const glm::vec3& normal, f32 depth, f32 impulse)
                : m_Type(type), m_EntityA(entityA), m_EntityB(entityB), m_ContactPoint(point), m_ContactNormal(normal), m_ContactDepth(depth), m_ContactImpulse(impulse) {}
        };

        template<typename T>
        void QueueContactEvent(T&& event)
        {
            std::lock_guard<std::mutex> lock(m_ContactEventsMutex);

            // Check queue size limit and early-return to prevent queue growth during contact storms
            // Use the protected container size under mutex rather than relaxed atomic load
            if (m_ContactEventQueue.size() >= MaxQueuedContactEvents)
            {
                return; // Drop the event instead of growing the queue
            }

            m_ContactEventQueue.push_back(std::forward<T>(event));
            // Update atomic counter to maintain consistency for fast external queries
            m_QueueSize.store(m_ContactEventQueue.size(), std::memory_order_relaxed);
        }

        // Retrieves entity UUID from JPH::Body::GetUserData (expects u64 UUID); returns 0 when no valid UUID is present
        [[nodiscard]] UUID GetEntityIDFromBody(const JPH::Body& body) noexcept;

        // Retrieves physics layer ID from JPH::Body::GetObjectLayer; returns INVALID_LAYER_ID for built-in layers
        [[nodiscard]] u32 GetPhysicsLayerFromBody(const JPH::Body& body) noexcept;

        // Helper method to process contact manifolds and avoid duplicate logic
        void ProcessContactManifold(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, ContactType type);

      private:
        JoltScene& m_Scene; // non-owning reference, guaranteed valid for lifetime of listener

        // Active contacts tracking for OnContactRemoved
        struct ContactInfo
        {
            UUID m_EntityA;
            UUID m_EntityB;

            ContactInfo() = default;
            ContactInfo(UUID entityA, UUID entityB) : m_EntityA(entityA), m_EntityB(entityB) {}
        };

        mutable std::mutex m_ActiveContactsMutex;
        std::unordered_map<JPH::SubShapeIDPair, ContactInfo> m_ActiveContacts;

        // Thread-safe contact event queue
        mutable std::mutex m_ContactEventsMutex;
        std::deque<ContactEvent> m_ContactEventQueue;

        // Atomic queue size counter for fast, thread-safe access
        std::atomic<sizet> m_QueueSize{ 0 };

        // Maximum number of contact events to queue (to prevent memory issues)
        static constexpr sizet MaxQueuedContactEvents = 10000;
    };

} // namespace OloEngine
