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

    static u64 Entity_FindEntityByName(MonoString* name)
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
    }

} // namespace OloEngine
