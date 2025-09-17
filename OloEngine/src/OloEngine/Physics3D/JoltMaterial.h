#pragma once

namespace OloEngine {

	// Forward declarations
	struct BoxCollider3DComponent;
	struct SphereCollider3DComponent;
	struct CapsuleCollider3DComponent;

	class JoltMaterial
	{
	public:
		JoltMaterial() = default;
		JoltMaterial(float friction, float restitution)
			: m_Friction(friction), m_Restitution(restitution)
		{}

		float GetFriction() const { return m_Friction; }
		void SetFriction(float friction) { m_Friction = friction; }

		float GetRestitution() const { return m_Restitution; }
		void SetRestitution(float restitution) { m_Restitution = restitution; }

		// Helper functions to create materials from our collider components
		static JoltMaterial CreateFromBoxCollider(const BoxCollider3DComponent& collider);
		static JoltMaterial CreateFromSphereCollider(const SphereCollider3DComponent& collider);
		static JoltMaterial CreateFromCapsuleCollider(const CapsuleCollider3DComponent& collider);

	private:
		float m_Friction = 0.2f;
		float m_Restitution = 0.0f;
	};

}