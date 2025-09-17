#include "OloEnginePCH.h"
#include "JoltMaterial.h"
#include "OloEngine/Scene/Components.h"

namespace OloEngine {

	JoltMaterial JoltMaterial::CreateFromBoxCollider(const BoxCollider3DComponent& collider)
	{
		return JoltMaterial(collider.Friction, collider.Restitution);
	}

	JoltMaterial JoltMaterial::CreateFromSphereCollider(const SphereCollider3DComponent& collider)
	{
		return JoltMaterial(collider.Friction, collider.Restitution);
	}

	JoltMaterial JoltMaterial::CreateFromCapsuleCollider(const CapsuleCollider3DComponent& collider)
	{
		return JoltMaterial(collider.Friction, collider.Restitution);
	}

}