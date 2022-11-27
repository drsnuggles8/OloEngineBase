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

namespace OloEngine
{

	static std::unordered_map<MonoType*, std::function<bool(Entity)>> s_EntityHasComponentFuncs;

#define OLO_ADD_INTERNAL_CALL(Name) mono_add_internal_call("OloEngine.InternalCalls::" #Name, Name)

	static void NativeLog(MonoString* string, int parameter)
	{
		char* cStr = ::mono_string_to_utf8(string);
		std::string str(cStr);
		::mono_free(cStr);
		std::cout << str << ", " << parameter << "\n";
	}

	static void NativeLog_Vector(glm::vec3 const* parameter, glm::vec3* outResult)
	{
		// TODO(olbu): Fix the logger, glm::vec3* is not valid type, need to provide a formatter<T> specialization
		//https://fmt.dev/latest/api.html#udt
		//OLO_CORE_WARN("Value: {0}", *parameter);
		*outResult = glm::normalize(*parameter);
	}

	[[nodiscard("Store this!")]] static float NativeLog_VectorDot(glm::vec3 const* parameter)
	{
		// TODO(olbu): Fix the logger, glm::vec3* is not valid type, need to provide a formatter<T> specialization
		//https://fmt.dev/latest/api.html#udt
		//OLO_CORE_WARN("Value: {0}", *parameter);
		return glm::dot(*parameter, *parameter);
	}

	static MonoObject* GetScriptInstance(UUID entityID)
	{
		return ScriptEngine::GetManagedInstance(entityID);
	}

	///////////////////////////////////////////////////////////////////////////////////////////
	// Entity /////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////

