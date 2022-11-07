#include "OloEnginePCH.h"
#include "ScriptGlue.h"
#include "ScriptEngine.h"

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/Input.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

#include "mono/metadata/object.h"
#include "mono/metadata/reflection.h"

#include "box2d/b2_body.h"

namespace OloEngine {

	static std::unordered_map<MonoType*, std::function<bool(Entity)>> s_EntityHasComponentFuncs;

#define OLO_ADD_INTERNAL_CALL(Name) mono_add_internal_call("OloEngine.InternalCalls::" #Name, Name)

	static void NativeLog(MonoString* string, int parameter)
	{
		char* cStr = ::mono_string_to_utf8(string);
		std::string str(cStr);
		::mono_free(cStr);
		std::cout << str << ", " << parameter << "\n";
	}

	[[nodiscard("This returns the normalized vector, you probably wanted another function!")]] static void NativeLog_Vector(glm::vec3 const* parameter, glm::vec3* outResult)
	{
		//TODO(olbu): Fix the logger, glm::vec3* is not valid type, need to provide a formatter<T> specialization
		//https://fmt.dev/latest/api.html#udt
		//OLO_CORE_WARN("Value: {0}", *parameter);
		*outResult = glm::normalize(*parameter);
	}

	[[nodiscard("This returns the dot product, you probably wanted another function!")]] static float NativeLog_VectorDot(glm::vec3 const* parameter)
	{
		//TODO(olbu): Fix the logger, glm::vec3* is not valid type, need to provide a formatter<T> specialization
		//https://fmt.dev/latest/api.html#udt
		//OLO_CORE_WARN("Value: {0}", *parameter);
		return glm::dot(*parameter, *parameter);
	}

	static MonoObject* GetScriptInstance(UUID entityID)
	{
		return ScriptEngine::GetManagedInstance(entityID);
	}

	static bool Entity_HasComponent(UUID entityID, MonoReflectionType* componentType)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene)
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity)

		MonoType* managedType = ::mono_reflection_type_get_type(componentType);
		OLO_CORE_ASSERT(s_EntityHasComponentFuncs.contains(managedType))
		return s_EntityHasComponentFuncs.at(managedType)(entity);
	}

	static uint64_t Entity_FindEntityByName(MonoString* name)
	{
		char* nameCStr = mono_string_to_utf8(name);

		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene);
		Entity entity = scene->FindEntityByName(nameCStr);
		mono_free(nameCStr);

		if (!entity)
			return 0;

		return entity.GetUUID();
	}

	static void TransformComponent_GetTranslation(UUID entityID, glm::vec3* outTranslation)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene)
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity)

		*outTranslation = entity.GetComponent<TransformComponent>().Translation;
	}

	static void TransformComponent_SetTranslation(UUID entityID, glm::vec3 const* translation)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene)
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity)

		entity.GetComponent<TransformComponent>().Translation = *translation;
	}

	static void Rigidbody2DComponent_ApplyLinearImpulse(UUID entityID, glm::vec2 const* impulse, glm::vec2 const* point, bool wake)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene)
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity)

		auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();
		auto* body = (b2Body*)rb2d.RuntimeBody;
		body->ApplyLinearImpulse(b2Vec2(impulse->x, impulse->y), b2Vec2(point->x, point->y), wake);
	}

	static void Rigidbody2DComponent_ApplyLinearImpulseToCenter(UUID entityID, glm::vec2 const* impulse, bool wake)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene)
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity)

		auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();
		auto* body = static_cast<b2Body*>(rb2d.RuntimeBody);
		body->ApplyLinearImpulseToCenter(b2Vec2(impulse->x, impulse->y), wake);
	}

	static bool Input_IsKeyDown(KeyCode keycode)
	{
		return Input::IsKeyPressed(keycode);
	}

	template<typename... Component>
	static void RegisterComponent()
	{
		([]()
		{
			std::string_view typeName = typeid(Component).name();
			size_t pos = typeName.find_last_of(':');
			std::string_view structName = typeName.substr(pos + 1);
			std::string managedTypename = fmt::format("OloEngine.{}", structName);

			MonoType* managedType = ::mono_reflection_type_from_name(managedTypename.data(), ScriptEngine::GetCoreAssemblyImage());
			if (!managedType)
			{
				OLO_CORE_ERROR("Could not find component type {}", managedTypename);
				return;
			}
			s_EntityHasComponentFuncs[managedType] = [](Entity entity) { return entity.HasComponent<Component>(); };
		}(), ...);
	}

	template<typename... Component>
	static void RegisterComponent(ComponentGroup<Component...>)
	{
		RegisterComponent<Component...>();
	}

	void ScriptGlue::RegisterComponents()
	{
		RegisterComponent(AllComponents{});
	}

	void ScriptGlue::RegisterFunctions()
	{
		OLO_ADD_INTERNAL_CALL(NativeLog);
		OLO_ADD_INTERNAL_CALL(NativeLog_Vector);
		OLO_ADD_INTERNAL_CALL(NativeLog_VectorDot);

		OLO_ADD_INTERNAL_CALL(GetScriptInstance);

		OLO_ADD_INTERNAL_CALL(Entity_HasComponent);
		OLO_ADD_INTERNAL_CALL(Entity_FindEntityByName);

		OLO_ADD_INTERNAL_CALL(TransformComponent_GetTranslation);
		OLO_ADD_INTERNAL_CALL(TransformComponent_SetTranslation);

		OLO_ADD_INTERNAL_CALL(Rigidbody2DComponent_ApplyLinearImpulse);
		OLO_ADD_INTERNAL_CALL(Rigidbody2DComponent_ApplyLinearImpulseToCenter);

		OLO_ADD_INTERNAL_CALL(Input_IsKeyDown);
	}

}
