#include "OloEnginePCH.h"
#include "ScriptGlue.h"
#include "ScriptEngine.h"

#if OLO_ENABLE_CSHARP_SCRIPTING

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/Gamepad.h"
#include "OloEngine/Core/GamepadManager.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Streaming/SceneStreamer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Localization/TextFormatter.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

#include <algorithm>
#include <cmath>
#include "OloEngine/Animation/MorphTargets/FacialExpressionLibrary.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageCalculation.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Audio/AudioEvents/AudioPlayback.h"
#include "OloEngine/Audio/AudioEvents/CommandID.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSound.h"

#include <mono/metadata/object.h>
#include <mono/metadata/reflection.h>

#include <box2d/box2d.h>

namespace OloEngine
{

    namespace Utils
    {
        std::string MonoStringToString(MonoString* string)
        {
            char* cStr = ::mono_string_to_utf8(string);
            std::string str(cStr);
            ::mono_free(cStr);
            return str;
        }
    } // namespace Utils

    static std::unordered_map<MonoType*, std::function<bool(Entity)>> s_EntityHasComponentFuncs;

#define OLO_ADD_INTERNAL_CALL(Name) mono_add_internal_call("OloEngine.InternalCalls::" #Name, Name)

    static void NativeLog(MonoString* string, int parameter)
    {
        std::string str = Utils::MonoStringToString(string);
        std::cout << str << ", " << parameter << "\n";
    }

    static void Log_LogMessage(i32 level, MonoString* message)
    {
        std::string msg = Utils::MonoStringToString(message);
        switch (level)
        {
            case 0:
                OLO_TRACE("{}", msg);
                break;
            case 1:
                OLO_INFO("{}", msg);
                break;
            case 2:
                OLO_WARN("{}", msg);
                break;
            case 3:
                OLO_ERROR("{}", msg);
                break;
            default:
                OLO_INFO("{}", msg);
                break;
        }
    }

    static void NativeLog_Vector(glm::vec3 const* parameter, glm::vec3* outResult)
    {
        OLO_CORE_WARN("Value: {0}", *parameter);
        *outResult = glm::normalize(*parameter);
    }

    [[nodiscard("Store this!")]] static f32 NativeLog_VectorDot(glm::vec3 const* parameter)
    {
        OLO_CORE_WARN("Value: {0}", *parameter);
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
        OLO_CORE_ASSERT(scene, "ScriptGlue::GetEntity - no active scene context");
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

    static bool Entity_IsValid(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        return scene->TryGetEntityWithUUID(entityID).has_value();
    }

    static u64 Entity_FindEntityByName(MonoString* name)
    {
        if (!name)
        {
            return 0;
        }
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

    // Auto-generated property bindings from OLO_PROPERTY() annotations
#include "Generated/ScriptGlueBindings.inl"

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
        b2Body_ApplyLinearImpulse(rb2d.RuntimeBody, b2Vec2(impulse->x, impulse->y), b2Vec2(point->x, point->y), wake);
    }

    static void Rigidbody2DComponent_ApplyLinearImpulseToCenter(UUID entityID, glm::vec2 const* impulse, bool wake)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);

        auto& rb2d = entity.GetComponent<Rigidbody2DComponent>();
        b2Body_ApplyLinearImpulseToCenter(rb2d.RuntimeBody, b2Vec2(impulse->x, impulse->y), wake);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Audio Source ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    void AudioSourceComponent_SetCone(u64 entityID, const f32* coneInnerAngle, const f32* coneOuterAngle, const f32* coneOuterGain)
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