	Entity GetEntity(UUID entityID)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene);
		return scene->GetEntityByUUID(entityID);
	}

	static bool Entity_HasComponent(UUID entityID, MonoReflectionType* componentType)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene);
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity);

		MonoType* managedType = ::mono_reflection_type_get_type(componentType);
		OLO_CORE_ASSERT(s_EntityHasComponentFuncs.contains(managedType));
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
		{
			return 0;
		}

		return entity.GetUUID();
	}

	///////////////////////////////////////////////////////////////////////////////////////////
	// Transform //////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////

	static void TransformComponent_GetTranslation(UUID entityID, glm::vec3* outTranslation)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene);
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity);

		*outTranslation = entity.GetComponent<TransformComponent>().Translation;
	}

	static void TransformComponent_SetTranslation(UUID entityID, glm::vec3 const* translation)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene);
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity);

		entity.GetComponent<TransformComponent>().Translation = *translation;
	}

	///////////////////////////////////////////////////////////////////////////////////////////
	// Rigidbody 2D ///////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////

	static void Rigidbody2DComponent_ApplyLinearImpulse(UUID entityID, glm::vec2 const* impulse, glm::vec2 const* point, bool wake)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene);
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity);

		auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();
		auto* body = (b2Body*)rb2d.RuntimeBody;
		body->ApplyLinearImpulse(b2Vec2(impulse->x, impulse->y), b2Vec2(point->x, point->y), wake);
	}

	static void Rigidbody2DComponent_ApplyLinearImpulseToCenter(UUID entityID, glm::vec2 const* impulse, bool wake)
	{
		Scene* scene = ScriptEngine::GetSceneContext();
		OLO_CORE_ASSERT(scene);
		Entity entity = scene->GetEntityByUUID(entityID);
		OLO_CORE_ASSERT(entity);

		auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();
		auto* body = static_cast<b2Body*>(rb2d.RuntimeBody);
		body->ApplyLinearImpulseToCenter(b2Vec2(impulse->x, impulse->y), wake);
	}

	///////////////////////////////////////////////////////////////////////////////////////////
	// Audio Source ///////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////

	void AudioSourceComponent_GetVolume(uint64_t entityID, float* outVolume)
	{
		*outVolume = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.VolumeMultiplier;
	}

	void AudioSourceComponent_SetVolume(uint64_t entityID, const float* volume)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.VolumeMultiplier = *volume;
		if (component.Source)
		{
			component.Source->SetVolume(*volume);
		}
	}

	void AudioSourceComponent_GetPitch(uint64_t entityID, float* outPitch)
	{
		*outPitch = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.PitchMultiplier;
	}

	void AudioSourceComponent_SetPitch(uint64_t entityID, const float* pitch)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.PitchMultiplier = *pitch;
		if (component.Source)
		{
			component.Source->SetVolume(*pitch);
		}
	}

	void AudioSourceComponent_GetPlayOnAwake(uint64_t entityID, bool* outPlayOnAwake)
	{
		*outPlayOnAwake = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.PlayOnAwake;
	}

	void AudioSourceComponent_SetPlayOnAwake(uint64_t entityID, const bool* playOnAwake)
	{
		GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.PlayOnAwake = *playOnAwake;
	}

	void AudioSourceComponent_GetLooping(uint64_t entityID, bool* outLooping)
	{
		*outLooping = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.Looping;
	}

	void AudioSourceComponent_SetLooping(uint64_t entityID, const bool* looping)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.Looping = *looping;
		if (component.Source)
		{
			component.Source->SetLooping(*looping);
		}
	}

	void AudioSourceComponent_GetSpatialization(uint64_t entityID, bool* outSpatialization)
	{
		*outSpatialization = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.Spatialization;
	}

	void AudioSourceComponent_SetSpatialization(uint64_t entityID, const bool* spatialization)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.Spatialization = *spatialization;
		if (component.Source)
		{
			component.Source->SetSpatialization(*spatialization);
		}
	}

	void AudioSourceComponent_GetAttenuationModel(uint64_t entityID, int* outAttenuationModel)
	{
		*outAttenuationModel = (int)GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.AttenuationModel;
	}

	void AudioSourceComponent_SetAttenuationModel(uint64_t entityID, const int* attenuationModel)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.AttenuationModel = (AttenuationModelType)(*attenuationModel);
		if (component.Source)
		{
			component.Source->SetAttenuationModel(component.Config.AttenuationModel);
		}
	}

	void AudioSourceComponent_GetRollOff(uint64_t entityID, float* outRollOff)
	{
		*outRollOff = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.RollOff;
	}

	void AudioSourceComponent_SetRollOff(uint64_t entityID, const float* rollOff)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.RollOff = *rollOff;
		if (component.Source)
		{
			component.Source->SetRollOff(*rollOff);
		}
	}

	void AudioSourceComponent_GetMinGain(uint64_t entityID, float* outMinGain)
	{
		*outMinGain = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.MinGain;
	}

	void AudioSourceComponent_SetMinGain(uint64_t entityID, const float* minGain)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.MinGain = *minGain;
		if (component.Source)
		{
			component.Source->SetMinGain(*minGain);
		}
	}

	void AudioSourceComponent_GetMaxGain(uint64_t entityID, float* outMaxGain)
	{
		*outMaxGain = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.MaxGain;
	}

	void AudioSourceComponent_SetMaxGain(uint64_t entityID, const float* maxGain)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.MaxGain = *maxGain;
		if (component.Source)
		{
			component.Source->SetMaxGain(*maxGain);
		}
	}

	void AudioSourceComponent_GetMinDistance(uint64_t entityID, float* outMinDistance)
	{
		*outMinDistance = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.MinDistance;
	}

	void AudioSourceComponent_SetMinDistance(uint64_t entityID, const float* minDistance)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.MinDistance = *minDistance;
		if (component.Source)
		{
			component.Source->SetMinDistance(*minDistance);
		}
	}

	void AudioSourceComponent_GetMaxDistance(uint64_t entityID, float* outMaxDistance)
	{
		*outMaxDistance = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.MaxDistance;
	}

	void AudioSourceComponent_SetMaxDistance(uint64_t entityID, const float* maxDistance)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.MaxDistance = *maxDistance;
		if (component.Source)
		{
			component.Source->SetMaxDistance(*maxDistance);
		}
	}

	void AudioSourceComponent_GetConeInnerAngle(uint64_t entityID, float* outConeInnerAngle)
	{
		*outConeInnerAngle = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.ConeInnerAngle;
	}

	void AudioSourceComponent_SetConeInnerAngle(uint64_t entityID, const float* coneInnerAngle)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.ConeInnerAngle = *coneInnerAngle;
		if (component.Source)
		{
			component.Source->SetCone(component.Config.ConeInnerAngle, component.Config.ConeOuterAngle, component.Config.ConeOuterGain);
		}
	}

	void AudioSourceComponent_GetConeOuterAngle(uint64_t entityID, float* outConeOuterAngle)
	{
		*outConeOuterAngle = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.ConeOuterAngle;
	}

	void AudioSourceComponent_SetConeOuterAngle(uint64_t entityID, const float* coneOuterAngle)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.ConeOuterAngle = *coneOuterAngle;
		if (component.Source)
		{
			component.Source->SetCone(component.Config.ConeInnerAngle, component.Config.ConeOuterAngle, component.Config.ConeOuterGain);
		}
	}

	void AudioSourceComponent_GetConeOuterGain(uint64_t entityID, float* outConeOuterGain)
	{
		*outConeOuterGain = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.ConeOuterGain;
	}

	void AudioSourceComponent_SetConeOuterGain(uint64_t entityID, const float* coneOuterGain)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.ConeOuterGain = *coneOuterGain;
		if (component.Source)
		{
			component.Source->SetCone(component.Config.ConeInnerAngle, component.Config.ConeOuterAngle, component.Config.ConeOuterGain);
		}
	}

	void AudioSourceComponent_SetCone(uint64_t entityID, const float* coneInnerAngle, const float* coneOuterAngle, const float* coneOuterGain)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.ConeInnerAngle = *coneInnerAngle;
		component.Config.ConeOuterAngle = *coneOuterAngle;
		component.Config.ConeOuterGain = *coneOuterGain;
		if (component.Source)
		{
			component.Source->SetCone(component.Config.ConeInnerAngle, component.Config.ConeOuterAngle, component.Config.ConeOuterGain);
		}
	}

	void AudioSourceComponent_GetDopplerFactor(uint64_t entityID, float* outDopplerFactor)
	{
		{
			*outDopplerFactor = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.DopplerFactor;
		}
	}

	void AudioSourceComponent_SetDopplerFactor(uint64_t entityID, const float* dopplerFactor)
	{
		auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		component.Config.DopplerFactor = *dopplerFactor;
		if (component.Source)
		{
			component.Source->SetDopplerFactor(*dopplerFactor);
		}
	}

	void AudioSourceComponent_IsPlaying(uint64_t entityID, bool* outIsPlaying)
	{
		const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		if (component.Source)
		{
			*outIsPlaying = component.Source->IsPlaying();
		}
		else
		{
			*outIsPlaying = false;
		}
	}

	void AudioSourceComponent_Play(uint64_t entityID)
	{
		const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		if (component.Source)
		{
			component.Source->Play();
		}
	}

	void AudioSourceComponent_Pause(uint64_t entityID)
	{
		const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		if (component.Source)
		{
			component.Source->Pause();
		}
	}

	void AudioSourceComponent_UnPause(uint64_t entityID)
	{
		const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		if (component.Source)
		{
			component.Source->UnPause();
		}
	}

	void AudioSourceComponent_Stop(uint64_t entityID)
	{
		const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
		if (component.Source)
		{
			component.Source->Stop();
		}
	}

	///////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////

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
		s_EntityHasComponentFuncs.clear();
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
