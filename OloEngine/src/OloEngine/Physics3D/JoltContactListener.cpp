#include "OloEnginePCH.h"
#include "JoltContactListener.h"
#include "JoltScene.h"
#include "JoltUtils.h"
#include "PhysicsLayer.h"
#include "JoltLayerInterface.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine {

	JoltContactListener::JoltContactListener(JoltScene& scene)
		: m_Scene(scene)
	{
		// No null check needed since we now accept a reference
	}

	[[nodiscard]] JPH::ValidateResult JoltContactListener::OnContactValidate([[maybe_unused]] const JPH::Body& inBody1, [[maybe_unused]] const JPH::Body& inBody2, [[maybe_unused]] JPH::RVec3Arg inBaseOffset, [[maybe_unused]] const JPH::CollideShapeResult& inCollisionResult)
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
		ProcessContactManifold(inBody1, inBody2, inManifold, ContactType::ContactAdded);
	}

	void JoltContactListener::OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, [[maybe_unused]] JPH::ContactSettings& ioSettings)
	{
		ProcessContactManifold(inBody1, inBody2, inManifold, ContactType::ContactPersisted);
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
				entityA = it->second.m_EntityA;
				entityB = it->second.m_EntityB;
				
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
			m_Scene.OnContactEvent(event.m_Type, event.m_EntityA, event.m_EntityB);
		}
	}

	void JoltContactListener::ProcessContactManifold(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, ContactType type)
	{
		UUID entityA = GetEntityIDFromBody(inBody1);
		UUID entityB = GetEntityIDFromBody(inBody2);

		if (entityA != 0 && entityB != 0 && !inManifold.mRelativeContactPointsOn1.empty())
		{
			// Create the SubShapeIDPair key for tracking this contact
			JPH::SubShapeIDPair contactKey(inBody1.GetID(), inManifold.mSubShapeID1, inBody2.GetID(), inManifold.mSubShapeID2);
			
			// Handle contact tracking based on the contact type
			{
				std::lock_guard<std::mutex> lock(m_ActiveContactsMutex);
				if (type == ContactType::ContactAdded)
				{
					// Always insert for ContactAdded
					m_ActiveContacts.try_emplace(contactKey, entityA, entityB);
				}
				else if (type == ContactType::ContactPersisted)
				{
					// Only add if not already present (should not happen in normal operation)
					if (m_ActiveContacts.find(contactKey) == m_ActiveContacts.end())
					{
						m_ActiveContacts.try_emplace(contactKey, entityA, entityB);
					}
				}
			}

			glm::vec3 contactPoint = JoltUtils::FromJoltRVec3(inManifold.GetWorldSpaceContactPointOn1(0));
			glm::vec3 contactNormal = JoltUtils::FromJoltVector(inManifold.mWorldSpaceNormal);
			f32 contactDepth = inManifold.mPenetrationDepth;

			QueueContactEvent(ContactEvent(type, entityA, entityB, contactPoint, contactNormal, contactDepth, 0.0f));
		}
	}

	[[nodiscard]] UUID JoltContactListener::GetEntityIDFromBody(const JPH::Body& body) noexcept
	{
		// The entity ID is stored in the body's user data
		return static_cast<UUID>(body.GetUserData());
	}

	[[nodiscard]] u32 JoltContactListener::GetPhysicsLayerFromBody(const JPH::Body& body) noexcept
	{
		JPH::ObjectLayer objectLayer = body.GetObjectLayer();
		
		// Check if this is a custom physics layer (offset by NUM_LAYERS)
		if (objectLayer >= ObjectLayers::NUM_LAYERS)
		{
			u32 customLayerIndex = static_cast<u32>(objectLayer) - ObjectLayers::NUM_LAYERS;
			
			// Validate against maximum Jolt layers to prevent undefined behavior
			if (customLayerIndex >= JoltUtils::kMaxJoltLayers)
			{
				OLO_CORE_ERROR("JoltContactListener::GetPhysicsLayerFromBody: Custom layer index {} exceeds maximum ({})", 
					customLayerIndex, JoltUtils::kMaxJoltLayers - 1);
				return INVALID_LAYER_ID;
			}
			
			return customLayerIndex;
		}
		
		// For built-in layers, return an invalid layer ID to indicate no custom layer
		return INVALID_LAYER_ID;
	}

}