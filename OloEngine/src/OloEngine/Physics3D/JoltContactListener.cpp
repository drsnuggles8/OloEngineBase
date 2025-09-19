#include "OloEnginePCH.h"
#include "JoltContactListener.h"
#include "JoltScene.h"
#include "JoltUtils.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine {

	JoltContactListener::JoltContactListener(JoltScene* scene)
		: m_Scene(scene)
	{
		OLO_CORE_ASSERT(scene, "JoltContactListener requires a valid JoltScene");
	}

	JPH::ValidateResult JoltContactListener::OnContactValidate([[maybe_unused]] const JPH::Body& inBody1, [[maybe_unused]] const JPH::Body& inBody2, [[maybe_unused]] JPH::RVec3Arg inBaseOffset, [[maybe_unused]] const JPH::CollideShapeResult& inCollisionResult)
	{
		// You can use this to validate contacts before they are added.
		// For now, we accept all contacts
		return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
	}

	void JoltContactListener::OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, [[maybe_unused]] JPH::ContactSettings& ioSettings)
	{
		UUID entityA = GetEntityIDFromBody(inBody1);
		UUID entityB = GetEntityIDFromBody(inBody2);

		if (entityA != 0 && entityB != 0 && inManifold.mRelativeContactPointsOn1.size() > 0)
		{
			glm::vec3 contactPoint = JoltUtils::FromJoltVector(inManifold.GetWorldSpaceContactPointOn1(0));
			glm::vec3 contactNormal = JoltUtils::FromJoltVector(inManifold.mWorldSpaceNormal);
			f32 contactDepth = inManifold.mPenetrationDepth;

			QueueContactEvent(ContactEvent(ContactType::ContactAdded, entityA, entityB, contactPoint, contactNormal, contactDepth, 0.0f));
		}
	}

	void JoltContactListener::OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, [[maybe_unused]] JPH::ContactSettings& ioSettings)
	{
		UUID entityA = GetEntityIDFromBody(inBody1);
		UUID entityB = GetEntityIDFromBody(inBody2);

		if (entityA != 0 && entityB != 0 && inManifold.mRelativeContactPointsOn1.size() > 0)
		{
			glm::vec3 contactPoint = JoltUtils::FromJoltVector(inManifold.GetWorldSpaceContactPointOn1(0));
			glm::vec3 contactNormal = JoltUtils::FromJoltVector(inManifold.mWorldSpaceNormal);
			f32 contactDepth = inManifold.mPenetrationDepth;

			QueueContactEvent(ContactEvent(ContactType::ContactPersisted, entityA, entityB, contactPoint, contactNormal, contactDepth, 0.0f));
		}
	}

	void JoltContactListener::OnContactRemoved([[maybe_unused]] const JPH::SubShapeIDPair& inSubShapePair)
	{
		// Note: We can't get the bodies directly from the SubShapeIDPair, so we'll need to store additional data
		// For now, we'll skip contact removed events. This can be enhanced later if needed.
		
		// ContactEvent event(ContactType::ContactRemoved, entityA, entityB);
		// QueueContactEvent(event);
	}

	void JoltContactListener::ProcessContactEvents()
	{
		// Drain the queue into a local container while holding the lock
		std::deque<ContactEvent> localEventQueue;
		{
			std::lock_guard<std::mutex> lock(m_ContactEventsMutex);
			
			// Move all events to local queue for processing without holding the mutex
			localEventQueue = std::move(m_ContactEventQueue);
			m_ContactEventQueue.clear(); // Ensure the original queue is empty
			m_QueueSize.store(0, std::memory_order_relaxed); // Reset queue size
		}
		
		// Process all contact events without holding the mutex
		for (const ContactEvent& event : localEventQueue)
		{
			// Send the contact event to the scene for processing
			if (m_Scene)
			{
				m_Scene->OnContactEvent(event.Type, event.EntityA, event.EntityB);
			}
		}
	}

	void JoltContactListener::QueueContactEvent(const ContactEvent& event)
	{
		std::lock_guard<std::mutex> lock(m_ContactEventsMutex);
		
		// Prevent memory issues by limiting the queue size
		if (m_ContactEventQueue.size() >= MaxQueuedContactEvents)
		{
			OLO_CORE_WARN("Contact event queue is full! Dropping oldest event.");
			m_ContactEventQueue.pop_front();
			m_QueueSize.fetch_sub(1, std::memory_order_relaxed);
		}
		
		m_ContactEventQueue.push_back(event);
		m_QueueSize.fetch_add(1, std::memory_order_relaxed);
	}

	void JoltContactListener::QueueContactEvent(ContactEvent&& event)
	{
		std::lock_guard<std::mutex> lock(m_ContactEventsMutex);
		
		// Prevent memory issues by limiting the queue size
		if (m_ContactEventQueue.size() >= MaxQueuedContactEvents)
		{
			OLO_CORE_WARN("Contact event queue is full! Dropping oldest event.");
			m_ContactEventQueue.pop_front();
			m_QueueSize.fetch_sub(1, std::memory_order_relaxed);
		}
		
		m_ContactEventQueue.push_back(std::move(event));
		m_QueueSize.fetch_add(1, std::memory_order_relaxed);
	}

	UUID JoltContactListener::GetEntityIDFromBody(const JPH::Body& body)
	{
		// The entity ID is stored in the body's user data
		return static_cast<UUID>(body.GetUserData());
	}

}