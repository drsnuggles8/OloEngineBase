#include "OloEnginePCH.h"
#include "JoltContactListener.h"
#include "JoltScene.h"
#include "JoltUtils.h"
#include "PhysicsLayer.h"
#include "JoltLayerInterface.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine {

	JoltContactListener::JoltContactListener(JoltScene* scene)
		: m_Scene(scene)
	{
		OLO_CORE_ASSERT(scene, "JoltContactListener requires a valid JoltScene");
	}

	JPH::ValidateResult JoltContactListener::OnContactValidate([[maybe_unused]] const JPH::Body& inBody1, [[maybe_unused]] const JPH::Body& inBody2, [[maybe_unused]] JPH::RVec3Arg inBaseOffset, [[maybe_unused]] const JPH::CollideShapeResult& inCollisionResult)
	{
		// Get the physics layer IDs from both bodies
		u32 layer1 = GetPhysicsLayerFromBody(inBody1);
		u32 layer2 = GetPhysicsLayerFromBody(inBody2);
		
		// If both bodies have custom physics layers, check if they should collide
		if (layer1 != INVALID_LAYER_ID && layer2 != INVALID_LAYER_ID)
		{
			if (!PhysicsLayerManager::ShouldCollide(layer1, layer2))
			{
				return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
			}
		}
		
		// Accept the contact if:
		// - At least one body uses built-in layers (handled by Jolt's layer interface)
		// - Both bodies have custom layers and should collide
		return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
	}

	void JoltContactListener::OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, [[maybe_unused]] JPH::ContactSettings& ioSettings)
	{
		UUID entityA = GetEntityIDFromBody(inBody1);
		UUID entityB = GetEntityIDFromBody(inBody2);

		if (entityA != 0 && entityB != 0 && !inManifold.mRelativeContactPointsOn1.empty())
		{
			// Create the SubShapeIDPair key for tracking this contact
			JPH::SubShapeIDPair contactKey(inBody1.GetID(), inManifold.mSubShapeID1, inBody2.GetID(), inManifold.mSubShapeID2);
			
			// Store the contact in our active contacts map
			{
				std::lock_guard<std::mutex> lock(m_ActiveContactsMutex);
				m_ActiveContacts.try_emplace(contactKey, entityA, entityB);
			}

			glm::vec3 contactPoint = JoltUtils::FromJoltRVec3(inManifold.GetWorldSpaceContactPointOn1(0));
			glm::vec3 contactNormal = JoltUtils::FromJoltVector(inManifold.mWorldSpaceNormal);
			f32 contactDepth = inManifold.mPenetrationDepth;

			QueueContactEvent(ContactEvent(ContactType::ContactAdded, entityA, entityB, contactPoint, contactNormal, contactDepth, 0.0f));
		}
	}

	void JoltContactListener::OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, [[maybe_unused]] JPH::ContactSettings& ioSettings)
	{
		UUID entityA = GetEntityIDFromBody(inBody1);
		UUID entityB = GetEntityIDFromBody(inBody2);

		if (entityA != 0 && entityB != 0 && !inManifold.mRelativeContactPointsOn1.empty())
		{
			// The contact should already be in our active contacts map from OnContactAdded
			// But we can ensure it's there (defensive programming)
			JPH::SubShapeIDPair contactKey(inBody1.GetID(), inManifold.mSubShapeID1, inBody2.GetID(), inManifold.mSubShapeID2);
			
			{
				std::lock_guard<std::mutex> lock(m_ActiveContactsMutex);
				// Only add if not already present (should not happen in normal operation)
				if (m_ActiveContacts.find(contactKey) == m_ActiveContacts.end())
				{
					m_ActiveContacts.try_emplace(contactKey, entityA, entityB);
				}
			}

			glm::vec3 contactPoint = JoltUtils::FromJoltRVec3(inManifold.GetWorldSpaceContactPointOn1(0));
			glm::vec3 contactNormal = JoltUtils::FromJoltVector(inManifold.mWorldSpaceNormal);
			f32 contactDepth = inManifold.mPenetrationDepth;

			QueueContactEvent(ContactEvent(ContactType::ContactPersisted, entityA, entityB, contactPoint, contactNormal, contactDepth, 0.0f));
		}
	}

	void JoltContactListener::OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair)
	{
		// Look up the contact in our active contacts map to retrieve the entity IDs
		UUID entityA = 0;
		UUID entityB = 0;
		
		{
			std::lock_guard<std::mutex> lock(m_ActiveContactsMutex);
			auto it = m_ActiveContacts.find(inSubShapePair);
			if (it != m_ActiveContacts.end())
			{
				entityA = it->second.EntityA;
				entityB = it->second.EntityB;
				
				// Remove the contact from our tracking map
				m_ActiveContacts.erase(it);
			}
		}
		
		// Only queue the event if we found valid entity IDs
		if (entityA != 0 && entityB != 0)
		{
			QueueContactEvent(ContactEvent(ContactType::ContactRemoved, entityA, entityB));
		}
	}

	void JoltContactListener::ProcessContactEvents()
	{
		// Check if there are events to process before acquiring lock
		const sizet queueSize = m_QueueSize.load(std::memory_order_relaxed);
		if (queueSize == 0)
		{
			return; // Early exit if no events to process
		}
		
		std::deque<ContactEvent> localEventQueue;
		
		// Swap the queue contents under minimal lock time
		{
			std::lock_guard<std::mutex> lock(m_ContactEventsMutex);
			
			// Swap entire queue - O(1) operation instead of N moves
			localEventQueue.swap(m_ContactEventQueue);
			
			// Reset queue size after swap
			m_QueueSize.store(0, std::memory_order_relaxed);
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
		
		// Check queue size limit and early-return to prevent queue growth during contact storms
		if (m_QueueSize.load(std::memory_order_relaxed) >= MaxQueuedContactEvents)
		{
			return; // Drop the event instead of growing the queue
		}
		
		m_ContactEventQueue.push_back(event);
		m_QueueSize.fetch_add(1, std::memory_order_relaxed);
	}

	void JoltContactListener::QueueContactEvent(ContactEvent&& event)
	{
		std::lock_guard<std::mutex> lock(m_ContactEventsMutex);
		
		// Check queue size limit and early-return to prevent queue growth during contact storms
		if (m_QueueSize.load(std::memory_order_relaxed) >= MaxQueuedContactEvents)
		{
			return; // Drop the event instead of growing the queue
		}
		
		m_ContactEventQueue.push_back(std::move(event));
		m_QueueSize.fetch_add(1, std::memory_order_relaxed);
	}

	UUID JoltContactListener::GetEntityIDFromBody(const JPH::Body& body) noexcept
	{
		// The entity ID is stored in the body's user data
		return static_cast<UUID>(body.GetUserData());
	}

	u32 JoltContactListener::GetPhysicsLayerFromBody(const JPH::Body& body) noexcept
	{
		JPH::ObjectLayer objectLayer = body.GetObjectLayer();
		
		// Check if this is a custom physics layer (offset by NUM_LAYERS)
		if (objectLayer >= ObjectLayers::NUM_LAYERS)
		{
			return static_cast<u32>(objectLayer) - ObjectLayers::NUM_LAYERS;
		}
		
		// For built-in layers, return an invalid layer ID to indicate no custom layer
		return INVALID_LAYER_ID;
	}

}