#include "OloEnginePCH.h"
#include "ScriptGlue.h"
#include "ScriptEngine.h"

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
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueVariables.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/MorphTargets/FacialExpressionLibrary.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"

#include "mono/metadata/object.h"
#include "mono/metadata/reflection.h"

#include "box2d/box2d.h"

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
    // TextComponent //////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static MonoString* TextComponent_GetText(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        OLO_CORE_ASSERT(entity.HasComponent<TextComponent>());

        auto const& tc = entity.GetComponent<TextComponent>();
        return ScriptEngine::CreateString(tc.TextString.c_str());
    }

    static void TextComponent_SetText(UUID entityID, MonoString* textString)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        OLO_CORE_ASSERT(entity.HasComponent<TextComponent>());

        auto& tc = entity.GetComponent<TextComponent>();
        tc.TextString = Utils::MonoStringToString(textString);
    }

    static void TextComponent_GetColor(UUID entityID, glm::vec4* color)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        OLO_CORE_ASSERT(entity.HasComponent<TextComponent>());

        auto const& tc = entity.GetComponent<TextComponent>();
        *color = tc.Color;
    }

    static void TextComponent_SetColor(UUID entityID, glm::vec4 const* color)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        OLO_CORE_ASSERT(entity.HasComponent<TextComponent>());

        auto& tc = entity.GetComponent<TextComponent>();
        tc.Color = *color;
    }

    static f32 TextComponent_GetKerning(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        OLO_CORE_ASSERT(entity.HasComponent<TextComponent>());

        auto const& tc = entity.GetComponent<TextComponent>();
        return tc.Kerning;
    }

    static void TextComponent_SetKerning(UUID entityID, f32 kerning)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        OLO_CORE_ASSERT(entity.HasComponent<TextComponent>());

        auto& tc = entity.GetComponent<TextComponent>();
        tc.Kerning = kerning;
    }

    static f32 TextComponent_GetLineSpacing(UUID entityID)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        OLO_CORE_ASSERT(entity.HasComponent<TextComponent>());

        auto const& tc = entity.GetComponent<TextComponent>();
        return tc.LineSpacing;
    }

    static void TextComponent_SetLineSpacing(UUID entityID, f32 lineSpacing)
    {
        Scene* scene = ScriptEngine::GetSceneContext();
        OLO_CORE_ASSERT(scene);
        Entity entity = scene->GetEntityByUUID(entityID);
        OLO_CORE_ASSERT(entity);
        OLO_CORE_ASSERT(entity.HasComponent<TextComponent>());

        auto& tc = entity.GetComponent<TextComponent>();
        tc.LineSpacing = lineSpacing;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Audio Source ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    void AudioSourceComponent_GetVolume(u64 entityID, f32* outVolume)
    {
        *outVolume = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.VolumeMultiplier;
    }

    void AudioSourceComponent_SetVolume(u64 entityID, const f32* volume)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.VolumeMultiplier = *volume;
        if (component.Source)
        {
            component.Source->SetVolume(*volume);
        }
    }

    void AudioSourceComponent_GetPitch(u64 entityID, f32* outPitch)
    {
        *outPitch = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.PitchMultiplier;
    }

    void AudioSourceComponent_SetPitch(u64 entityID, const f32* pitch)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.PitchMultiplier = *pitch;
        if (component.Source)
        {
            component.Source->SetVolume(*pitch);
        }
    }

    void AudioSourceComponent_GetPlayOnAwake(u64 entityID, bool* outPlayOnAwake)
    {
        *outPlayOnAwake = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.PlayOnAwake;
    }

    void AudioSourceComponent_SetPlayOnAwake(u64 entityID, const bool* playOnAwake)
    {
        GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.PlayOnAwake = *playOnAwake;
    }

    void AudioSourceComponent_GetLooping(u64 entityID, bool* outLooping)
    {
        *outLooping = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.Looping;
    }

    void AudioSourceComponent_SetLooping(u64 entityID, const bool* looping)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.Looping = *looping;
        if (component.Source)
        {
            component.Source->SetLooping(*looping);
        }
    }

    void AudioSourceComponent_GetSpatialization(u64 entityID, bool* outSpatialization)
    {
        *outSpatialization = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.Spatialization;
    }

    void AudioSourceComponent_SetSpatialization(u64 entityID, const bool* spatialization)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.Spatialization = *spatialization;
        if (component.Source)
        {
            component.Source->SetSpatialization(*spatialization);
        }
    }

    void AudioSourceComponent_GetAttenuationModel(u64 entityID, int* outAttenuationModel)
    {
        *outAttenuationModel = (int)GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.AttenuationModel;
    }

    void AudioSourceComponent_SetAttenuationModel(u64 entityID, const int* attenuationModel)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.AttenuationModel = (AttenuationModelType)(*attenuationModel);
        if (component.Source)
        {
            component.Source->SetAttenuationModel(component.Config.AttenuationModel);
        }
    }

    void AudioSourceComponent_GetRollOff(u64 entityID, f32* outRollOff)
    {
        *outRollOff = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.RollOff;
    }

    void AudioSourceComponent_SetRollOff(u64 entityID, const f32* rollOff)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.RollOff = *rollOff;
        if (component.Source)
        {
            component.Source->SetRollOff(*rollOff);
        }
    }

    void AudioSourceComponent_GetMinGain(u64 entityID, f32* outMinGain)
    {
        *outMinGain = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.MinGain;
    }

    void AudioSourceComponent_SetMinGain(u64 entityID, const f32* minGain)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.MinGain = *minGain;
        if (component.Source)
        {
            component.Source->SetMinGain(*minGain);
        }
    }

    void AudioSourceComponent_GetMaxGain(u64 entityID, f32* outMaxGain)
    {
        *outMaxGain = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.MaxGain;
    }

    void AudioSourceComponent_SetMaxGain(u64 entityID, const f32* maxGain)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.MaxGain = *maxGain;
        if (component.Source)
        {
            component.Source->SetMaxGain(*maxGain);
        }
    }

    void AudioSourceComponent_GetMinDistance(u64 entityID, f32* outMinDistance)
    {
        *outMinDistance = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.MinDistance;
    }

    void AudioSourceComponent_SetMinDistance(u64 entityID, const f32* minDistance)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.MinDistance = *minDistance;
        if (component.Source)
        {
            component.Source->SetMinDistance(*minDistance);
        }
    }

    void AudioSourceComponent_GetMaxDistance(u64 entityID, f32* outMaxDistance)
    {
        *outMaxDistance = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.MaxDistance;
    }

    void AudioSourceComponent_SetMaxDistance(u64 entityID, const f32* maxDistance)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.MaxDistance = *maxDistance;
        if (component.Source)
        {
            component.Source->SetMaxDistance(*maxDistance);
        }
    }

    void AudioSourceComponent_GetConeInnerAngle(u64 entityID, f32* outConeInnerAngle)
    {
        *outConeInnerAngle = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.ConeInnerAngle;
    }

    void AudioSourceComponent_SetConeInnerAngle(u64 entityID, const f32* coneInnerAngle)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.ConeInnerAngle = *coneInnerAngle;
        if (component.Source)
        {
            component.Source->SetCone(component.Config.ConeInnerAngle, component.Config.ConeOuterAngle, component.Config.ConeOuterGain);
        }
    }

    void AudioSourceComponent_GetConeOuterAngle(u64 entityID, f32* outConeOuterAngle)
    {
        *outConeOuterAngle = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.ConeOuterAngle;
    }

    void AudioSourceComponent_SetConeOuterAngle(u64 entityID, const f32* coneOuterAngle)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.ConeOuterAngle = *coneOuterAngle;
        if (component.Source)
        {
            component.Source->SetCone(component.Config.ConeInnerAngle, component.Config.ConeOuterAngle, component.Config.ConeOuterGain);
        }
    }

    void AudioSourceComponent_GetConeOuterGain(u64 entityID, f32* outConeOuterGain)
    {
        *outConeOuterGain = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.ConeOuterGain;
    }

    void AudioSourceComponent_SetConeOuterGain(u64 entityID, const f32* coneOuterGain)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.ConeOuterGain = *coneOuterGain;
        if (component.Source)
        {
            component.Source->SetCone(component.Config.ConeInnerAngle, component.Config.ConeOuterAngle, component.Config.ConeOuterGain);
        }
    }

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

    void AudioSourceComponent_GetDopplerFactor(u64 entityID, f32* outDopplerFactor)
    {
        {
            *outDopplerFactor = GetEntity(entityID).GetComponent<AudioSourceComponent>().Config.DopplerFactor;
        }
    }

    void AudioSourceComponent_SetDopplerFactor(u64 entityID, const f32* dopplerFactor)
    {
        auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        component.Config.DopplerFactor = *dopplerFactor;
        if (component.Source)
        {
            component.Source->SetDopplerFactor(*dopplerFactor);
        }
    }

    void AudioSourceComponent_IsPlaying(u64 entityID, bool* outIsPlaying)
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

    void AudioSourceComponent_Play(u64 entityID)
    {
        const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.Source)
        {
            component.Source->Play();
        }
    }

    void AudioSourceComponent_Pause(u64 entityID)
    {
        const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.Source)
        {
            component.Source->Pause();
        }
    }

    void AudioSourceComponent_UnPause(u64 entityID)
    {
        const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.Source)
        {
            component.Source->UnPause();
        }
    }

    void AudioSourceComponent_Stop(u64 entityID)
    {
        const auto& component = GetEntity(entityID).GetComponent<AudioSourceComponent>();
        if (component.Source)
        {
            component.Source->Stop();
        }
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // UI Components //////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    // --- UICanvasComponent ---

    static i32 UICanvasComponent_GetSortOrder(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UICanvasComponent>().m_SortOrder;
    }

    static void UICanvasComponent_SetSortOrder(UUID entityID, i32 sortOrder)
    {
        GetEntity(entityID).GetComponent<UICanvasComponent>().m_SortOrder = sortOrder;
    }

    // --- UIRectTransformComponent ---

    static void UIRectTransformComponent_GetAnchorMin(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_AnchorMin;
    }

    static void UIRectTransformComponent_SetAnchorMin(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_AnchorMin = *v;
    }

    static void UIRectTransformComponent_GetAnchorMax(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_AnchorMax;
    }

    static void UIRectTransformComponent_SetAnchorMax(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_AnchorMax = *v;
    }

    static void UIRectTransformComponent_GetAnchoredPosition(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_AnchoredPosition;
    }

    static void UIRectTransformComponent_SetAnchoredPosition(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_AnchoredPosition = *v;
    }

    static void UIRectTransformComponent_GetSizeDelta(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_SizeDelta;
    }

    static void UIRectTransformComponent_SetSizeDelta(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_SizeDelta = *v;
    }

    static void UIRectTransformComponent_GetPivot(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_Pivot;
    }

    static void UIRectTransformComponent_SetPivot(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_Pivot = *v;
    }

    static f32 UIRectTransformComponent_GetRotation(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_Rotation;
    }

    static void UIRectTransformComponent_SetRotation(UUID entityID, f32 rotation)
    {
        GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_Rotation = rotation;
    }

    static void UIRectTransformComponent_GetScale(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_Scale;
    }

    static void UIRectTransformComponent_SetScale(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIRectTransformComponent>().m_Scale = *v;
    }

    // --- UIImageComponent ---

    static void UIImageComponent_GetColor(UUID entityID, glm::vec4* out)
    {
        *out = GetEntity(entityID).GetComponent<UIImageComponent>().m_Color;
    }

    static void UIImageComponent_SetColor(UUID entityID, glm::vec4 const* v)
    {
        GetEntity(entityID).GetComponent<UIImageComponent>().m_Color = *v;
    }

    // --- UIPanelComponent ---

    static void UIPanelComponent_GetBackgroundColor(UUID entityID, glm::vec4* out)
    {
        *out = GetEntity(entityID).GetComponent<UIPanelComponent>().m_BackgroundColor;
    }

    static void UIPanelComponent_SetBackgroundColor(UUID entityID, glm::vec4 const* v)
    {
        GetEntity(entityID).GetComponent<UIPanelComponent>().m_BackgroundColor = *v;
    }

    // --- UITextComponent ---

    static MonoString* UITextComponent_GetText(UUID entityID)
    {
        auto const& tc = GetEntity(entityID).GetComponent<UITextComponent>();
        return ScriptEngine::CreateString(tc.m_Text.c_str());
    }

    static void UITextComponent_SetText(UUID entityID, MonoString* text)
    {
        GetEntity(entityID).GetComponent<UITextComponent>().m_Text = Utils::MonoStringToString(text);
    }

    static f32 UITextComponent_GetFontSize(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UITextComponent>().m_FontSize;
    }

    static void UITextComponent_SetFontSize(UUID entityID, f32 fontSize)
    {
        GetEntity(entityID).GetComponent<UITextComponent>().m_FontSize = fontSize;
    }

    static void UITextComponent_GetColor(UUID entityID, glm::vec4* out)
    {
        *out = GetEntity(entityID).GetComponent<UITextComponent>().m_Color;
    }

    static void UITextComponent_SetColor(UUID entityID, glm::vec4 const* v)
    {
        GetEntity(entityID).GetComponent<UITextComponent>().m_Color = *v;
    }

    static f32 UITextComponent_GetKerning(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UITextComponent>().m_Kerning;
    }

    static void UITextComponent_SetKerning(UUID entityID, f32 kerning)
    {
        GetEntity(entityID).GetComponent<UITextComponent>().m_Kerning = kerning;
    }

    static f32 UITextComponent_GetLineSpacing(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UITextComponent>().m_LineSpacing;
    }

    static void UITextComponent_SetLineSpacing(UUID entityID, f32 lineSpacing)
    {
        GetEntity(entityID).GetComponent<UITextComponent>().m_LineSpacing = lineSpacing;
    }

    // --- UIButtonComponent ---

    static void UIButtonComponent_GetNormalColor(UUID entityID, glm::vec4* out)
    {
        *out = GetEntity(entityID).GetComponent<UIButtonComponent>().m_NormalColor;
    }

    static void UIButtonComponent_SetNormalColor(UUID entityID, glm::vec4 const* v)
    {
        GetEntity(entityID).GetComponent<UIButtonComponent>().m_NormalColor = *v;
    }

    static void UIButtonComponent_GetHoveredColor(UUID entityID, glm::vec4* out)
    {
        *out = GetEntity(entityID).GetComponent<UIButtonComponent>().m_HoveredColor;
    }

    static void UIButtonComponent_SetHoveredColor(UUID entityID, glm::vec4 const* v)
    {
        GetEntity(entityID).GetComponent<UIButtonComponent>().m_HoveredColor = *v;
    }

    static void UIButtonComponent_GetPressedColor(UUID entityID, glm::vec4* out)
    {
        *out = GetEntity(entityID).GetComponent<UIButtonComponent>().m_PressedColor;
    }

    static void UIButtonComponent_SetPressedColor(UUID entityID, glm::vec4 const* v)
    {
        GetEntity(entityID).GetComponent<UIButtonComponent>().m_PressedColor = *v;
    }

    static void UIButtonComponent_GetDisabledColor(UUID entityID, glm::vec4* out)
    {
        *out = GetEntity(entityID).GetComponent<UIButtonComponent>().m_DisabledColor;
    }

    static void UIButtonComponent_SetDisabledColor(UUID entityID, glm::vec4 const* v)
    {
        GetEntity(entityID).GetComponent<UIButtonComponent>().m_DisabledColor = *v;
    }

    static void UIButtonComponent_GetInteractable(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<UIButtonComponent>().m_Interactable;
    }

    static void UIButtonComponent_SetInteractable(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<UIButtonComponent>().m_Interactable = *v;
    }

    static i32 UIButtonComponent_GetState(UUID entityID)
    {
        return static_cast<i32>(GetEntity(entityID).GetComponent<UIButtonComponent>().m_State);
    }

    // --- UISliderComponent ---

    static f32 UISliderComponent_GetValue(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UISliderComponent>().m_Value;
    }

    static void UISliderComponent_SetValue(UUID entityID, f32 value)
    {
        GetEntity(entityID).GetComponent<UISliderComponent>().m_Value = value;
    }

    static f32 UISliderComponent_GetMinValue(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UISliderComponent>().m_MinValue;
    }

    static void UISliderComponent_SetMinValue(UUID entityID, f32 minValue)
    {
        GetEntity(entityID).GetComponent<UISliderComponent>().m_MinValue = minValue;
    }

    static f32 UISliderComponent_GetMaxValue(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UISliderComponent>().m_MaxValue;
    }

    static void UISliderComponent_SetMaxValue(UUID entityID, f32 maxValue)
    {
        GetEntity(entityID).GetComponent<UISliderComponent>().m_MaxValue = maxValue;
    }

    static void UISliderComponent_GetInteractable(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<UISliderComponent>().m_Interactable;
    }

    static void UISliderComponent_SetInteractable(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<UISliderComponent>().m_Interactable = *v;
    }

    // --- UICheckboxComponent ---

    static void UICheckboxComponent_GetIsChecked(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<UICheckboxComponent>().m_IsChecked;
    }

    static void UICheckboxComponent_SetIsChecked(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<UICheckboxComponent>().m_IsChecked = *v;
    }

    static void UICheckboxComponent_GetInteractable(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<UICheckboxComponent>().m_Interactable;
    }

    static void UICheckboxComponent_SetInteractable(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<UICheckboxComponent>().m_Interactable = *v;
    }

    // --- UIProgressBarComponent ---

    static f32 UIProgressBarComponent_GetValue(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UIProgressBarComponent>().m_Value;
    }

    static void UIProgressBarComponent_SetValue(UUID entityID, f32 value)
    {
        GetEntity(entityID).GetComponent<UIProgressBarComponent>().m_Value = value;
    }

    static f32 UIProgressBarComponent_GetMinValue(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UIProgressBarComponent>().m_MinValue;
    }

    static void UIProgressBarComponent_SetMinValue(UUID entityID, f32 minValue)
    {
        GetEntity(entityID).GetComponent<UIProgressBarComponent>().m_MinValue = minValue;
    }

    static f32 UIProgressBarComponent_GetMaxValue(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UIProgressBarComponent>().m_MaxValue;
    }

    static void UIProgressBarComponent_SetMaxValue(UUID entityID, f32 maxValue)
    {
        GetEntity(entityID).GetComponent<UIProgressBarComponent>().m_MaxValue = maxValue;
    }

    // --- UIInputFieldComponent ---

    static MonoString* UIInputFieldComponent_GetText(UUID entityID)
    {
        auto const& tc = GetEntity(entityID).GetComponent<UIInputFieldComponent>();
        return ScriptEngine::CreateString(tc.m_Text.c_str());
    }

    static void UIInputFieldComponent_SetText(UUID entityID, MonoString* text)
    {
        GetEntity(entityID).GetComponent<UIInputFieldComponent>().m_Text = Utils::MonoStringToString(text);
    }

    static MonoString* UIInputFieldComponent_GetPlaceholder(UUID entityID)
    {
        auto const& tc = GetEntity(entityID).GetComponent<UIInputFieldComponent>();
        return ScriptEngine::CreateString(tc.m_Placeholder.c_str());
    }

    static void UIInputFieldComponent_SetPlaceholder(UUID entityID, MonoString* placeholder)
    {
        GetEntity(entityID).GetComponent<UIInputFieldComponent>().m_Placeholder = Utils::MonoStringToString(placeholder);
    }

    static f32 UIInputFieldComponent_GetFontSize(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UIInputFieldComponent>().m_FontSize;
    }

    static void UIInputFieldComponent_SetFontSize(UUID entityID, f32 fontSize)
    {
        GetEntity(entityID).GetComponent<UIInputFieldComponent>().m_FontSize = fontSize;
    }

    static void UIInputFieldComponent_GetTextColor(UUID entityID, glm::vec4* out)
    {
        *out = GetEntity(entityID).GetComponent<UIInputFieldComponent>().m_TextColor;
    }

    static void UIInputFieldComponent_SetTextColor(UUID entityID, glm::vec4 const* v)
    {
        GetEntity(entityID).GetComponent<UIInputFieldComponent>().m_TextColor = *v;
    }

    static void UIInputFieldComponent_GetInteractable(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<UIInputFieldComponent>().m_Interactable;
    }

    static void UIInputFieldComponent_SetInteractable(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<UIInputFieldComponent>().m_Interactable = *v;
    }

    // --- UIScrollViewComponent ---

    static void UIScrollViewComponent_GetScrollPosition(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIScrollViewComponent>().m_ScrollPosition;
    }

    static void UIScrollViewComponent_SetScrollPosition(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIScrollViewComponent>().m_ScrollPosition = *v;
    }

    static void UIScrollViewComponent_GetContentSize(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIScrollViewComponent>().m_ContentSize;
    }

    static void UIScrollViewComponent_SetContentSize(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIScrollViewComponent>().m_ContentSize = *v;
    }

    static f32 UIScrollViewComponent_GetScrollSpeed(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UIScrollViewComponent>().m_ScrollSpeed;
    }

    static void UIScrollViewComponent_SetScrollSpeed(UUID entityID, f32 scrollSpeed)
    {
        GetEntity(entityID).GetComponent<UIScrollViewComponent>().m_ScrollSpeed = scrollSpeed;
    }

    // --- UIDropdownComponent ---

    static i32 UIDropdownComponent_GetSelectedIndex(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UIDropdownComponent>().m_SelectedIndex;
    }

    static void UIDropdownComponent_SetSelectedIndex(UUID entityID, i32 selectedIndex)
    {
        GetEntity(entityID).GetComponent<UIDropdownComponent>().m_SelectedIndex = selectedIndex;
    }

    static void UIDropdownComponent_GetInteractable(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<UIDropdownComponent>().m_Interactable;
    }

    static void UIDropdownComponent_SetInteractable(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<UIDropdownComponent>().m_Interactable = *v;
    }

    // --- UIGridLayoutComponent ---

    static void UIGridLayoutComponent_GetCellSize(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIGridLayoutComponent>().m_CellSize;
    }

    static void UIGridLayoutComponent_SetCellSize(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIGridLayoutComponent>().m_CellSize = *v;
    }

    static void UIGridLayoutComponent_GetSpacing(UUID entityID, glm::vec2* out)
    {
        *out = GetEntity(entityID).GetComponent<UIGridLayoutComponent>().m_Spacing;
    }

    static void UIGridLayoutComponent_SetSpacing(UUID entityID, glm::vec2 const* v)
    {
        GetEntity(entityID).GetComponent<UIGridLayoutComponent>().m_Spacing = *v;
    }

    static i32 UIGridLayoutComponent_GetConstraintCount(UUID entityID)
    {
        return GetEntity(entityID).GetComponent<UIGridLayoutComponent>().m_ConstraintCount;
    }

    static void UIGridLayoutComponent_SetConstraintCount(UUID entityID, i32 constraintCount)
    {
        GetEntity(entityID).GetComponent<UIGridLayoutComponent>().m_ConstraintCount = constraintCount;
    }

    // --- UIToggleComponent ---

    static void UIToggleComponent_GetIsOn(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<UIToggleComponent>().m_IsOn;
    }

    static void UIToggleComponent_SetIsOn(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<UIToggleComponent>().m_IsOn = *v;
    }

    static void UIToggleComponent_GetInteractable(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<UIToggleComponent>().m_Interactable;
    }

    static void UIToggleComponent_SetInteractable(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<UIToggleComponent>().m_Interactable = *v;
    }

    // --- ParticleSystemComponent ---

    static void ParticleSystemComponent_GetPlaying(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<ParticleSystemComponent>().System.Playing;
    }

    static void ParticleSystemComponent_SetPlaying(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<ParticleSystemComponent>().System.Playing = *v;
    }

    static void ParticleSystemComponent_GetLooping(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<ParticleSystemComponent>().System.Looping;
    }

    static void ParticleSystemComponent_SetLooping(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<ParticleSystemComponent>().System.Looping = *v;
    }

    static void ParticleSystemComponent_GetEmissionRate(UUID entityID, f32* out)
    {
        *out = GetEntity(entityID).GetComponent<ParticleSystemComponent>().System.Emitter.RateOverTime;
    }

    static void ParticleSystemComponent_SetEmissionRate(UUID entityID, f32 const* v)
    {
        GetEntity(entityID).GetComponent<ParticleSystemComponent>().System.Emitter.RateOverTime = *v;
    }

    // --- ParticleSystemComponent Wind ---

    static void ParticleSystemComponent_GetWindInfluence(UUID entityID, f32* out)
    {
        *out = GetEntity(entityID).GetComponent<ParticleSystemComponent>().System.WindInfluence;
    }

    static void ParticleSystemComponent_SetWindInfluence(UUID entityID, f32 const* v)
    {
        GetEntity(entityID).GetComponent<ParticleSystemComponent>().System.WindInfluence = *v;
    }

    // --- LightProbeComponent ---

    static void LightProbeComponent_GetInfluenceRadius(UUID entityID, f32* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeComponent>().m_InfluenceRadius;
    }

    static void LightProbeComponent_SetInfluenceRadius(UUID entityID, f32 const* v)
    {
        GetEntity(entityID).GetComponent<LightProbeComponent>().m_InfluenceRadius = *v;
    }

    static void LightProbeComponent_GetIntensity(UUID entityID, f32* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeComponent>().m_Intensity;
    }

    static void LightProbeComponent_SetIntensity(UUID entityID, f32 const* v)
    {
        GetEntity(entityID).GetComponent<LightProbeComponent>().m_Intensity = *v;
    }

    static void LightProbeComponent_GetActive(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeComponent>().m_Active;
    }

    static void LightProbeComponent_SetActive(UUID entityID, bool const* v)
    {
        GetEntity(entityID).GetComponent<LightProbeComponent>().m_Active = *v;
    }

    // --- LightProbeVolumeComponent ---

    static void LightProbeVolumeComponent_GetBoundsMin(UUID entityID, glm::vec3* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>().m_BoundsMin;
    }

    static void LightProbeVolumeComponent_SetBoundsMin(UUID entityID, glm::vec3 const* v)
    {
        auto& lpv = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>();
        lpv.m_BoundsMin = *v;
        lpv.m_Dirty = true;
    }

    static void LightProbeVolumeComponent_GetBoundsMax(UUID entityID, glm::vec3* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>().m_BoundsMax;
    }

    static void LightProbeVolumeComponent_SetBoundsMax(UUID entityID, glm::vec3 const* v)
    {
        auto& lpv = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>();
        lpv.m_BoundsMax = *v;
        lpv.m_Dirty = true;
    }

    static void LightProbeVolumeComponent_GetSpacing(UUID entityID, f32* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>().m_Spacing;
    }

    static void LightProbeVolumeComponent_SetSpacing(UUID entityID, f32 const* v)
    {
        auto& lpv = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>();
        lpv.m_Spacing = *v;
        lpv.m_Dirty = true;
    }

    static void LightProbeVolumeComponent_GetIntensity(UUID entityID, f32* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>().m_Intensity;
    }

    static void LightProbeVolumeComponent_SetIntensity(UUID entityID, f32 const* v)
    {
        auto& lpv = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>();
        lpv.m_Intensity = *v;
        lpv.m_Dirty = true;
    }

    static void LightProbeVolumeComponent_GetActive(UUID entityID, bool* out)
    {
        *out = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>().m_Active;
    }

    static void LightProbeVolumeComponent_SetActive(UUID entityID, bool const* v)
    {
        auto& lpv = GetEntity(entityID).GetComponent<LightProbeVolumeComponent>();
        lpv.m_Active = *v;
        lpv.m_Dirty = true;
    }

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

    // --- StreamingVolumeComponent ---

    static void StreamingVolumeComponent_GetLoadRadius(UUID entityID, f32* out)
    {
        *out = GetEntity(entityID).GetComponent<StreamingVolumeComponent>().LoadRadius;
    }

    static void StreamingVolumeComponent_SetLoadRadius(UUID entityID, f32 const* v)
    {
        GetEntity(entityID).GetComponent<StreamingVolumeComponent>().LoadRadius = *v;
    }

    static void StreamingVolumeComponent_GetUnloadRadius(UUID entityID, f32* out)
    {
        *out = GetEntity(entityID).GetComponent<StreamingVolumeComponent>().UnloadRadius;
    }

    static void StreamingVolumeComponent_SetUnloadRadius(UUID entityID, f32 const* v)
    {
        GetEntity(entityID).GetComponent<StreamingVolumeComponent>().UnloadRadius = *v;
    }

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
    // NavAgentComponent //////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////////////////////

    static void NavAgentComponent_GetTargetPosition(UUID entityID, glm::vec3* outTarget)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        *outTarget = entity.GetComponent<NavAgentComponent>().m_TargetPosition;
    }

    static void NavAgentComponent_SetTargetPosition(UUID entityID, glm::vec3 const* target)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        auto& agent = entity.GetComponent<NavAgentComponent>();
        agent.m_TargetPosition = *target;
        agent.m_HasTarget = true;
    }

    static void NavAgentComponent_GetMaxSpeed(UUID entityID, f32* outSpeed)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        *outSpeed = entity.GetComponent<NavAgentComponent>().m_MaxSpeed;
    }

    static void NavAgentComponent_SetMaxSpeed(UUID entityID, f32 const* speed)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        entity.GetComponent<NavAgentComponent>().m_MaxSpeed = *speed;
    }

    static void NavAgentComponent_GetAcceleration(UUID entityID, f32* outAccel)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        *outAccel = entity.GetComponent<NavAgentComponent>().m_Acceleration;
    }

    static void NavAgentComponent_SetAcceleration(UUID entityID, f32 const* accel)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        entity.GetComponent<NavAgentComponent>().m_Acceleration = *accel;
    }

    static void NavAgentComponent_GetStoppingDistance(UUID entityID, f32* outDist)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        *outDist = entity.GetComponent<NavAgentComponent>().m_StoppingDistance;
    }

    static void NavAgentComponent_SetStoppingDistance(UUID entityID, f32 const* dist)
    {
        OLO_PROFILE_FUNCTION();
        auto entity = GetEntity(entityID);
        OLO_CORE_ASSERT(entity.HasComponent<NavAgentComponent>());
        entity.GetComponent<NavAgentComponent>().m_StoppingDistance = *dist;
    }

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
			std::string managedTypename = fmt::format("OloEngine.{}", structName);

			MonoType* managedType = ::mono_reflection_type_from_name(managedTypename.data(), ScriptEngine::GetCoreAssemblyImage());
			if (!managedType)
			{
				OLO_CORE_ERROR("Could not find component type {}", managedTypename);
				return;
			}
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

        OLO_ADD_INTERNAL_CALL(TransformComponent_GetTranslation);
        OLO_ADD_INTERNAL_CALL(TransformComponent_SetTranslation);

        OLO_ADD_INTERNAL_CALL(Rigidbody2DComponent_ApplyLinearImpulse);
        OLO_ADD_INTERNAL_CALL(Rigidbody2DComponent_ApplyLinearImpulseToCenter);

        OLO_ADD_INTERNAL_CALL(Input_IsKeyDown);

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
        // TextComponent //////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(TextComponent_GetText);
        OLO_ADD_INTERNAL_CALL(TextComponent_SetText);
        OLO_ADD_INTERNAL_CALL(TextComponent_GetColor);
        OLO_ADD_INTERNAL_CALL(TextComponent_SetColor);
        OLO_ADD_INTERNAL_CALL(TextComponent_GetKerning);
        OLO_ADD_INTERNAL_CALL(TextComponent_SetKerning);
        OLO_ADD_INTERNAL_CALL(TextComponent_GetLineSpacing);
        OLO_ADD_INTERNAL_CALL(TextComponent_SetLineSpacing);
        ///////////////////////////////////////////////////////////////
        // Audio Source ///////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetVolume);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetVolume);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetPitch);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetPitch);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetPlayOnAwake);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetPlayOnAwake);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetLooping);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetLooping);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetSpatialization);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetSpatialization);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetAttenuationModel);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetAttenuationModel);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetRollOff);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetRollOff);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetMinGain);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetMinGain);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetMaxGain);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetMaxGain);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetMinDistance);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetMinDistance);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetMaxDistance);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetMaxDistance);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetConeInnerAngle);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetConeInnerAngle);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetConeOuterAngle);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetConeOuterAngle);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetConeOuterGain);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetConeOuterGain);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetCone);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_GetDopplerFactor);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_SetDopplerFactor);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_IsPlaying);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_Play);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_Pause);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_UnPause);
        OLO_ADD_INTERNAL_CALL(AudioSourceComponent_Stop);

        ///////////////////////////////////////////////////////////////
        // UI Components //////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(UICanvasComponent_GetSortOrder);
        OLO_ADD_INTERNAL_CALL(UICanvasComponent_SetSortOrder);

        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_GetAnchorMin);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_SetAnchorMin);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_GetAnchorMax);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_SetAnchorMax);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_GetAnchoredPosition);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_SetAnchoredPosition);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_GetSizeDelta);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_SetSizeDelta);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_GetPivot);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_SetPivot);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_GetRotation);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_SetRotation);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_GetScale);
        OLO_ADD_INTERNAL_CALL(UIRectTransformComponent_SetScale);

        OLO_ADD_INTERNAL_CALL(UIImageComponent_GetColor);
        OLO_ADD_INTERNAL_CALL(UIImageComponent_SetColor);

        OLO_ADD_INTERNAL_CALL(UIPanelComponent_GetBackgroundColor);
        OLO_ADD_INTERNAL_CALL(UIPanelComponent_SetBackgroundColor);

        OLO_ADD_INTERNAL_CALL(UITextComponent_GetText);
        OLO_ADD_INTERNAL_CALL(UITextComponent_SetText);
        OLO_ADD_INTERNAL_CALL(UITextComponent_GetFontSize);
        OLO_ADD_INTERNAL_CALL(UITextComponent_SetFontSize);
        OLO_ADD_INTERNAL_CALL(UITextComponent_GetColor);
        OLO_ADD_INTERNAL_CALL(UITextComponent_SetColor);
        OLO_ADD_INTERNAL_CALL(UITextComponent_GetKerning);
        OLO_ADD_INTERNAL_CALL(UITextComponent_SetKerning);
        OLO_ADD_INTERNAL_CALL(UITextComponent_GetLineSpacing);
        OLO_ADD_INTERNAL_CALL(UITextComponent_SetLineSpacing);

        OLO_ADD_INTERNAL_CALL(UIButtonComponent_GetNormalColor);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_SetNormalColor);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_GetHoveredColor);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_SetHoveredColor);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_GetPressedColor);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_SetPressedColor);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_GetDisabledColor);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_SetDisabledColor);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_GetInteractable);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_SetInteractable);
        OLO_ADD_INTERNAL_CALL(UIButtonComponent_GetState);

        OLO_ADD_INTERNAL_CALL(UISliderComponent_GetValue);
        OLO_ADD_INTERNAL_CALL(UISliderComponent_SetValue);
        OLO_ADD_INTERNAL_CALL(UISliderComponent_GetMinValue);
        OLO_ADD_INTERNAL_CALL(UISliderComponent_SetMinValue);
        OLO_ADD_INTERNAL_CALL(UISliderComponent_GetMaxValue);
        OLO_ADD_INTERNAL_CALL(UISliderComponent_SetMaxValue);
        OLO_ADD_INTERNAL_CALL(UISliderComponent_GetInteractable);
        OLO_ADD_INTERNAL_CALL(UISliderComponent_SetInteractable);

        OLO_ADD_INTERNAL_CALL(UICheckboxComponent_GetIsChecked);
        OLO_ADD_INTERNAL_CALL(UICheckboxComponent_SetIsChecked);
        OLO_ADD_INTERNAL_CALL(UICheckboxComponent_GetInteractable);
        OLO_ADD_INTERNAL_CALL(UICheckboxComponent_SetInteractable);

        OLO_ADD_INTERNAL_CALL(UIProgressBarComponent_GetValue);
        OLO_ADD_INTERNAL_CALL(UIProgressBarComponent_SetValue);
        OLO_ADD_INTERNAL_CALL(UIProgressBarComponent_GetMinValue);
        OLO_ADD_INTERNAL_CALL(UIProgressBarComponent_SetMinValue);
        OLO_ADD_INTERNAL_CALL(UIProgressBarComponent_GetMaxValue);
        OLO_ADD_INTERNAL_CALL(UIProgressBarComponent_SetMaxValue);

        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_GetText);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_SetText);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_GetPlaceholder);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_SetPlaceholder);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_GetFontSize);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_SetFontSize);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_GetTextColor);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_SetTextColor);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_GetInteractable);
        OLO_ADD_INTERNAL_CALL(UIInputFieldComponent_SetInteractable);

        OLO_ADD_INTERNAL_CALL(UIScrollViewComponent_GetScrollPosition);
        OLO_ADD_INTERNAL_CALL(UIScrollViewComponent_SetScrollPosition);
        OLO_ADD_INTERNAL_CALL(UIScrollViewComponent_GetContentSize);
        OLO_ADD_INTERNAL_CALL(UIScrollViewComponent_SetContentSize);
        OLO_ADD_INTERNAL_CALL(UIScrollViewComponent_GetScrollSpeed);
        OLO_ADD_INTERNAL_CALL(UIScrollViewComponent_SetScrollSpeed);

        OLO_ADD_INTERNAL_CALL(UIDropdownComponent_GetSelectedIndex);
        OLO_ADD_INTERNAL_CALL(UIDropdownComponent_SetSelectedIndex);
        OLO_ADD_INTERNAL_CALL(UIDropdownComponent_GetInteractable);
        OLO_ADD_INTERNAL_CALL(UIDropdownComponent_SetInteractable);

        OLO_ADD_INTERNAL_CALL(UIGridLayoutComponent_GetCellSize);
        OLO_ADD_INTERNAL_CALL(UIGridLayoutComponent_SetCellSize);
        OLO_ADD_INTERNAL_CALL(UIGridLayoutComponent_GetSpacing);
        OLO_ADD_INTERNAL_CALL(UIGridLayoutComponent_SetSpacing);
        OLO_ADD_INTERNAL_CALL(UIGridLayoutComponent_GetConstraintCount);
        OLO_ADD_INTERNAL_CALL(UIGridLayoutComponent_SetConstraintCount);

        OLO_ADD_INTERNAL_CALL(UIToggleComponent_GetIsOn);
        OLO_ADD_INTERNAL_CALL(UIToggleComponent_SetIsOn);
        OLO_ADD_INTERNAL_CALL(UIToggleComponent_GetInteractable);
        OLO_ADD_INTERNAL_CALL(UIToggleComponent_SetInteractable);

        OLO_ADD_INTERNAL_CALL(ParticleSystemComponent_GetPlaying);
        OLO_ADD_INTERNAL_CALL(ParticleSystemComponent_SetPlaying);
        OLO_ADD_INTERNAL_CALL(ParticleSystemComponent_GetLooping);
        OLO_ADD_INTERNAL_CALL(ParticleSystemComponent_SetLooping);
        OLO_ADD_INTERNAL_CALL(ParticleSystemComponent_GetEmissionRate);
        OLO_ADD_INTERNAL_CALL(ParticleSystemComponent_SetEmissionRate);

        OLO_ADD_INTERNAL_CALL(ParticleSystemComponent_GetWindInfluence);
        OLO_ADD_INTERNAL_CALL(ParticleSystemComponent_SetWindInfluence);

        OLO_ADD_INTERNAL_CALL(LightProbeComponent_GetInfluenceRadius);
        OLO_ADD_INTERNAL_CALL(LightProbeComponent_SetInfluenceRadius);
        OLO_ADD_INTERNAL_CALL(LightProbeComponent_GetIntensity);
        OLO_ADD_INTERNAL_CALL(LightProbeComponent_SetIntensity);
        OLO_ADD_INTERNAL_CALL(LightProbeComponent_GetActive);
        OLO_ADD_INTERNAL_CALL(LightProbeComponent_SetActive);

        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_GetBoundsMin);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_SetBoundsMin);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_GetBoundsMax);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_SetBoundsMax);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_GetSpacing);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_SetSpacing);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_GetIntensity);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_SetIntensity);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_GetActive);
        OLO_ADD_INTERNAL_CALL(LightProbeVolumeComponent_SetActive);
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
        OLO_ADD_INTERNAL_CALL(StreamingVolumeComponent_GetLoadRadius);
        OLO_ADD_INTERNAL_CALL(StreamingVolumeComponent_SetLoadRadius);
        OLO_ADD_INTERNAL_CALL(StreamingVolumeComponent_GetUnloadRadius);
        OLO_ADD_INTERNAL_CALL(StreamingVolumeComponent_SetUnloadRadius);
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
        // MaterialComponent /////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(MaterialComponent_GetShaderGraphHandle);
        OLO_ADD_INTERNAL_CALL(MaterialComponent_SetShaderGraphHandle);

        ///////////////////////////////////////////////////////////////
        // NavAgentComponent /////////////////////////////////////////
        ///////////////////////////////////////////////////////////////
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_GetTargetPosition);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_SetTargetPosition);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_GetMaxSpeed);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_SetMaxSpeed);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_GetAcceleration);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_SetAcceleration);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_GetStoppingDistance);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_SetStoppingDistance);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_HasPath);
        OLO_ADD_INTERNAL_CALL(NavAgentComponent_ClearTarget);

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
    }

} // namespace OloEngine