    void AudioSourceComponent_IsPlaying(u64 entityID, bool* outIsPlaying)
    {
        const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.UseEventSystem && component.ActiveEventID != 0)
        {
            *outIsPlaying = Audio::AudioPlayback::IsEventActive(component.ActiveEventID);
        }
        else if (component.Source)
        {
            *outIsPlaying = component.Source->IsPlaying();
        }
        else
        {
            *outIsPlaying = false;
        }
    }

    void AudioSourceComponent_Play(u64 entityID)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.UseEventSystem && component.StartCommandID.IsValid())
        {
            if (component.ActiveEventID != 0)
            {
                Audio::AudioPlayback::StopEvent(component.ActiveEventID);
            }
            component.ActiveEventID = Audio::AudioPlayback::PostTrigger(component.StartCommandID, entityID);
            return;
        }
        if (component.Source)
        {
            component.Source->Play();
        }
    }

    void AudioSourceComponent_Pause(u64 entityID)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.UseEventSystem && component.ActiveEventID != 0)
        {
            Audio::AudioPlayback::PauseEvent(component.ActiveEventID);
            return;
        }
        if (component.Source)
        {
            component.Source->Pause();
        }
    }

    void AudioSourceComponent_UnPause(u64 entityID)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.UseEventSystem && component.ActiveEventID != 0)
        {
            Audio::AudioPlayback::ResumeEvent(component.ActiveEventID);
            return;
        }
        if (component.Source)
        {
            component.Source->UnPause();
        }
    }

    void AudioSourceComponent_Stop(u64 entityID)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.UseEventSystem && component.ActiveEventID != 0)
        {
            Audio::AudioPlayback::StopEvent(component.ActiveEventID);
            component.ActiveEventID = 0;
            return;
        }
        if (component.Source)
        {
            component.Source->Stop();
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Audio Sound Graph //////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    // Sound is allocated by Scene::InitAudioRuntime after the graph compiles. Before that
    // (graph not yet instantiated, or asset failed to compile) the actions silently no-op
    // and IsPlaying returns false — gameplay code can poll without crashing on early-frame
    // calls.
    void AudioSoundGraphComponent_IsPlaying(u64 entityID, bool* outIsPlaying)
    {
        const auto& component = GetEntity(entityID).GetComponent<AudioSoundGraphComponent>();
        *outIsPlaying = component.Sound && component.Sound->IsPlaying();
    }

    void AudioSoundGraphComponent_Play(u64 entityID)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSoundGraphComponent>();
        if (component.Sound)
            component.Sound->Play();
    }

    void AudioSoundGraphComponent_Stop(u64 entityID)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSoundGraphComponent>();
        if (component.Sound)
            component.Sound->Stop();
    }

    void AudioSoundGraphComponent_Pause(u64 entityID)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSoundGraphComponent>();
        if (component.Sound)
            component.Sound->Pause();
    }

    // Three typed SetParameter entry points because Mono cannot dispatch the C++
    // overload set across the P/Invoke boundary by argument type alone. The C# side
    // exposes a single `SetParameter` with overloads that pick the right call.
    static bool AudioSoundGraphComponent_SetParameterFloat(UUID entityID, MonoString* paramName, f32 value)
    {
        if (!paramName)
            return false;
        auto& component = GetEntity(entityID).GetComponent<AudioSoundGraphComponent>();
        char* name = mono_string_to_utf8(paramName);
        bool result = component.SetParameter(std::string(name), value);
        mono_free(name);
        return result;
    }

    static bool AudioSoundGraphComponent_SetParameterInt(UUID entityID, MonoString* paramName, i32 value)
    {
        if (!paramName)
            return false;
        auto& component = GetEntity(entityID).GetComponent<AudioSoundGraphComponent>();
        char* name = mono_string_to_utf8(paramName);
        bool result = component.SetParameter(std::string(name), value);
        mono_free(name);
        return result;
    }

    static bool AudioSoundGraphComponent_SetParameterBool(UUID entityID, MonoString* paramName, bool value)
    {
        if (!paramName)
            return false;
        auto& component = GetEntity(entityID).GetComponent<AudioSoundGraphComponent>();
        char* name = mono_string_to_utf8(paramName);
        bool result = component.SetParameter(std::string(name), value);
        mono_free(name);
        return result;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Audio Events ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static u64 AudioEvents_PostTrigger(MonoString* eventName, u64 objectID)
    {
        if (!eventName)
        {
            return 0;
        }
        char* name = mono_string_to_utf8(eventName);
        u64 eventID = Audio::AudioPlayback::PostTriggerByName(name, objectID);
        mono_free(name);
        return eventID;
    }

    static void AudioEvents_StopEvent(u64 eventID)
    {
        Audio::AudioPlayback::StopEvent(eventID);
    }

    static void AudioEvents_PauseEvent(u64 eventID)
    {
        Audio::AudioPlayback::PauseEvent(eventID);
    }

    static void AudioEvents_ResumeEvent(u64 eventID)
    {
        Audio::AudioPlayback::ResumeEvent(eventID);
    }

    static void AudioEvents_StopAll()
    {
        Audio::AudioPlayback::StopAll();
    }

    static bool AudioEvents_IsEventActive(u64 eventID)
    {
        return Audio::AudioPlayback::IsEventActive(eventID);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // UI Components //////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    // --- LightProbeVolumeComponent (hand-written action/query methods) ---

    static void LightProbeVolumeComponent_Dirty(UUID entityID)
    {
        GetEntity(entityID).GetComponent<LightProbeVolumeComponent>().m_Dirty = true;
    }

    static void LightProbeVolumeComponent_GetTotalProbeCount(UUID entityID, i32* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>().GetTotalProbeCount();
    }

    // --- Scene Wind Settings ---

    static void Scene_GetWindEnabled(bool* out)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        *out = scene->GetWindSettings().Enabled;
    }

    static void Scene_SetWindEnabled(bool const* v)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetWindSettings().Enabled = *v;
    }

    static void Scene_GetWindDirection(glm::vec3* out)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        *out = scene->GetWindSettings().Direction;
    }

    static void Scene_SetWindDirection(glm::vec3 const* v)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetWindSettings().Direction = *v;
    }

    static void Scene_GetWindSpeed(f32* out)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        *out = scene->GetWindSettings().Speed;
    }

    static void Scene_SetWindSpeed(f32 const* v)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetWindSettings().Speed = *v;
    }

    static void Scene_GetWindGustStrength(f32* out)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        *out = scene->GetWindSettings().GustStrength;
    }

    static void Scene_SetWindGustStrength(f32 const* v)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetWindSettings().GustStrength = *v;
    }

    static void Scene_GetWindTurbulenceIntensity(f32* out)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        *out = scene->GetWindSettings().TurbulenceIntensity;
    }

    static void Scene_SetWindTurbulenceIntensity(f32 const* v)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetWindSettings().TurbulenceIntensity = *v;
    }

    // --- StreamingVolumeComponent (hand-written query methods) ---

    static void StreamingVolumeComponent_GetIsLoaded(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<StreamingVolumeComponent>().IsLoaded;
    }

    // --- Scene Streaming ---

    static void Scene_LoadRegion(u64 regionId)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        if (auto* streamer = scene->GetSceneStreamer())
        {
            streamer->LoadRegion(UUID(regionId));
        }
    }

    static void Scene_UnloadRegion(u64 regionId)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        if (auto* streamer = scene->GetSceneStreamer())
        {
            streamer->UnloadRegion(UUID(regionId));
        }
    }

    static void Scene_GetStreamingEnabled(bool* out)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        *out = scene->GetStreamingSettings().Enabled;
    }

    static void Scene_SetStreamingEnabled(bool const* v)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetStreamingSettings().Enabled = *v;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Networking /////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool Network_IsServer()
    {
        return NetworkManager::IsServer();
    }

    static bool Network_IsClient()
    {
        return NetworkManager::IsClient();
    }

    static bool Network_IsConnected()
    {
        return NetworkManager::IsConnected();
    }

    static bool Network_Connect(MonoString* address, u16 port)
    {
        std::string addr = Utils::MonoStringToString(address);
        return NetworkManager::Connect(addr, port);
    }

    static void Network_Disconnect()
    {
        NetworkManager::Disconnect();
    }

    static bool Network_StartServer(u16 port)
    {
        return NetworkManager::StartServer(port);
    }

    static void Network_StopServer()
    {
        NetworkManager::StopServer();
    }

    // ==========================================================================
    // AnimationGraphComponent
    // ==========================================================================

    static void AnimationGraphComponent_SetFloat(UUID entityID, MonoString* paramName, f32 value)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!paramName)
        {
            return;
        }
        if (entity.HasComponent<AnimationGraphComponent>())
        {
            char* name = mono_string_to_utf8(paramName);
            entity.GetComponent<AnimationGraphComponent>().Parameters.SetFloat(name, value);
            mono_free(name);
        }
    }

    static void AnimationGraphComponent_SetBool(UUID entityID, MonoString* paramName, bool value)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!paramName)
        {
            return;
        }
        if (entity.HasComponent<AnimationGraphComponent>())
        {
            char* name = mono_string_to_utf8(paramName);
            entity.GetComponent<AnimationGraphComponent>().Parameters.SetBool(name, value);
            mono_free(name);
        }
    }

    static void AnimationGraphComponent_SetInt(UUID entityID, MonoString* paramName, i32 value)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!paramName)
        {
            return;
        }
        if (entity.HasComponent<AnimationGraphComponent>())
        {
            char* name = mono_string_to_utf8(paramName);
            entity.GetComponent<AnimationGraphComponent>().Parameters.SetInt(name, value);
            mono_free(name);
        }
    }

    static void AnimationGraphComponent_SetTrigger(UUID entityID, MonoString* paramName)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!paramName)
        {
            return;
        }
        if (entity.HasComponent<AnimationGraphComponent>())
        {
            char* name = mono_string_to_utf8(paramName);
            entity.GetComponent<AnimationGraphComponent>().Parameters.SetTrigger(name);
            mono_free(name);
        }
    }

    static f32 AnimationGraphComponent_GetFloat(UUID entityID, MonoString* paramName)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!paramName)
        {
            return 0.0f;
        }
        if (entity.HasComponent<AnimationGraphComponent>())
        {
            char* name = mono_string_to_utf8(paramName);
            f32 result = entity.GetComponent<AnimationGraphComponent>().Parameters.GetFloat(name);
            mono_free(name);
            return result;
        }
        return 0.0f;
    }

    static bool AnimationGraphComponent_GetBool(UUID entityID, MonoString* paramName)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!paramName)
        {
            return false;
        }
        if (entity.HasComponent<AnimationGraphComponent>())
        {
            char* name = mono_string_to_utf8(paramName);
            bool result = entity.GetComponent<AnimationGraphComponent>().Parameters.GetBool(name);
            mono_free(name);
            return result;
        }
        return false;
    }

    static i32 AnimationGraphComponent_GetInt(UUID entityID, MonoString* paramName)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!paramName)
        {
            return 0;
        }
        if (entity.HasComponent<AnimationGraphComponent>())
        {
            char* name = mono_string_to_utf8(paramName);
            i32 result = entity.GetComponent<AnimationGraphComponent>().Parameters.GetInt(name);
            mono_free(name);
            return result;
        }
        return 0;
    }

    static MonoString* AnimationGraphComponent_GetCurrentState(UUID entityID, i32 layerIndex)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (entity.HasComponent<AnimationGraphComponent>())
        {
            auto& graphComp = entity.GetComponent<AnimationGraphComponent>();
            if (graphComp.RuntimeGraph)
            {
                auto const& stateName = graphComp.RuntimeGraph->GetCurrentStateName(layerIndex);
                return ScriptEngine::CreateString(stateName.c_str());
            }
        }
        return ScriptEngine::CreateString("");
    }

    // ==========================================================================
    // MorphTargetComponent
    // ==========================================================================

    static void MorphTargetComponent_SetWeight(UUID entityID, MonoString* targetName, f32 weight)
    {
        OLO_PROFILE_FUNCTION();

        if (!targetName)
            return;
        auto& component = GetEntity(entityID).GetComponent<MorphTargetComponent>();
        std::string name = Utils::MonoStringToString(targetName);
        component.SetWeight(name, weight);
    }

    static f32 MorphTargetComponent_GetWeight(UUID entityID, MonoString* targetName)
    {
        OLO_PROFILE_FUNCTION();

        if (!targetName)
            return 0.0f;
        auto& component = GetEntity(entityID).GetComponent<MorphTargetComponent>();
        std::string name = Utils::MonoStringToString(targetName);
        return component.GetWeight(name);
    }

    static void MorphTargetComponent_ResetAll(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();

        auto& component = GetEntity(entityID).GetComponent<MorphTargetComponent>();
        component.ResetAllWeights();
    }

    static i32 MorphTargetComponent_GetTargetCount(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();

        auto& component = GetEntity(entityID).GetComponent<MorphTargetComponent>();
        return component.MorphTargets ? static_cast<i32>(component.MorphTargets->GetTargetCount()) : 0;
    }

    static void MorphTargetComponent_ApplyExpression(UUID entityID, MonoString* expressionName, f32 blend)
    {
        OLO_PROFILE_FUNCTION();

        if (!expressionName)
            return;
        auto& component = GetEntity(entityID).GetComponent<MorphTargetComponent>();
        std::string name = Utils::MonoStringToString(expressionName);
        FacialExpressionLibrary::ApplyExpression(component, name, blend);
    }

    // ==========================================================================
    // SaveGame
    // ==========================================================================

    static i32 SaveGame_Save(MonoString* slotName, MonoString* displayName)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        if (!scene)
        {
            return static_cast<i32>(SaveLoadResult::NoActiveScene);
        }
        if (!slotName)
        {
            return static_cast<i32>(SaveLoadResult::InvalidInput);
        }
        std::string slot = Utils::MonoStringToString(slotName);
        std::string name = displayName ? Utils::MonoStringToString(displayName) : slot;
        return static_cast<i32>(SaveGameManager::Save(*scene, slot, name));
    }

    static i32 SaveGame_Load(MonoString* slotName)
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        if (!scene)
        {
            return static_cast<i32>(SaveLoadResult::NoActiveScene);
        }
        if (!slotName)
        {
            return static_cast<i32>(SaveLoadResult::InvalidInput);
        }
        std::string slot = Utils::MonoStringToString(slotName);
        return static_cast<i32>(SaveGameManager::Load(*scene, slot));
    }

    static i32 SaveGame_QuickSave()
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        if (!scene)
        {
            return static_cast<i32>(SaveLoadResult::NoActiveScene);
        }
        return static_cast<i32>(SaveGameManager::QuickSave(*scene));
    }

    static i32 SaveGame_QuickLoad()
    {
        OLO_PROFILE_FUNCTION();

        Scene* scene = ScriptEngine::GetSceneContext();
        if (!scene)
        {
            return static_cast<i32>(SaveLoadResult::NoActiveScene);
        }
        return static_cast<i32>(SaveGameManager::QuickLoad(*scene));
    }

    static bool SaveGame_DeleteSave(MonoString* slotName)
    {
        OLO_PROFILE_FUNCTION();

        if (!slotName)
        {
            return false;
        }
        std::string slot = Utils::MonoStringToString(slotName);
        return SaveGameManager::DeleteSave(slot);
    }

    static bool SaveGame_ValidateSave(MonoString* slotName)
    {
        OLO_PROFILE_FUNCTION();

        if (!slotName)
        {
            return false;
        }
        std::string slot = Utils::MonoStringToString(slotName);
        return SaveGameManager::ValidateSave(slot);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // MaterialComponent //////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static i32 InstancedMeshComponent_GetInstanceCount(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!entity.HasComponent<InstancedMeshComponent>())
            return 0;
        return static_cast<i32>(entity.GetComponent<InstancedMeshComponent>().Instances.size());
    }

    static void InstancedMeshComponent_AddInstance(UUID entityID, glm::vec3* position, glm::vec3* eulerRotation, glm::vec3* scale, glm::vec4* color, f32 custom, i32 instanceEntityID)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (!entity.HasComponent<InstancedMeshComponent>())
            return;

        InstanceData inst;
        glm::mat4 t = glm::translate(glm::mat4(1.0f), *position);
        glm::mat4 r = glm::toMat4(glm::quat(*eulerRotation));
        glm::mat4 s = glm::scale(glm::mat4(1.0f), *scale);
        inst.Transform = t * r * s;
        inst.Normal = glm::transpose(glm::inverse(inst.Transform));
        inst.PrevTransform = inst.Transform;
        inst.Color = *color;
        inst.Custom = custom;
        inst.EntityID = instanceEntityID;
        entity.GetComponent<InstancedMeshComponent>().Instances.push_back(inst);
    }

    static void InstancedMeshComponent_ClearInstances(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (entity.HasComponent<InstancedMeshComponent>())
            entity.GetComponent<InstancedMeshComponent>().Instances.clear();
    }

    static bool InstancedMeshComponent_GetCastShadows(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        return entity.HasComponent<InstancedMeshComponent>() && entity.GetComponent<InstancedMeshComponent>().CastShadows;
    }

    static void InstancedMeshComponent_SetCastShadows(UUID entityID, bool value)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        if (entity.HasComponent<InstancedMeshComponent>())
            entity.GetComponent<InstancedMeshComponent>().CastShadows = value;
    }

    static u64 MaterialComponent_GetShaderGraphHandle(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        return static_cast<u64>(entity.GetComponent<MaterialComponent>().m_ShaderGraphHandle);
    }

    static void MaterialComponent_SetShaderGraphHandle(UUID entityID, u64 handle)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        auto& matComp = entity.GetComponent<MaterialComponent>();

        // Compile and apply the shader graph
        if (handle != 0)
        {
            if (auto graphAsset = AssetManager::GetAsset<ShaderGraphAsset>(handle))
            {
                if (auto shader = graphAsset->CompileToShader("ShaderGraph_" + std::to_string(handle)))
                {
                    matComp.m_ShaderGraphHandle = handle;
                    matComp.m_Material.SetShader(shader);
                }
                else
                {
                    OLO_CORE_WARN("ScriptGlue: ShaderGraph {} failed to compile", handle);
                }
            }
            else
            {
                OLO_CORE_WARN("ScriptGlue: ShaderGraph asset {} not found", handle);
            }
        }
        else
        {
            matComp.m_ShaderGraphHandle = 0;
            matComp.m_Material.SetShader(nullptr);
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // ShaderLibrary3D /////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool ShaderLibrary3D_LoadShader(MonoString* monoFilepath)
    {
        if (!monoFilepath)
            return false;
        auto const filepath = Utils::MonoStringToString(monoFilepath);
        auto& library = Renderer3D::GetShaderLibrary();
        auto shader = library.Load(filepath);
        return shader != nullptr;
    }

    static bool ShaderLibrary3D_Exists(MonoString* monoName)
    {
        if (!monoName)
            return false;
        auto const name = Utils::MonoStringToString(monoName);
        return Renderer3D::GetShaderLibrary().Exists(name);
    }

    static MonoString* ShaderLibrary3D_GetShaderName(MonoString* monoName)
    {
        if (!monoName)
            return ScriptEngine::CreateString("");
        auto const name = Utils::MonoStringToString(monoName);
        auto& library = Renderer3D::GetShaderLibrary();
        if (!library.Exists(name))
        {
            return ScriptEngine::CreateString("");
        }
        auto shader = library.Get(name);
        return ScriptEngine::CreateString(shader->GetName().c_str());
    }

    static void ShaderLibrary3D_ReloadAll()
    {
        Renderer3D::GetShaderLibrary().ReloadShaders();
    }

    static void ShaderLibrary3D_ReloadShader(MonoString* monoName)
    {
        if (!monoName)
            return;
        auto const name = Utils::MonoStringToString(monoName);
        auto& library = Renderer3D::GetShaderLibrary();
        if (library.Exists(name))
        {
            library.Get(name)->Reload();
        }
    }

    static u32 ShaderLibrary3D_GetShaderCount()
    {
        return Renderer3D::GetShaderLibrary().GetTotalCount();
    }

    // ShaderLibrary2D /////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool ShaderLibrary2D_LoadShader(MonoString* monoFilepath)
    {
        if (!monoFilepath)
            return false;
        auto const filepath = Utils::MonoStringToString(monoFilepath);
        auto& library = Renderer2D::GetShaderLibrary();
        auto shader = library.Load(filepath);
        return shader != nullptr;
    }

    static bool ShaderLibrary2D_Exists(MonoString* monoName)
    {
        if (!monoName)
            return false;
        auto const name = Utils::MonoStringToString(monoName);
        return Renderer2D::GetShaderLibrary().Exists(name);
    }

    static MonoString* ShaderLibrary2D_GetShaderName(MonoString* monoName)
    {
        if (!monoName)
            return ScriptEngine::CreateString("");
        auto const name = Utils::MonoStringToString(monoName);
        auto& library = Renderer2D::GetShaderLibrary();
        if (!library.Exists(name))
        {
            return ScriptEngine::CreateString("");
        }
        auto shader = library.Get(name);
        return ScriptEngine::CreateString(shader->GetName().c_str());
    }

    static void ShaderLibrary2D_ReloadAll()
    {
        Renderer2D::GetShaderLibrary().ReloadShaders();
    }

    static void ShaderLibrary2D_ReloadShader(MonoString* monoName)
    {
        if (!monoName)
            return;
        auto const name = Utils::MonoStringToString(monoName);
        auto& library = Renderer2D::GetShaderLibrary();
        if (library.Exists(name))
        {
            library.Get(name)->Reload();
        }
    }

    static u32 ShaderLibrary2D_GetShaderCount()
    {
        return Renderer2D::GetShaderLibrary().GetTotalCount();
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // NavAgentComponent (hand-written action/query methods) //////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool NavAgentComponent_HasPath(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        return entity.GetComponent<NavAgentComponent>().m_HasPath;
    }

    static void NavAgentComponent_ClearTarget(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        auto& agent = entity.GetComponent<NavAgentComponent>();
        agent.m_HasTarget = false;
        agent.m_HasPath = false;
        agent.m_PathCorners.clear();
        agent.m_CurrentCornerIndex = 0;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // UIWorldAnchorComponent /////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static u64 UIWorldAnchorComponent_GetTargetEntity(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<UIWorldAnchorComponent>());
        return static_cast<u64>(entity.GetComponent<UIWorldAnchorComponent>().m_TargetEntity);
    }

    static void UIWorldAnchorComponent_SetTargetEntity(UUID entityID, u64 targetEntityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<UIWorldAnchorComponent>());
        entity.GetComponent<UIWorldAnchorComponent>().m_TargetEntity = UUID(targetEntityID);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // IKTargetComponent //////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool IsFiniteVec3(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // Get skeleton bone count for validating indices/chain lengths.
    // Returns 0 if no skeleton is present so callers can guard correctly.
    static u32 GetSkeletonBoneCount(Entity entity)
    {
        if (entity.HasComponent<SkeletonComponent>())
        {
            auto const& skel = entity.GetComponent<SkeletonComponent>();
            if (skel.m_Skeleton)
            {
                return static_cast<u32>(skel.m_Skeleton->m_BoneNames.size());
            }
        }
        return 0u;
    }

    static bool IKTargetComponent_GetAimIKEnabled(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().AimIKEnabled;
    }

    static void IKTargetComponent_SetAimIKEnabled(UUID entityID, bool enabled)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        entity.GetComponent<IKTargetComponent>().AimIKEnabled = enabled;
    }

    static u32 IKTargetComponent_GetAimBoneIndex(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().AimBoneIndex;
    }

    static void IKTargetComponent_SetAimBoneIndex(UUID entityID, u32 index)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        auto boneCount = GetSkeletonBoneCount(entity);
        entity.GetComponent<IKTargetComponent>().AimBoneIndex = (boneCount > 0) ? std::min(index, boneCount - 1) : 0u;
    }

    static void IKTargetComponent_GetAimTarget(UUID entityID, glm::vec3* out)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        *out = entity.GetComponent<IKTargetComponent>().AimTarget;
    }

    static void IKTargetComponent_SetAimTarget(UUID entityID, glm::vec3 const* v)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        if (IsFiniteVec3(*v))
        {
            entity.GetComponent<IKTargetComponent>().AimTarget = *v;
        }
    }

    static void IKTargetComponent_GetAimAxis(UUID entityID, glm::vec3* out)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        *out = entity.GetComponent<IKTargetComponent>().AimAxis;
    }

    static void IKTargetComponent_SetAimAxis(UUID entityID, glm::vec3 const* v)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        if (IsFiniteVec3(*v) && glm::length2(*v) > 1e-8f)
        {
            entity.GetComponent<IKTargetComponent>().AimAxis = *v;
        }
    }

    static void IKTargetComponent_GetAimOffset(UUID entityID, glm::vec3* out)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        *out = entity.GetComponent<IKTargetComponent>().AimOffset;
    }

    static void IKTargetComponent_SetAimOffset(UUID entityID, glm::vec3 const* v)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        if (IsFiniteVec3(*v))
        {
            entity.GetComponent<IKTargetComponent>().AimOffset = *v;
        }
    }

    static void IKTargetComponent_GetAimPoleVector(UUID entityID, glm::vec3* out)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        *out = entity.GetComponent<IKTargetComponent>().AimPoleVector;
    }

    static void IKTargetComponent_SetAimPoleVector(UUID entityID, glm::vec3 const* v)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        if (IsFiniteVec3(*v) && glm::length2(*v) > 1e-8f)
        {
            entity.GetComponent<IKTargetComponent>().AimPoleVector = *v;
        }
    }

    static u32 IKTargetComponent_GetAimChainLength(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().AimChainLength;
    }

    static void IKTargetComponent_SetAimChainLength(UUID entityID, u32 length)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        entity.GetComponent<IKTargetComponent>().AimChainLength = std::clamp(length, 1u, GetSkeletonBoneCount(entity));
    }

    static f32 IKTargetComponent_GetAimChainFactor(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().AimChainFactor;
    }

    static void IKTargetComponent_SetAimChainFactor(UUID entityID, f32 factor)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        if (std::isfinite(factor))
        {
            entity.GetComponent<IKTargetComponent>().AimChainFactor = glm::clamp(factor, 0.0f, 1.0f);
        }
    }

    static u64 IKTargetComponent_GetAimTargetEntity(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return static_cast<u64>(entity.GetComponent<IKTargetComponent>().AimTargetEntity);
    }

    static void IKTargetComponent_SetAimTargetEntity(UUID entityID, u64 targetID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        entity.GetComponent<IKTargetComponent>().AimTargetEntity = targetID;
    }

    static f32 IKTargetComponent_GetAimWeight(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().AimWeight;
    }

    static void IKTargetComponent_SetAimWeight(UUID entityID, f32 weight)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        if (std::isfinite(weight))
        {
            entity.GetComponent<IKTargetComponent>().AimWeight = glm::clamp(weight, 0.0f, 1.0f);
        }
    }

    static bool IKTargetComponent_GetLimbIKEnabled(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().LimbIKEnabled;
    }

    static void IKTargetComponent_SetLimbIKEnabled(UUID entityID, bool enabled)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        entity.GetComponent<IKTargetComponent>().LimbIKEnabled = enabled;
    }

    static u32 IKTargetComponent_GetLimbBoneIndex(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().LimbBoneIndex;
    }

    static void IKTargetComponent_SetLimbBoneIndex(UUID entityID, u32 index)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        auto boneCount = GetSkeletonBoneCount(entity);
        entity.GetComponent<IKTargetComponent>().LimbBoneIndex = (boneCount > 0) ? std::min(index, boneCount - 1) : 0u;
    }

    static void IKTargetComponent_GetLimbTarget(UUID entityID, glm::vec3* out)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        *out = entity.GetComponent<IKTargetComponent>().LimbTarget;
    }

    static void IKTargetComponent_SetLimbTarget(UUID entityID, glm::vec3 const* v)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        if (IsFiniteVec3(*v))
        {
            entity.GetComponent<IKTargetComponent>().LimbTarget = *v;
        }
    }

    static u32 IKTargetComponent_GetLimbChainLength(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().LimbChainLength;
    }

    static void IKTargetComponent_SetLimbChainLength(UUID entityID, u32 length)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        entity.GetComponent<IKTargetComponent>().LimbChainLength = std::clamp(length, 1u, GetSkeletonBoneCount(entity));
    }

    static u64 IKTargetComponent_GetLimbTargetEntity(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return static_cast<u64>(entity.GetComponent<IKTargetComponent>().LimbTargetEntity);
    }

    static void IKTargetComponent_SetLimbTargetEntity(UUID entityID, u64 targetID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        entity.GetComponent<IKTargetComponent>().LimbTargetEntity = targetID;
    }

    static f32 IKTargetComponent_GetLimbWeight(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        return entity.GetComponent<IKTargetComponent>().LimbWeight;
    }

    static void IKTargetComponent_SetLimbWeight(UUID entityID, f32 weight)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<IKTargetComponent>());
        if (std::isfinite(weight))
        {
            entity.GetComponent<IKTargetComponent>().LimbWeight = glm::clamp(weight, 0.0f, 1.0f);
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Dialogue ///////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static void DialogueComponent_StartDialogue(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        auto* dialogueSystem = scene->GetDialogueSystem();
        if (dialogueSystem)
            dialogueSystem->StartDialogue(entity);
    }

    static void DialogueComponent_AdvanceDialogue(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        auto* dialogueSystem = scene->GetDialogueSystem();
        if (dialogueSystem)
            dialogueSystem->AdvanceDialogue(entity);
    }

    static void DialogueComponent_SelectChoice(UUID entityID, i32 choiceIndex)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        auto* dialogueSystem = scene->GetDialogueSystem();
        if (dialogueSystem)
            dialogueSystem->SelectChoice(entity, choiceIndex);
    }

    static bool DialogueComponent_IsDialogueActive(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        return entity.HasComponent<DialogueStateComponent>();
    }

    static void DialogueComponent_EndDialogue(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        auto* dialogueSystem = scene->GetDialogueSystem();
        if (dialogueSystem)
            dialogueSystem->EndDialogue(entity);
    }

    static u64 DialogueComponent_GetDialogueTree(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        return static_cast<u64>(entity.GetComponent<DialogueComponent>().m_DialogueTree);
    }

    static void DialogueComponent_SetDialogueTree(UUID entityID, u64 handle)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        entity.GetComponent<DialogueComponent>().m_DialogueTree = handle;
    }

    static bool DialogueComponent_GetAutoTrigger(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        return entity.GetComponent<DialogueComponent>().m_AutoTrigger;
    }

    static void DialogueComponent_SetAutoTrigger(UUID entityID, bool value)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        entity.GetComponent<DialogueComponent>().m_AutoTrigger = value;
    }

    static f32 DialogueComponent_GetTriggerRadius(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        return entity.GetComponent<DialogueComponent>().m_TriggerRadius;
    }

    static void DialogueComponent_SetTriggerRadius(UUID entityID, f32 value)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        entity.GetComponent<DialogueComponent>().m_TriggerRadius = value;
    }

    static bool DialogueComponent_GetTriggerOnce(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        return entity.GetComponent<DialogueComponent>().m_TriggerOnce;
    }

    static void DialogueComponent_SetTriggerOnce(UUID entityID, bool value)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        entity.GetComponent<DialogueComponent>().m_TriggerOnce = value;
    }

    static bool DialogueComponent_GetHasTriggered(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        return entity.GetComponent<DialogueComponent>().m_HasTriggered;
    }

    static void DialogueComponent_SetHasTriggered(UUID entityID, bool value)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        entity.GetComponent<DialogueComponent>().m_HasTriggered = value;
    }

    static bool DialogueVariables_GetBool(MonoString* key, bool defaultValue)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return defaultValue;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        return scene->GetDialogueVariables().GetBool(Utils::MonoStringToString(key), defaultValue);
    }

    static void DialogueVariables_SetBool(MonoString* key, bool value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetDialogueVariables().SetBool(Utils::MonoStringToString(key), value);
    }

    static i32 DialogueVariables_GetInt(MonoString* key, i32 defaultValue)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return defaultValue;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        return scene->GetDialogueVariables().GetInt(Utils::MonoStringToString(key), defaultValue);
    }

    static void DialogueVariables_SetInt(MonoString* key, i32 value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetDialogueVariables().SetInt(Utils::MonoStringToString(key), value);
    }

    static f32 DialogueVariables_GetFloat(MonoString* key, f32 defaultValue)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return defaultValue;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        return scene->GetDialogueVariables().GetFloat(Utils::MonoStringToString(key), defaultValue);
    }

    static void DialogueVariables_SetFloat(MonoString* key, f32 value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetDialogueVariables().SetFloat(Utils::MonoStringToString(key), value);
    }

    static MonoString* DialogueVariables_GetString(MonoString* key, MonoString* defaultValue)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return defaultValue;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        std::string result = scene->GetDialogueVariables().GetString(
            Utils::MonoStringToString(key),
            defaultValue ? Utils::MonoStringToString(defaultValue) : "");
        return ScriptEngine::CreateString(result.c_str());
    }

    static void DialogueVariables_SetString(MonoString* key, MonoString* value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetDialogueVariables().SetString(
            Utils::MonoStringToString(key),
            value ? Utils::MonoStringToString(value) : "");
    }

    static bool DialogueVariables_Has(MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return false;
        }
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        return scene->GetDialogueVariables().Has(Utils::MonoStringToString(key));
    }

    static void DialogueVariables_Clear()
    {
        OLO_PROFILE_FUNCTION();
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        scene->GetDialogueVariables().Clear();
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool Input_IsKeyDown(KeyCode keycode)
    {
        return Input::IsKeyPressed(keycode);
    }

    static bool Input_IsKeyJustPressed(KeyCode keycode)
    {
        return Input::IsKeyJustPressed(keycode);
    }

    static bool Input_IsKeyJustReleased(KeyCode keycode)
    {
        return Input::IsKeyJustReleased(keycode);
    }

    static void Input_GetMousePosition(glm::vec2* outPosition)
    {
        glm::vec2 raw = Input::GetMousePosition();
        // In the editor the game viewport is an ImGui sub-window; subtract its
        // origin so scripts see coordinates relative to the viewport — matching
        // the size returned by Input_GetWindowSize.
        if (Scene* scene = ScriptEngine::GetSceneContext(); scene)
        {
            raw -= scene->GetViewportOffset();
        }
        *outPosition = raw;
    }

    static void Input_GetWindowSize(glm::vec2* outSize)
    {
        // Return the game viewport size (not the host window), so scripts
        // normalising mouse coordinates work in both editor PIE and standalone.
        if (Scene* scene = ScriptEngine::GetSceneContext(); scene && scene->GetViewportWidth() > 0)
        {
            *outSize = { static_cast<f32>(scene->GetViewportWidth()), static_cast<f32>(scene->GetViewportHeight()) };
            return;
        }
        auto& window = Application::Get().GetWindow();
        *outSize = { static_cast<f32>(window.GetWidth()), static_cast<f32>(window.GetHeight()) };
    }

    static bool Input_IsMouseButtonDown(i32 button)
    {
        return Input::IsMouseButtonPressed(static_cast<MouseCode>(button));
    }

    static bool Input_IsGamepadButtonPressed(u8 button, i32 gamepadIndex)
    {
        if (button >= Gamepad::ButtonCount)
        {
            return false;
        }
        auto* gp = GamepadManager::GetGamepad(gamepadIndex);
        return gp && gp->IsButtonPressed(static_cast<GamepadButton>(button));
    }

    static bool Input_IsGamepadButtonJustPressed(u8 button, i32 gamepadIndex)
    {
        if (button >= Gamepad::ButtonCount)
        {
            return false;
        }
        auto* gp = GamepadManager::GetGamepad(gamepadIndex);
        return gp && gp->IsButtonJustPressed(static_cast<GamepadButton>(button));
    }

    static bool Input_IsGamepadButtonJustReleased(u8 button, i32 gamepadIndex)
    {
        if (button >= Gamepad::ButtonCount)
        {
            return false;
        }
        auto* gp = GamepadManager::GetGamepad(gamepadIndex);
        return gp && gp->IsButtonJustReleased(static_cast<GamepadButton>(button));
    }

    static f32 Input_GetGamepadAxis(u8 axis, i32 gamepadIndex)
    {
        if (axis >= Gamepad::AxisCount)
        {
            return 0.0f;
        }
        auto* gp = GamepadManager::GetGamepad(gamepadIndex);
        return gp ? gp->GetAxis(static_cast<GamepadAxis>(axis)) : 0.0f;
    }

    static void Input_GetGamepadLeftStick(i32 gamepadIndex, glm::vec2* outStick)
    {
        auto* gp = GamepadManager::GetGamepad(gamepadIndex);
        *outStick = gp ? gp->GetLeftStickDeadzone() : glm::vec2(0.0f);
    }

    static void Input_GetGamepadRightStick(i32 gamepadIndex, glm::vec2* outStick)
    {
        auto* gp = GamepadManager::GetGamepad(gamepadIndex);
        *outStick = gp ? gp->GetRightStickDeadzone() : glm::vec2(0.0f);
    }

    static bool Input_IsGamepadConnected(i32 gamepadIndex)
    {
        auto* gp = GamepadManager::GetGamepad(gamepadIndex);
        return gp && gp->IsConnected();
    }

    static i32 Input_GetGamepadConnectedCount()
    {
        return GamepadManager::GetConnectedCount();
    }

    static f32 Input_GetActionAxisValue(MonoString* actionName)
    {
        OLO_PROFILE_FUNCTION();

        if (!actionName)
        {
            return 0.0f;
        }
        char* name = mono_string_to_utf8(actionName);
        f32 result = InputActionManager::GetActionAxisValue(name);
        mono_free(name);
        return result;
    }

    static bool Input_IsActionPressed(MonoString* actionName)
    {
        OLO_PROFILE_FUNCTION();

        if (!actionName)
        {
            return false;
        }
        char* name = mono_string_to_utf8(actionName);
        bool result = InputActionManager::IsActionPressed(name);
        mono_free(name);
        return result;
    }

    static bool Input_IsActionJustPressed(MonoString* actionName)
    {
        OLO_PROFILE_FUNCTION();

        if (!actionName)
        {
            return false;
        }
        char* name = mono_string_to_utf8(actionName);
        bool result = InputActionManager::IsActionJustPressed(name);
        mono_free(name);
        return result;
    }

    static bool Input_IsActionJustReleased(MonoString* actionName)
    {
        OLO_PROFILE_FUNCTION();

        if (!actionName)
        {
            return false;
        }
        char* name = mono_string_to_utf8(actionName);
        bool result = InputActionManager::IsActionJustReleased(name);
        mono_free(name);
        return result;
    }

    template<typename... Component>
    static void RegisterComponent()
    {
        ([]()
         {
			std::string_view typeName = typeid(Component).name();
			sizet pos = typeName.find_last_of(':');
			std::string_view structName = typeName.substr(pos + 1);
			std::string structNameStr(structName);

			MonoClass* managedClass = ::mono_class_from_name(ScriptEngine::GetCoreAssemblyImage(), "OloEngine", structNameStr.c_str());
			if (!managedClass)
			{
				OLO_CORE_TRACE("No C# binding for component type OloEngine.{} (skipped)", structNameStr);
				return;
			}
			MonoType* managedType = ::mono_class_get_type(managedClass);
			s_EntityHasComponentFuncs[managedType] = [](Entity entity) { return entity.HasComponent<Component>(); }; }(), ...);
    }

    template<typename... Component>
    static void RegisterComponent(ComponentGroup<Component...>)
    {
        s_EntityHasComponentFuncs.clear();
        RegisterComponent<Component...>();
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // BehaviorTreeComponent //////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static void BehaviorTreeComponent_SetBlackboardBool(UUID entityID, MonoString* key, bool value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        entity.GetComponent<BehaviorTreeComponent>().Blackboard.Set(Utils::MonoStringToString(key), value);
    }

    static bool BehaviorTreeComponent_GetBlackboardBool(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        return entity.GetComponent<BehaviorTreeComponent>().Blackboard.Get<bool>(Utils::MonoStringToString(key));
    }

    static void BehaviorTreeComponent_SetBlackboardInt(UUID entityID, MonoString* key, i32 value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        entity.GetComponent<BehaviorTreeComponent>().Blackboard.Set(Utils::MonoStringToString(key), value);
    }

    static i32 BehaviorTreeComponent_GetBlackboardInt(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return 0;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        return entity.GetComponent<BehaviorTreeComponent>().Blackboard.Get<i32>(Utils::MonoStringToString(key));
    }

    static void BehaviorTreeComponent_SetBlackboardFloat(UUID entityID, MonoString* key, f32 value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        entity.GetComponent<BehaviorTreeComponent>().Blackboard.Set(Utils::MonoStringToString(key), value);
    }

    static f32 BehaviorTreeComponent_GetBlackboardFloat(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return 0.0f;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        return entity.GetComponent<BehaviorTreeComponent>().Blackboard.Get<f32>(Utils::MonoStringToString(key));
    }

    static void BehaviorTreeComponent_SetBlackboardString(UUID entityID, MonoString* key, MonoString* value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key || !value)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        entity.GetComponent<BehaviorTreeComponent>().Blackboard.Set(
            Utils::MonoStringToString(key), Utils::MonoStringToString(value));
    }

    static MonoString* BehaviorTreeComponent_GetBlackboardString(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return ScriptEngine::CreateString("");
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        auto val = entity.GetComponent<BehaviorTreeComponent>().Blackboard.Get<std::string>(Utils::MonoStringToString(key));
        return ScriptEngine::CreateString(val.c_str());
    }

    static void BehaviorTreeComponent_SetBlackboardVec3(UUID entityID, MonoString* key, glm::vec3 const* value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        entity.GetComponent<BehaviorTreeComponent>().Blackboard.Set(Utils::MonoStringToString(key), *value);
    }

    static void BehaviorTreeComponent_GetBlackboardVec3(UUID entityID, MonoString* key, glm::vec3* outResult)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            *outResult = {};
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        *outResult = entity.GetComponent<BehaviorTreeComponent>().Blackboard.Get<glm::vec3>(Utils::MonoStringToString(key));
    }

    static void BehaviorTreeComponent_RemoveBlackboardKey(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        entity.GetComponent<BehaviorTreeComponent>().Blackboard.Remove(Utils::MonoStringToString(key));
    }

    static bool BehaviorTreeComponent_HasBlackboardKey(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        return entity.GetComponent<BehaviorTreeComponent>().Blackboard.Has(Utils::MonoStringToString(key));
    }

    static bool BehaviorTreeComponent_IsRunning(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<BehaviorTreeComponent>());
        return entity.GetComponent<BehaviorTreeComponent>().IsRunning;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // StateMachineComponent //////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static void StateMachineComponent_SetBlackboardBool(UUID entityID, MonoString* key, bool value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        entity.GetComponent<StateMachineComponent>().Blackboard.Set(Utils::MonoStringToString(key), value);
    }

    static bool StateMachineComponent_GetBlackboardBool(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        return entity.GetComponent<StateMachineComponent>().Blackboard.Get<bool>(Utils::MonoStringToString(key));
    }

    static void StateMachineComponent_SetBlackboardInt(UUID entityID, MonoString* key, i32 value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        entity.GetComponent<StateMachineComponent>().Blackboard.Set(Utils::MonoStringToString(key), value);
    }

    static i32 StateMachineComponent_GetBlackboardInt(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return 0;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        return entity.GetComponent<StateMachineComponent>().Blackboard.Get<i32>(Utils::MonoStringToString(key));
    }

    static void StateMachineComponent_SetBlackboardFloat(UUID entityID, MonoString* key, f32 value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        entity.GetComponent<StateMachineComponent>().Blackboard.Set(Utils::MonoStringToString(key), value);
    }

    static f32 StateMachineComponent_GetBlackboardFloat(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return 0.0f;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        return entity.GetComponent<StateMachineComponent>().Blackboard.Get<f32>(Utils::MonoStringToString(key));
    }

    static void StateMachineComponent_SetBlackboardString(UUID entityID, MonoString* key, MonoString* value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key || !value)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        entity.GetComponent<StateMachineComponent>().Blackboard.Set(
            Utils::MonoStringToString(key), Utils::MonoStringToString(value));
    }

    static MonoString* StateMachineComponent_GetBlackboardString(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return ScriptEngine::CreateString("");
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        auto val = entity.GetComponent<StateMachineComponent>().Blackboard.Get<std::string>(Utils::MonoStringToString(key));
        return ScriptEngine::CreateString(val.c_str());
    }

    static void StateMachineComponent_SetBlackboardVec3(UUID entityID, MonoString* key, glm::vec3 const* value)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        entity.GetComponent<StateMachineComponent>().Blackboard.Set(Utils::MonoStringToString(key), *value);
    }

    static void StateMachineComponent_GetBlackboardVec3(UUID entityID, MonoString* key, glm::vec3* outResult)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            *outResult = {};
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        *outResult = entity.GetComponent<StateMachineComponent>().Blackboard.Get<glm::vec3>(Utils::MonoStringToString(key));
    }

    static void StateMachineComponent_RemoveBlackboardKey(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        entity.GetComponent<StateMachineComponent>().Blackboard.Remove(Utils::MonoStringToString(key));
    }

    static bool StateMachineComponent_HasBlackboardKey(UUID entityID, MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        return entity.GetComponent<StateMachineComponent>().Blackboard.Has(Utils::MonoStringToString(key));
    }

    static MonoString* StateMachineComponent_GetCurrentState(UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        auto& smc = entity.GetComponent<StateMachineComponent>();
        if (smc.RuntimeFSM && smc.RuntimeFSM->IsStarted())
        {
            return ScriptEngine::CreateString(smc.RuntimeFSM->GetCurrentStateID().c_str());
        }
        return ScriptEngine::CreateString("");
    }

    static void StateMachineComponent_ForceTransition(UUID entityID, MonoString* stateId)
    {
        OLO_PROFILE_FUNCTION();
        if (!stateId)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<StateMachineComponent>());
        auto& smc = entity.GetComponent<StateMachineComponent>();
        if (smc.RuntimeFSM)
        {
            smc.RuntimeFSM->ForceTransition(Utils::MonoStringToString(stateId), entity, smc.Blackboard);
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Inventory //////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool InventoryComponent_AddItem(UUID entityID, MonoString* itemId, i32 count)
    {
        if (!itemId || count <= 0)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<InventoryComponent>());
        auto& ic = entity.GetComponent<InventoryComponent>();
        std::string id = Utils::MonoStringToString(itemId);

        const auto* def = ItemDatabase::Get(id);
        if (!def)
        {
            return false;
        }
        i32 maxStack = std::max(def->MaxStackSize, 1);

        // Split count into multiple instances respecting MaxStackSize
        i32 remaining = count;
        while (remaining > 0)
        {
            ItemInstance instance;
            instance.InstanceID = UUID();
            instance.ItemDefinitionID = id;
            instance.StackCount = std::min(remaining, maxStack);
            if (!ic.PlayerInventory.AddItem(instance))
            {
                return false;
            }
            remaining -= instance.StackCount;
        }
        return true;
    }

    static bool InventoryComponent_RemoveItem(UUID entityID, MonoString* itemId, i32 count)
    {
        if (!itemId || count <= 0)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<InventoryComponent>());
        auto& ic = entity.GetComponent<InventoryComponent>();
        return ic.PlayerInventory.RemoveItemByDefinition(Utils::MonoStringToString(itemId), count);
    }

    static bool InventoryComponent_HasItem(UUID entityID, MonoString* itemId, i32 count)
    {
        if (!itemId || count <= 0)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<InventoryComponent>());
        auto& ic = entity.GetComponent<InventoryComponent>();
        return ic.PlayerInventory.HasItem(Utils::MonoStringToString(itemId), count);
    }

    static i32 InventoryComponent_CountItem(UUID entityID, MonoString* itemId)
    {
        if (!itemId)
        {
            return 0;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<InventoryComponent>());
        auto& ic = entity.GetComponent<InventoryComponent>();
        return ic.PlayerInventory.CountItem(Utils::MonoStringToString(itemId));
    }

    static i32 InventoryComponent_GetCurrency(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<InventoryComponent>());
        return entity.GetComponent<InventoryComponent>().Currency;
    }

    static void InventoryComponent_SetCurrency(UUID entityID, i32 value)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<InventoryComponent>());
        entity.GetComponent<InventoryComponent>().Currency = value;
    }

    ///////////////////////////////////////////////////////////////////////////
    // Quest //////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////

    static bool QuestJournalComponent_AcceptQuest(UUID entityID, MonoString* questIdStr)
    {
        if (!questIdStr)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        std::string questId = Utils::MonoStringToString(questIdStr);
        const auto* def = QuestDatabase::Get(questId);
        if (!def)
        {
            return false;
        }
        return entity.GetComponent<QuestJournalComponent>().Journal.AcceptQuest(questId, *def);
    }

    static bool QuestJournalComponent_AbandonQuest(UUID entityID, MonoString* questIdStr)
    {
        if (!questIdStr)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return entity.GetComponent<QuestJournalComponent>().Journal.AbandonQuest(Utils::MonoStringToString(questIdStr));
    }

    static bool QuestJournalComponent_CompleteQuest(UUID entityID, MonoString* questIdStr, MonoString* branchStr)
    {
        if (!questIdStr)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        std::string branch = branchStr ? Utils::MonoStringToString(branchStr) : "";
        return entity.GetComponent<QuestJournalComponent>().Journal.CompleteQuest(Utils::MonoStringToString(questIdStr), branch).has_value();
    }

    static bool QuestJournalComponent_IsQuestActive(UUID entityID, MonoString* questIdStr)
    {
        if (!questIdStr)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return entity.GetComponent<QuestJournalComponent>().Journal.IsQuestActive(Utils::MonoStringToString(questIdStr));
    }

    static bool QuestJournalComponent_HasCompletedQuest(UUID entityID, MonoString* questIdStr)
    {
        if (!questIdStr)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return entity.GetComponent<QuestJournalComponent>().Journal.HasCompletedQuest(Utils::MonoStringToString(questIdStr));
    }

    static void QuestJournalComponent_IncrementObjective(UUID entityID, MonoString* questIdStr, MonoString* objectiveIdStr, i32 amount)
    {
        if (!questIdStr || !objectiveIdStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.IncrementObjective(
            Utils::MonoStringToString(questIdStr), Utils::MonoStringToString(objectiveIdStr), amount);
    }

    static void QuestJournalComponent_NotifyKill(UUID entityID, MonoString* targetTagStr)
    {
        if (!targetTagStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.NotifyKill(Utils::MonoStringToString(targetTagStr));
    }

    static void QuestJournalComponent_NotifyCollect(UUID entityID, MonoString* itemIdStr, i32 count)
    {
        if (!itemIdStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.NotifyCollect(Utils::MonoStringToString(itemIdStr), count);
    }

    static void QuestJournalComponent_NotifyInteract(UUID entityID, MonoString* interactableIdStr)
    {
        if (!interactableIdStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.NotifyInteract(Utils::MonoStringToString(interactableIdStr));
    }

    static void QuestJournalComponent_NotifyReachLocation(UUID entityID, MonoString* locationIdStr)
    {
        if (!locationIdStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.NotifyReachLocation(Utils::MonoStringToString(locationIdStr));
    }

    static void QuestJournalComponent_SetPlayerLevel(UUID entityID, i32 level)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.SetPlayerLevel(level);
    }

    static i32 QuestJournalComponent_GetPlayerLevel(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return entity.GetComponent<QuestJournalComponent>().Journal.GetPlayerLevel();
    }

    static void QuestJournalComponent_SetReputation(UUID entityID, MonoString* factionStr, i32 value)
    {
        if (!factionStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.SetReputation(Utils::MonoStringToString(factionStr), value);
    }

    static i32 QuestJournalComponent_GetReputation(UUID entityID, MonoString* factionStr)
    {
        if (!factionStr)
        {
            return 0;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return entity.GetComponent<QuestJournalComponent>().Journal.GetReputation(Utils::MonoStringToString(factionStr));
    }

    static void QuestJournalComponent_SetItemCount(UUID entityID, MonoString* itemStr, i32 count)
    {
        if (!itemStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.SetItemCount(Utils::MonoStringToString(itemStr), count);
    }

    static void QuestJournalComponent_SetStat(UUID entityID, MonoString* statStr, i32 value)
    {
        if (!statStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.SetStat(Utils::MonoStringToString(statStr), value);
    }

    static void QuestJournalComponent_SetPlayerClass(UUID entityID, MonoString* classStr)
    {
        if (!classStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.SetPlayerClass(Utils::MonoStringToString(classStr));
    }

    static void QuestJournalComponent_SetPlayerFaction(UUID entityID, MonoString* factionStr)
    {
        if (!factionStr)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        entity.GetComponent<QuestJournalComponent>().Journal.SetPlayerFaction(Utils::MonoStringToString(factionStr));
    }

    static i32 QuestJournalComponent_GetItemCount(UUID entityID, MonoString* itemStr)
    {
        if (!itemStr)
        {
            return 0;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return entity.GetComponent<QuestJournalComponent>().Journal.GetItemCount(Utils::MonoStringToString(itemStr));
    }

    static i32 QuestJournalComponent_GetStat(UUID entityID, MonoString* statStr)
    {
        if (!statStr)
        {
            return 0;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return entity.GetComponent<QuestJournalComponent>().Journal.GetStat(Utils::MonoStringToString(statStr));
    }

    static MonoString* QuestJournalComponent_GetPlayerClass(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return ScriptEngine::CreateString(entity.GetComponent<QuestJournalComponent>().Journal.GetPlayerClass().c_str());
    }

    static MonoString* QuestJournalComponent_GetPlayerFaction(UUID entityID)
    {
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<QuestJournalComponent>());
        return ScriptEngine::CreateString(entity.GetComponent<QuestJournalComponent>().Journal.GetPlayerFaction().c_str());
    }

    // AbilityComponent bindings
    static f32 AbilityComponent_GetAttribute(UUID entityID, MonoString* name)
    {
        if (!name)
        {
            return 0.0f;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<AbilityComponent>());
        char* nameStr = mono_string_to_utf8(name);
        f32 result = entity.GetComponent<AbilityComponent>().Attributes.GetBaseValue(nameStr);
        mono_free(nameStr);
        return result;
    }

    static void AbilityComponent_SetAttribute(UUID entityID, MonoString* name, f32 value)
    {
        if (!name)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<AbilityComponent>());
        char* nameStr = mono_string_to_utf8(name);
        entity.GetComponent<AbilityComponent>().Attributes.SetBaseValue(nameStr, value);
        mono_free(nameStr);
    }

    static f32 AbilityComponent_GetCurrentAttribute(UUID entityID, MonoString* name)
    {
        if (!name)
        {
            return 0.0f;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<AbilityComponent>());
        char* nameStr = mono_string_to_utf8(name);
        f32 result = entity.GetComponent<AbilityComponent>().Attributes.GetCurrentValue(nameStr);
        mono_free(nameStr);
        return result;
    }

    static bool AbilityComponent_TryActivateAbility(UUID entityID, MonoString* abilityTag)
    {
        if (!abilityTag)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<AbilityComponent>());
        char* tagStr = mono_string_to_utf8(abilityTag);
        GameplayTag tag(tagStr);
        mono_free(tagStr);

        Scene* scene = ScriptEngine::GetSceneContext();
        return GameplayAbilitySystem::TryActivateAbility(scene, entity, tag);
    }

    static bool AbilityComponent_HasTag(UUID entityID, MonoString* tag)
    {
        if (!tag)
        {
            return false;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<AbilityComponent>());
        char* tagStr = mono_string_to_utf8(tag);
        bool result = entity.GetComponent<AbilityComponent>().OwnedTags.HasTagExact(GameplayTag(tagStr));
        mono_free(tagStr);
        return result;
    }

    static void AbilityComponent_AddTag(UUID entityID, MonoString* tag)
    {
        if (!tag)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<AbilityComponent>());
        char* tagStr = mono_string_to_utf8(tag);
        entity.GetComponent<AbilityComponent>().OwnedTags.AddTag(GameplayTag(tagStr));
        mono_free(tagStr);
    }

    static void AbilityComponent_RemoveTag(UUID entityID, MonoString* tag)
    {
        if (!tag)
        {
            return;
        }
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<AbilityComponent>());
        char* tagStr = mono_string_to_utf8(tag);
        entity.GetComponent<AbilityComponent>().OwnedTags.RemoveTag(GameplayTag(tagStr));
        mono_free(tagStr);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Physics raycast ////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool Physics_Raycast(glm::vec3* origin, glm::vec3* direction, f32 maxDistance,
                                glm::vec3* outHitPosition, glm::vec3* outHitNormal, f32* outDistance, u64* outEntityID)
    {
        // Zero-init out params so managed code never sees garbage on early return
        *outHitPosition = {};
        *outHitNormal = {};
        *outDistance = 0.0f;
        *outEntityID = 0;

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);

        JoltScene* joltScene = scene->GetPhysicsScene();
        if (!joltScene)
        {
            return false;
        }

        RayCastInfo rayInfo(*origin, *direction, maxDistance);
        SceneQueryHit hit;
        if (!joltScene->CastRay(rayInfo, hit))
        {
            return false;
        }

        *outHitPosition = hit.m_Position;
        *outHitNormal = hit.m_Normal;
        *outDistance = hit.m_Distance;
        *outEntityID = static_cast<u64>(hit.m_HitEntity);
        return true;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Camera.ScreenToWorldRay ////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static bool Camera_ScreenToWorldRay(UUID cameraEntityID, glm::vec2* screenPos, glm::vec3* outOrigin, glm::vec3* outDirection)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);

        Entity cameraEntity = scene->GetEntityByUUID(cameraEntityID);
        if (!cameraEntity || !cameraEntity.HasComponent<CameraComponent>())
        {
            *outOrigin = glm::vec3(0.0f);
            *outDirection = glm::vec3(0.0f, 0.0f, -1.0f);
            return false;
        }

        auto const& cameraComp = cameraEntity.GetComponent<CameraComponent>();
        auto const& transform = cameraEntity.GetComponent<TransformComponent>();

        // Build inverse view-projection matrix
        glm::mat4 viewMatrix = glm::inverse(transform.GetTransform());
        glm::mat4 projMatrix = cameraComp.Camera.GetProjection();
        glm::mat4 invVP = glm::inverse(projMatrix * viewMatrix);

        // Convert screen coords to NDC [-1, 1]
        f32 ndcX = screenPos->x * 2.0f - 1.0f;
        f32 ndcY = screenPos->y * 2.0f - 1.0f;

        // Unproject near and far points
        glm::vec4 nearPoint = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
        glm::vec4 farPoint = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);

        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;

        *outOrigin = glm::vec3(nearPoint);
        *outDirection = glm::normalize(glm::vec3(farPoint - nearPoint));
        return true;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Damage routing (cross-entity) //////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static f32 AbilityComponent_ApplyDamageToTarget(UUID sourceEntityID, UUID targetEntityID,
                                                    f32 rawDamage, MonoString* damageTypeTag, bool isCritical)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);

        Entity source = scene->GetEntityByUUID(sourceEntityID);
        Entity target = scene->GetEntityByUUID(targetEntityID);
        if (!source || !target)
        {
            return 0.0f;
        }

        if (!source.HasComponent<AbilityComponent>() || !target.HasComponent<AbilityComponent>())
        {
            return 0.0f;
        }

        auto const& sourceAC = source.GetComponent<AbilityComponent>();
        auto& targetAC = target.GetComponent<AbilityComponent>();

        DamageEvent event;
        event.Source = source;
        event.Target = target;
        event.RawDamage = rawDamage;
        event.IsCritical = isCritical;
        event.CritMultiplier = sourceAC.Attributes.GetCurrentValue("CritMultiplier");
        if (isCritical && event.CritMultiplier <= 0.0f)
        {
            event.CritMultiplier = 2.0f;
        }

        if (damageTypeTag)
        {
            char* tagStr = mono_string_to_utf8(damageTypeTag);
            event.DamageType = GameplayTag(tagStr);
            mono_free(tagStr);
        }

        f32 finalDamage = DamageCalculation::Calculate(event, sourceAC.Attributes, targetAC.Attributes);

        // Apply the damage to the target's Health attribute
        f32 currentHealth = targetAC.Attributes.GetCurrentValue("Health");
        targetAC.Attributes.SetBaseValue("Health", std::max(currentHealth - finalDamage, 0.0f));

        return finalDamage;
    }

    static bool AbilityComponent_TryActivateAbilityOnTarget(UUID casterEntityID, MonoString* abilityTag, UUID targetEntityID)
    {
        if (!abilityTag)
        {
            return false;
        }

        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);

        Entity caster = scene->GetEntityByUUID(casterEntityID);
        Entity target = scene->GetEntityByUUID(targetEntityID);
        if (!caster || !target)
        {
            return false;
        }

        if (!caster.HasComponent<AbilityComponent>() || !target.HasComponent<AbilityComponent>())
        {
            return false;
        }

        char* tagStr = mono_string_to_utf8(abilityTag);
        GameplayTag tag(tagStr);
        mono_free(tagStr);

        // Activate on the caster (checks cooldowns, costs, tags).
        // Note: TryActivateAbility also applies ActivationEffects to the caster.
        // For targeted abilities, we redirect effects to the target below.
        if (!GameplayAbilitySystem::TryActivateAbility(scene, caster, tag))
        {
            return false;
        }

        // Apply TargetActivationEffects to the TARGET entity.
        // ActivationEffects were already applied to the caster by TryActivateAbility;
        // only the explicit target-specific effects list goes to the target.
        auto& casterAC = caster.GetComponent<AbilityComponent>();
        for (auto& ability : casterAC.Abilities)
        {
            if (ability.Definition.AbilityTag == tag)
            {
                if (!ability.Definition.TargetActivationEffects.empty())
                {
                    auto& targetAC = target.GetComponent<AbilityComponent>();
                    for (auto const& effect : ability.Definition.TargetActivationEffects)
                    {
                        targetAC.ActiveEffects.ApplyEffect(effect, targetAC.OwnedTags, tag);
                    }
                }
                break;
            }
        }

        return true;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Application / Time /////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static f32 Application_GetTimeScale()
    {
        return Application::Get().GetTimeScale();
    }

    static void Application_SetTimeScale(f32 scale)
    {
        Application::Get().SetTimeScale(scale);
    }

    static void Application_QuitGame()
    {
        auto& app = Application::Get();
        if (app.GetSpecification().IsEditor)
        {
            // In the editor, QuitGame() is a no-op — use the Stop button instead.
            OLO_CORE_WARN("[ScriptGlue] Application_QuitGame ignored in editor. "
                          "Use the editor Stop button or call SceneManager.ReloadCurrentScene() to restart.");
            return;
        }

        OLO_CORE_INFO("[ScriptGlue] Application_QuitGame — shutting down (standalone)");
        app.Close();
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Scene //////////////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static void Scene_ReloadCurrentScene()
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        if (scene)
        {
            scene->SetPendingReload(true);
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Localization ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static MonoString* Localization_Get(MonoString* key)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
            return ScriptEngine::CreateString("");
        const std::string keyStr = Utils::MonoStringToString(key);
        return ScriptEngine::CreateString(LocalizationManager::Get(keyStr).c_str());
    }

    // Format() / FormatPlural() take parameter dictionaries which Mono can't
    // marshal cheaply across the boundary. The managed-side wrapper flattens
    // its Dictionary<string,string> into two MonoArrays (keys + values) of
    // equal length; we zip them back into a ParamMap here.
    static MonoString* Localization_Format(MonoString* key, MonoArray* keys, MonoArray* values)
    {
        OLO_PROFILE_FUNCTION();
        if (!key)
            return ScriptEngine::CreateString("");
        const std::string keyStr = Utils::MonoStringToString(key);

        TextFormatter::ParamMap params;
        if (keys && values)
        {
            const uintptr_t keyCount = mono_array_length(keys);
            const uintptr_t valCount = mono_array_length(values);
            const uintptr_t paired = std::min(keyCount, valCount);
            params.reserve(paired);
            for (uintptr_t i = 0; i < paired; ++i)
            {
                MonoString* k = mono_array_get(keys, MonoString*, i);
                MonoString* v = mono_array_get(values, MonoString*, i);
                if (!k)
                    continue;
                std::string ks = Utils::MonoStringToString(k);
                std::string vs = v ? Utils::MonoStringToString(v) : std::string{};
                params.emplace(std::move(ks), std::move(vs));
            }
        }

        return ScriptEngine::CreateString(LocalizationManager::Format(keyStr, params).c_str());
    }

    static MonoString* Localization_FormatPlural(MonoString* key, MonoString* countParam, i32 count, MonoArray* keys, MonoArray* values)
    {
        OLO_PROFILE_FUNCTION();
        if (!key || !countParam)
            return ScriptEngine::CreateString("");
        const std::string keyStr = Utils::MonoStringToString(key);
        const std::string countParamStr = Utils::MonoStringToString(countParam);

        TextFormatter::ParamMap params;
        if (keys && values)
        {
            const uintptr_t keyCount = mono_array_length(keys);
            const uintptr_t valCount = mono_array_length(values);
            const uintptr_t paired = std::min(keyCount, valCount);
            params.reserve(paired);
            for (uintptr_t i = 0; i < paired; ++i)
            {
                MonoString* k = mono_array_get(keys, MonoString*, i);
                MonoString* v = mono_array_get(values, MonoString*, i);
                if (!k)
                    continue;
                std::string ks = Utils::MonoStringToString(k);
                std::string vs = v ? Utils::MonoStringToString(v) : std::string{};
                params.emplace(std::move(ks), std::move(vs));
            }
        }

        return ScriptEngine::CreateString(LocalizationManager::FormatPlural(keyStr, countParamStr, count, std::move(params)).c_str());
    }

    static bool Localization_SetLocale(MonoString* localeCode)
    {
        OLO_PROFILE_FUNCTION();
        if (!localeCode)
            return false;
        return LocalizationManager::SetCurrentLocale(Utils::MonoStringToString(localeCode));
    }

    static MonoString* Localization_GetCurrentLocale()
    {
        return ScriptEngine::CreateString(LocalizationManager::GetCurrentLocale().c_str());
    }

    static bool Localization_HasKey(MonoString* key)
    {
        if (!key)
            return false;
        return LocalizationManager::HasKey(Utils::MonoStringToString(key));
    }

    static MonoString* Localization_ResolveLocalizedText(MonoString* value)
    {
        if (!value)
            return ScriptEngine::CreateString("");
        const std::string s = Utils::MonoStringToString(value);
        return ScriptEngine::CreateString(LocalizationManager::ResolveLocalizedText(s).c_str());
    }

    static MonoString* Localization_FormatInt(i64 value, MonoString* localeCode)
    {
        const std::string loc = localeCode ? Utils::MonoStringToString(localeCode) : std::string{};
        return ScriptEngine::CreateString(LocalizationManager::FormatNumber(value, loc).c_str());
    }

    static MonoString* Localization_FormatFloat(f64 value, i32 decimals, MonoString* localeCode)
    {
        const std::string loc = localeCode ? Utils::MonoStringToString(localeCode) : std::string{};
        return ScriptEngine::CreateString(LocalizationManager::FormatNumber(value, decimals, loc).c_str());
    }

    static void Localization_ClearMissingKeys()
    {
        LocalizationManager::ClearMissingKeys();
    }

    static bool Localization_GeneratePseudoLocale(MonoString* source, MonoString* pseudoCode)
    {
        const std::string srcStr = source ? Utils::MonoStringToString(source) : std::string{ "en" };
        const std::string pseudoStr = pseudoCode ? Utils::MonoStringToString(pseudoCode) : std::string{ "pseudo" };
        return LocalizationManager::GeneratePseudoLocale(srcStr, pseudoStr);
    }

    static MonoString* Localization_FormatCurrency(f64 amount, MonoString* localeCode, MonoString* symbolOverride)
    {
        const std::string loc = localeCode ? Utils::MonoStringToString(localeCode) : std::string{};
        const std::string sym = symbolOverride ? Utils::MonoStringToString(symbolOverride) : std::string{};
        return ScriptEngine::CreateString(LocalizationManager::FormatCurrency(amount, loc, sym).c_str());
    }

    static MonoString* Localization_FormatList(MonoArray* items, MonoString* localeCode)
    {
        std::vector<std::string> v;
        if (items)
        {
            const uintptr_t n = mono_array_length(items);
            v.reserve(n);
            for (uintptr_t i = 0; i < n; ++i)
            {
                MonoString* s = mono_array_get(items, MonoString*, i);
                if (s)
                    v.push_back(Utils::MonoStringToString(s));
            }
        }
        const std::string loc = localeCode ? Utils::MonoStringToString(localeCode) : std::string{};
        return ScriptEngine::CreateString(LocalizationManager::FormatList(v, loc).c_str());
    }

    // Date / time: take Unix epoch seconds (i64) since System.DateTime
    // marshalling through Mono is awkward. The managed wrapper converts
    // via DateTimeOffset.ToUnixTimeSeconds before calling.
    static MonoString* Localization_FormatDate(i64 epochSeconds, i32 style, MonoString* localeCode)
    {
        const auto tp = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(epochSeconds));
        const std::string loc = localeCode ? Utils::MonoStringToString(localeCode) : std::string{};
        return ScriptEngine::CreateString(
            LocalizationManager::FormatDate(tp, static_cast<LocalizationManager::DateStyle>(style), loc).c_str());
    }

    static MonoString* Localization_FormatTime(i64 epochSeconds, i32 style, MonoString* localeCode)
    {
        const auto tp = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(epochSeconds));
        const std::string loc = localeCode ? Utils::MonoStringToString(localeCode) : std::string{};
        return ScriptEngine::CreateString(
            LocalizationManager::FormatTime(tp, static_cast<LocalizationManager::TimeStyle>(style), loc).c_str());
    }

    static MonoString* Localization_FormatRelativeTime(i64 epochSeconds, MonoString* localeCode)
    {
        const auto tp = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(epochSeconds));
        const std::string loc = localeCode ? Utils::MonoStringToString(localeCode) : std::string{};
        return ScriptEngine::CreateString(LocalizationManager::FormatRelativeTime(tp, loc).c_str());
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
        OLO_ADD_INTERNAL_CALL(Log_LogMessage);

        OLO_ADD_INTERNAL_CALL(GetScriptInstance);

        OLO_ADD_INTERNAL_CALL(Entity_HasComponent);
        OLO_ADD_INTERNAL_CALL(Entity_IsValid);
        OLO_ADD_INTERNAL_CALL(Entity_FindEntityByName);

        // Auto-generated property binding registrations from OLO_PROPERTY() annotations
#include "Generated/ScriptGlueRegistrations.inl"

        OLO_ADD_INTERNAL_CALL(Rigidbody2DComponent_ApplyLinearImpulse);
        OLO_ADD_INTERNAL_CALL(Rigidbody2DComponent_ApplyLinearImpulseToCenter);

        OLO_ADD_INTERNAL_CALL(Input_IsKeyDown);
        OLO_ADD_INTERNAL_CALL(Input_IsKeyJustPressed);
        OLO_ADD_INTERNAL_CALL(Input_IsKeyJustReleased);

        ///////////////////////////////////////////////////////////////
        // Gamepad ////////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Input_IsGamepadButtonPressed);
        OLO_ADD_INTERNAL_CALL(Input_IsGamepadButtonJustPressed);
        OLO_ADD_INTERNAL_CALL(Input_IsGamepadButtonJustReleased);
        OLO_ADD_INTERNAL_CALL(Input_GetGamepadAxis);
        OLO_ADD_INTERNAL_CALL(Input_GetGamepadLeftStick);
        OLO_ADD_INTERNAL_CALL(Input_GetGamepadRightStick);
        OLO_ADD_INTERNAL_CALL(Input_IsGamepadConnected);
        OLO_ADD_INTERNAL_CALL(Input_GetGamepadConnectedCount);

        ///////////////////////////////////////////////////////////////
        // Input Action Mapping ///////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Input_IsActionPressed);
        OLO_ADD_INTERNAL_CALL(Input_IsActionJustPressed);
        OLO_ADD_INTERNAL_CALL(Input_IsActionJustReleased);
        OLO_ADD_INTERNAL_CALL(Input_GetActionAxisValue);

        ///////////////////////////////////////////////////////////////
        // Audio Source ///////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetCone);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_IsPlaying);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_Play);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_Pause);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_UnPause);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_Stop);

        OLO_ADD_INTERNAL_CALL(AudioSoundGraphComponent_IsPlaying);
        OLO_ADD_INTERNAL_CALL(AudioSoundGraphComponent_Play);
        OLO_ADD_INTERNAL_CALL(AudioSoundGraphComponent_Stop);
        OLO_ADD_INTERNAL_CALL(AudioSoundGraphComponent_Pause);
        OLO_ADD_INTERNAL_CALL(AudioSoundGraphComponent_SetParameterFloat);
        OLO_ADD_INTERNAL_CALL(AudioSoundGraphComponent_SetParameterInt);
        OLO_ADD_INTERNAL_CALL(AudioSoundGraphComponent_SetParameterBool);

        ///////////////////////////////////////////////////////////////
        // Audio Events ///////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(AudioEvents_PostTrigger);
        OLO_ADD_INTERNAL_CALL(AudioEvents_StopEvent);
        OLO_ADD_INTERNAL_CALL(AudioEvents_PauseEvent);
        OLO_ADD_INTERNAL_CALL(AudioEvents_ResumeEvent);
        OLO_ADD_INTERNAL_CALL(AudioEvents_StopAll);
        OLO_ADD_INTERNAL_CALL(AudioEvents_IsEventActive);

        ///////////////////////////////////////////////////////////////
        // UI/Particle/LightProbe (hand-written action/query only) ////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_Dirty);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_GetTotalProbeCount);

        OLO_ADD_INTERNAL_CALL(Scene_GetWindEnabled);
        OLO_ADD_INTERNAL_CALL(Scene_SetWindEnabled);
        OLO_ADD_INTERNAL_CALL(Scene_GetWindDirection);
        OLO_ADD_INTERNAL_CALL(Scene_SetWindDirection);
        OLO_ADD_INTERNAL_CALL(Scene_GetWindSpeed);
        OLO_ADD_INTERNAL_CALL(Scene_SetWindSpeed);
        OLO_ADD_INTERNAL_CALL(Scene_GetWindGustStrength);
        OLO_ADD_INTERNAL_CALL(Scene_SetWindGustStrength);
        OLO_ADD_INTERNAL_CALL(Scene_GetWindTurbulenceIntensity);
        OLO_ADD_INTERNAL_CALL(Scene_SetWindTurbulenceIntensity);

        ///////////////////////////////////////////////////////////////
        // Streaming //////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(StreamingVolumeComponent_GetIsLoaded);
        OLO_ADD_INTERNAL_CALL(Scene_LoadRegion);
        OLO_ADD_INTERNAL_CALL(Scene_UnloadRegion);
        OLO_ADD_INTERNAL_CALL(Scene_GetStreamingEnabled);
        OLO_ADD_INTERNAL_CALL(Scene_SetStreamingEnabled);

        ///////////////////////////////////////////////////////////////
        // Networking ////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Network_IsServer);
        OLO_ADD_INTERNAL_CALL(Network_IsClient);
        OLO_ADD_INTERNAL_CALL(Network_IsConnected);
        OLO_ADD_INTERNAL_CALL(Network_Connect);
        OLO_ADD_INTERNAL_CALL(Network_Disconnect);
        OLO_ADD_INTERNAL_CALL(Network_StartServer);
        OLO_ADD_INTERNAL_CALL(Network_StopServer);

        ///////////////////////////////////////////////////////////////
        // Dialogue //////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(DialogueComponent_StartDialogue);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_AdvanceDialogue);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_SelectChoice);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_IsDialogueActive);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_EndDialogue);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_GetDialogueTree);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_SetDialogueTree);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_GetAutoTrigger);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_SetAutoTrigger);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_GetTriggerRadius);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_SetTriggerRadius);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_GetTriggerOnce);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_SetTriggerOnce);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_GetHasTriggered);
        OLO_ADD_INTERNAL_CALL(DialogueComponent_SetHasTriggered);

        OLO_ADD_INTERNAL_CALL(DialogueVariables_GetBool);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_SetBool);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_GetInt);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_SetInt);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_GetFloat);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_SetFloat);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_GetString);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_SetString);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_Has);
        OLO_ADD_INTERNAL_CALL(DialogueVariables_Clear);

        ///////////////////////////////////////////////////////////////
        // InstancedMeshComponent ////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(InstancedMeshComponent_GetInstanceCount);
        OLO_ADD_INTERNAL_CALL(InstancedMeshComponent_AddInstance);
        OLO_ADD_INTERNAL_CALL(InstancedMeshComponent_ClearInstances);
        OLO_ADD_INTERNAL_CALL(InstancedMeshComponent_GetCastShadows);
        OLO_ADD_INTERNAL_CALL(InstancedMeshComponent_SetCastShadows);

        ///////////////////////////////////////////////////////////////
        // MaterialComponent /////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(MaterialComponent_GetShaderGraphHandle);
        OLO_ADD_INTERNAL_CALL(MaterialComponent_SetShaderGraphHandle);

        ///////////////////////////////////////////////////////////////
        // NavAgentComponent (hand-written action/query only) ////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_HasPath);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_ClearTarget);

        ///////////////////////////////////////////////////////////////
        // UIWorldAnchorComponent (hand-written UUID methods) ////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(UIWorldAnchorComponent_GetTargetEntity);
        OLO_ADD_INTERNAL_CALL(UIWorldAnchorComponent_SetTargetEntity);

        ///////////////////////////////////////////////////////////////
        // IKTargetComponent /////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimIKEnabled);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimIKEnabled);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimBoneIndex);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimBoneIndex);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimTarget);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimTarget);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimAxis);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimAxis);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimOffset);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimOffset);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimPoleVector);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimPoleVector);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimChainLength);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimChainLength);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimChainFactor);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimChainFactor);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimTargetEntity);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimTargetEntity);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetAimWeight);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetAimWeight);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetLimbIKEnabled);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetLimbIKEnabled);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetLimbBoneIndex);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetLimbBoneIndex);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetLimbTarget);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetLimbTarget);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetLimbChainLength);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetLimbChainLength);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetLimbTargetEntity);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetLimbTargetEntity);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_GetLimbWeight);
        OLO_ADD_INTERNAL_CALL(IKTargetComponent_SetLimbWeight);

        ///////////////////////////////////////////////////////////////
        // AnimationGraphComponent ////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(AnimationGraphComponent_SetFloat);
        OLO_ADD_INTERNAL_CALL(AnimationGraphComponent_SetBool);
        OLO_ADD_INTERNAL_CALL(AnimationGraphComponent_SetInt);
        OLO_ADD_INTERNAL_CALL(AnimationGraphComponent_SetTrigger);
        OLO_ADD_INTERNAL_CALL(AnimationGraphComponent_GetFloat);
        OLO_ADD_INTERNAL_CALL(AnimationGraphComponent_GetBool);
        OLO_ADD_INTERNAL_CALL(AnimationGraphComponent_GetInt);
        OLO_ADD_INTERNAL_CALL(AnimationGraphComponent_GetCurrentState);

        ///////////////////////////////////////////////////////////////
        // MorphTargetComponent //////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(MorphTargetComponent_SetWeight);
        OLO_ADD_INTERNAL_CALL(MorphTargetComponent_GetWeight);
        OLO_ADD_INTERNAL_CALL(MorphTargetComponent_ResetAll);
        OLO_ADD_INTERNAL_CALL(MorphTargetComponent_GetTargetCount);
        OLO_ADD_INTERNAL_CALL(MorphTargetComponent_ApplyExpression);

        ///////////////////////////////////////////////////////////////
        // SaveGame //////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(SaveGame_Save);
        OLO_ADD_INTERNAL_CALL(SaveGame_Load);
        OLO_ADD_INTERNAL_CALL(SaveGame_QuickSave);
        OLO_ADD_INTERNAL_CALL(SaveGame_QuickLoad);
        OLO_ADD_INTERNAL_CALL(SaveGame_DeleteSave);
        OLO_ADD_INTERNAL_CALL(SaveGame_ValidateSave);

        // BehaviorTreeComponent
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_SetBlackboardBool);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_GetBlackboardBool);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_SetBlackboardInt);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_GetBlackboardInt);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_SetBlackboardFloat);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_GetBlackboardFloat);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_SetBlackboardString);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_GetBlackboardString);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_SetBlackboardVec3);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_GetBlackboardVec3);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_RemoveBlackboardKey);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_HasBlackboardKey);
        OLO_ADD_INTERNAL_CALL(BehaviorTreeComponent_IsRunning);

        // StateMachineComponent
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_SetBlackboardBool);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_GetBlackboardBool);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_SetBlackboardInt);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_GetBlackboardInt);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_SetBlackboardFloat);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_GetBlackboardFloat);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_SetBlackboardString);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_GetBlackboardString);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_SetBlackboardVec3);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_GetBlackboardVec3);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_RemoveBlackboardKey);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_HasBlackboardKey);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_GetCurrentState);
        OLO_ADD_INTERNAL_CALL(StateMachineComponent_ForceTransition);

        ///////////////////////////////////////////////////////////////
        // Inventory //////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(InventoryComponent_AddItem);
        OLO_ADD_INTERNAL_CALL(InventoryComponent_RemoveItem);
        OLO_ADD_INTERNAL_CALL(InventoryComponent_HasItem);
        OLO_ADD_INTERNAL_CALL(InventoryComponent_CountItem);
        OLO_ADD_INTERNAL_CALL(InventoryComponent_GetCurrency);
        OLO_ADD_INTERNAL_CALL(InventoryComponent_SetCurrency);

        ///////////////////////////////////////////////////////////////
        // Quest //////////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_AcceptQuest);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_AbandonQuest);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_CompleteQuest);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_IsQuestActive);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_HasCompletedQuest);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_IncrementObjective);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_NotifyKill);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_NotifyCollect);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_NotifyInteract);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_NotifyReachLocation);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_SetPlayerLevel);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_GetPlayerLevel);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_SetReputation);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_GetReputation);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_SetItemCount);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_GetItemCount);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_SetStat);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_GetStat);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_SetPlayerClass);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_GetPlayerClass);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_SetPlayerFaction);
        OLO_ADD_INTERNAL_CALL(QuestJournalComponent_GetPlayerFaction);

        ///////////////////////////////////////////////////////////////
        // AbilityComponent ///////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(AbilityComponent_GetAttribute);
        OLO_ADD_INTERNAL_CALL(AbilityComponent_SetAttribute);
        OLO_ADD_INTERNAL_CALL(AbilityComponent_GetCurrentAttribute);
        OLO_ADD_INTERNAL_CALL(AbilityComponent_TryActivateAbility);
        OLO_ADD_INTERNAL_CALL(AbilityComponent_HasTag);
        OLO_ADD_INTERNAL_CALL(AbilityComponent_AddTag);
        OLO_ADD_INTERNAL_CALL(AbilityComponent_RemoveTag);
        OLO_ADD_INTERNAL_CALL(AbilityComponent_ApplyDamageToTarget);
        OLO_ADD_INTERNAL_CALL(AbilityComponent_TryActivateAbilityOnTarget);

        ///////////////////////////////////////////////////////////////
        // Physics ////////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Physics_Raycast);

        ///////////////////////////////////////////////////////////////
        // Camera /////////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Camera_ScreenToWorldRay);

        ///////////////////////////////////////////////////////////////
        // Mouse Input ////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Input_GetMousePosition);
        OLO_ADD_INTERNAL_CALL(Input_GetWindowSize);
        OLO_ADD_INTERNAL_CALL(Input_IsMouseButtonDown);

        ///////////////////////////////////////////////////////////////
        // ShaderLibrary3D ///////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(ShaderLibrary3D_LoadShader);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary3D_Exists);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary3D_GetShaderName);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary3D_ReloadAll);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary3D_ReloadShader);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary3D_GetShaderCount);

        ///////////////////////////////////////////////////////////////
        // ShaderLibrary2D ///////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(ShaderLibrary2D_LoadShader);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary2D_Exists);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary2D_GetShaderName);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary2D_ReloadAll);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary2D_ReloadShader);
        OLO_ADD_INTERNAL_CALL(ShaderLibrary2D_GetShaderCount);

        ///////////////////////////////////////////////////////////////
        // Application / Time /////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Application_GetTimeScale);
        OLO_ADD_INTERNAL_CALL(Application_SetTimeScale);
        OLO_ADD_INTERNAL_CALL(Application_QuitGame);

        ///////////////////////////////////////////////////////////////
        // Scene //////////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Scene_ReloadCurrentScene);

        ///////////////////////////////////////////////////////////////
        // Localization ///////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(Localization_Get);
        OLO_ADD_INTERNAL_CALL(Localization_Format);
        OLO_ADD_INTERNAL_CALL(Localization_FormatPlural);
        OLO_ADD_INTERNAL_CALL(Localization_SetLocale);
        OLO_ADD_INTERNAL_CALL(Localization_GetCurrentLocale);
        OLO_ADD_INTERNAL_CALL(Localization_HasKey);
        OLO_ADD_INTERNAL_CALL(Localization_ResolveLocalizedText);
        OLO_ADD_INTERNAL_CALL(Localization_FormatInt);
        OLO_ADD_INTERNAL_CALL(Localization_FormatFloat);
        OLO_ADD_INTERNAL_CALL(Localization_ClearMissingKeys);
        OLO_ADD_INTERNAL_CALL(Localization_GeneratePseudoLocale);
        OLO_ADD_INTERNAL_CALL(Localization_FormatCurrency);
        OLO_ADD_INTERNAL_CALL(Localization_FormatList);
        OLO_ADD_INTERNAL_CALL(Localization_FormatDate);
        OLO_ADD_INTERNAL_CALL(Localization_FormatTime);
        OLO_ADD_INTERNAL_CALL(Localization_FormatRelativeTime);
    }

} // namespace OloEngine

#else // !OLO_ENABLE_CSHARP_SCRIPTING

namespace OloEngine
{
    void ScriptGlue::RegisterComponents() {}
    void ScriptGlue::RegisterFunctions() {}
} // namespace OloEngine

#endif // OLO_ENABLE_CSHARP_SCRIPTING
