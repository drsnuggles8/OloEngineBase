using System;
using System.Runtime.CompilerServices;

namespace OloEngine
{
	public static class InternalCalls
	{
        #region Entity
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static bool Entity_HasComponent(ulong entityID, Type componentType);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static ulong Entity_FindEntityByName(string name);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static object GetScriptInstance(ulong entityID);
        #endregion

        #region TransformComponent
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void TransformComponent_GetTranslation(ulong entityID, out Vector3 translation);
		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void TransformComponent_SetTranslation(ulong entityID, ref Vector3 translation);
        #endregion

        #region Rigidbody2DComponent
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void Rigidbody2DComponent_ApplyLinearImpulse(ulong entityID, ref Vector2 impulse, ref Vector2 point, bool wake);
		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void Rigidbody2DComponent_ApplyLinearImpulseToCenter(ulong entityID, ref Vector2 impulse, bool wake);
        #endregion

        #region TextComponent
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static string TextComponent_GetText(ulong entityID);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void TextComponent_SetText(ulong entityID, string text);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void TextComponent_GetColor(ulong entityID, out Vector4 color);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void TextComponent_SetColor(ulong entityID, ref Vector4 color);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static float TextComponent_GetKerning(ulong entityID);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void TextComponent_SetKerning(ulong entityID, float kerning);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static float TextComponent_GetLineSpacing(ulong entityID);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void TextComponent_SetLineSpacing(ulong entityID, float lineSpacing);
        #endregion


        #region Rigidbody2DComponent
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsKeyDown(KeyCode keycode);
        #endregion

        #region Gamepad
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsGamepadButtonPressed(byte button, int gamepadIndex);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsGamepadButtonJustPressed(byte button, int gamepadIndex);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsGamepadButtonJustReleased(byte button, int gamepadIndex);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static float Input_GetGamepadAxis(byte axis, int gamepadIndex);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void Input_GetGamepadLeftStick(int gamepadIndex, out Vector2 stick);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void Input_GetGamepadRightStick(int gamepadIndex, out Vector2 stick);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsGamepadConnected(int gamepadIndex);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static int Input_GetGamepadConnectedCount();
        #endregion

        #region InputActionMapping
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsActionPressed(string actionName);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsActionJustPressed(string actionName);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsActionJustReleased(string actionName);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static float Input_GetActionAxisValue(string actionName);
        #endregion

        #region AudioSourceComponent

        [MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetVolume(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetVolume(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetPitch(ulong entityID, out float p);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetPitch(ulong entityID, ref float p);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetPlayOnAwake(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetPlayOnAwake(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetLooping(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetLooping(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetSpatialization(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetSpatialization(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetAttenuationModel(ulong entityID, out int v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetAttenuationModel(ulong entityID, ref int v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetRollOff(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetRollOff(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetMinGain(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetMinGain(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetMaxGain(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetMaxGain(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetMinDistance(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetMinDistance(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetMaxDistance(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetMaxDistance(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetConeInnerAngle(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetConeInnerAngle(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetConeOuterAngle(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetConeOuterAngle(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetConeOuterGain(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetConeOuterGain(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetCone(ulong entityID, ref float innerConeAngle, ref float outerConeAngle, ref float outerConeGain);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_GetDopplerFactor(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_SetDopplerFactor(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_IsPlaying(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_Play(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_Pause(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_UnPause(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSource_Stop(ulong entityID);

		#endregion

		#region UICanvasComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int UICanvasComponent_GetSortOrder(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UICanvasComponent_SetSortOrder(ulong entityID, int sortOrder);
		#endregion

		#region UIRectTransformComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_GetAnchorMin(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_SetAnchorMin(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_GetAnchorMax(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_SetAnchorMax(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_GetAnchoredPosition(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_SetAnchoredPosition(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_GetSizeDelta(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_SetSizeDelta(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_GetPivot(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_SetPivot(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UIRectTransformComponent_GetRotation(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_SetRotation(ulong entityID, float rotation);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_GetScale(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIRectTransformComponent_SetScale(ulong entityID, ref Vector2 v);
		#endregion

		#region UIImageComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIImageComponent_GetColor(ulong entityID, out Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIImageComponent_SetColor(ulong entityID, ref Vector4 v);
		#endregion

		#region UIPanelComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIPanelComponent_GetBackgroundColor(ulong entityID, out Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIPanelComponent_SetBackgroundColor(ulong entityID, ref Vector4 v);
		#endregion

		#region UITextComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string UITextComponent_GetText(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UITextComponent_SetText(ulong entityID, string text);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UITextComponent_GetFontSize(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UITextComponent_SetFontSize(ulong entityID, float fontSize);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UITextComponent_GetColor(ulong entityID, out Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UITextComponent_SetColor(ulong entityID, ref Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UITextComponent_GetKerning(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UITextComponent_SetKerning(ulong entityID, float kerning);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UITextComponent_GetLineSpacing(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UITextComponent_SetLineSpacing(ulong entityID, float lineSpacing);
		#endregion

		#region UIButtonComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_GetNormalColor(ulong entityID, out Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_SetNormalColor(ulong entityID, ref Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_GetHoveredColor(ulong entityID, out Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_SetHoveredColor(ulong entityID, ref Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_GetPressedColor(ulong entityID, out Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_SetPressedColor(ulong entityID, ref Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_GetDisabledColor(ulong entityID, out Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_SetDisabledColor(ulong entityID, ref Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_GetInteractable(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIButtonComponent_SetInteractable(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int UIButtonComponent_GetState(ulong entityID);
		#endregion

		#region UISliderComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UISliderComponent_GetValue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UISliderComponent_SetValue(ulong entityID, float value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UISliderComponent_GetMinValue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UISliderComponent_SetMinValue(ulong entityID, float minValue);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UISliderComponent_GetMaxValue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UISliderComponent_SetMaxValue(ulong entityID, float maxValue);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UISliderComponent_GetInteractable(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UISliderComponent_SetInteractable(ulong entityID, ref bool v);
		#endregion

		#region UICheckboxComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UICheckboxComponent_GetIsChecked(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UICheckboxComponent_SetIsChecked(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UICheckboxComponent_GetInteractable(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UICheckboxComponent_SetInteractable(ulong entityID, ref bool v);
		#endregion

		#region UIProgressBarComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UIProgressBarComponent_GetValue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIProgressBarComponent_SetValue(ulong entityID, float value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UIProgressBarComponent_GetMinValue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIProgressBarComponent_SetMinValue(ulong entityID, float minValue);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UIProgressBarComponent_GetMaxValue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIProgressBarComponent_SetMaxValue(ulong entityID, float maxValue);
		#endregion

		#region UIInputFieldComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string UIInputFieldComponent_GetText(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIInputFieldComponent_SetText(ulong entityID, string text);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string UIInputFieldComponent_GetPlaceholder(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIInputFieldComponent_SetPlaceholder(ulong entityID, string placeholder);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UIInputFieldComponent_GetFontSize(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIInputFieldComponent_SetFontSize(ulong entityID, float fontSize);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIInputFieldComponent_GetTextColor(ulong entityID, out Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIInputFieldComponent_SetTextColor(ulong entityID, ref Vector4 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIInputFieldComponent_GetInteractable(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIInputFieldComponent_SetInteractable(ulong entityID, ref bool v);
		#endregion

		#region UIScrollViewComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIScrollViewComponent_GetScrollPosition(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIScrollViewComponent_SetScrollPosition(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIScrollViewComponent_GetContentSize(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIScrollViewComponent_SetContentSize(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float UIScrollViewComponent_GetScrollSpeed(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIScrollViewComponent_SetScrollSpeed(ulong entityID, float scrollSpeed);
		#endregion

		#region UIDropdownComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int UIDropdownComponent_GetSelectedIndex(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIDropdownComponent_SetSelectedIndex(ulong entityID, int selectedIndex);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIDropdownComponent_GetInteractable(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIDropdownComponent_SetInteractable(ulong entityID, ref bool v);
		#endregion

		#region UIGridLayoutComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIGridLayoutComponent_GetCellSize(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIGridLayoutComponent_SetCellSize(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIGridLayoutComponent_GetSpacing(ulong entityID, out Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIGridLayoutComponent_SetSpacing(ulong entityID, ref Vector2 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int UIGridLayoutComponent_GetConstraintCount(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIGridLayoutComponent_SetConstraintCount(ulong entityID, int constraintCount);
		#endregion

		#region UIToggleComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIToggleComponent_GetIsOn(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIToggleComponent_SetIsOn(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIToggleComponent_GetInteractable(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIToggleComponent_SetInteractable(ulong entityID, ref bool v);
		#endregion

		#region ParticleSystemComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ParticleSystemComponent_GetPlaying(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ParticleSystemComponent_SetPlaying(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ParticleSystemComponent_GetLooping(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ParticleSystemComponent_SetLooping(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ParticleSystemComponent_GetEmissionRate(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ParticleSystemComponent_SetEmissionRate(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ParticleSystemComponent_GetWindInfluence(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ParticleSystemComponent_SetWindInfluence(ulong entityID, ref float v);
		#endregion

		#region LightProbeComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeComponent_GetInfluenceRadius(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeComponent_SetInfluenceRadius(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeComponent_GetIntensity(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeComponent_SetIntensity(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeComponent_GetActive(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeComponent_SetActive(ulong entityID, ref bool v);
		#endregion

		#region LightProbeVolumeComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_GetBoundsMin(ulong entityID, out Vector3 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_SetBoundsMin(ulong entityID, ref Vector3 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_GetBoundsMax(ulong entityID, out Vector3 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_SetBoundsMax(ulong entityID, ref Vector3 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_GetSpacing(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_SetSpacing(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_GetIntensity(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_SetIntensity(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_GetActive(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_SetActive(ulong entityID, ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_Dirty(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void LightProbeVolumeComponent_GetTotalProbeCount(ulong entityID, out int count);
		#endregion

		#region WindSettings
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_GetWindEnabled(out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_SetWindEnabled(ref bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_GetWindDirection(out Vector3 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_SetWindDirection(ref Vector3 v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_GetWindSpeed(out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_SetWindSpeed(ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_GetWindGustStrength(out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_SetWindGustStrength(ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_GetWindTurbulenceIntensity(out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_SetWindTurbulenceIntensity(ref float v);
		#endregion

		#region StreamingVolumeComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StreamingVolumeComponent_GetLoadRadius(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StreamingVolumeComponent_SetLoadRadius(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StreamingVolumeComponent_GetUnloadRadius(ulong entityID, out float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StreamingVolumeComponent_SetUnloadRadius(ulong entityID, ref float v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StreamingVolumeComponent_GetIsLoaded(ulong entityID, out bool v);
		#endregion

		#region SceneStreaming
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_LoadRegion(ulong regionId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_UnloadRegion(ulong regionId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_GetStreamingEnabled(out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_SetStreamingEnabled(ref bool v);
		#endregion

		#region Networking
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Network_IsServer();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Network_IsClient();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Network_IsConnected();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Network_Connect(string address, ushort port);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Network_Disconnect();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Network_StartServer(ushort port);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Network_StopServer();
		#endregion

		#region Dialogue
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_StartDialogue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_AdvanceDialogue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_SelectChoice(ulong entityID, int choiceIndex);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool DialogueComponent_IsDialogueActive(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_EndDialogue(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern ulong DialogueComponent_GetDialogueTree(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_SetDialogueTree(ulong entityID, ulong handle);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool DialogueComponent_GetAutoTrigger(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_SetAutoTrigger(ulong entityID, bool value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float DialogueComponent_GetTriggerRadius(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_SetTriggerRadius(ulong entityID, float value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool DialogueComponent_GetTriggerOnce(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_SetTriggerOnce(ulong entityID, bool value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool DialogueComponent_GetHasTriggered(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueComponent_SetHasTriggered(ulong entityID, bool value);

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool DialogueVariables_GetBool(string key, bool defaultValue);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueVariables_SetBool(string key, bool value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int DialogueVariables_GetInt(string key, int defaultValue);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueVariables_SetInt(string key, int value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float DialogueVariables_GetFloat(string key, float defaultValue);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueVariables_SetFloat(string key, float value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string DialogueVariables_GetString(string key, string defaultValue);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void DialogueVariables_SetString(string key, string value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool DialogueVariables_Has(string key);
		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal static extern void DialogueVariables_Clear();
		#endregion

		#region MaterialComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern ulong MaterialComponent_GetShaderGraphHandle(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void MaterialComponent_SetShaderGraphHandle(ulong entityID, ulong handle);
		#endregion

		#region NavAgentComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_GetTargetPosition(ulong entityID, out Vector3 target);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_SetTargetPosition(ulong entityID, ref Vector3 target);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_GetMaxSpeed(ulong entityID, out float speed);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_SetMaxSpeed(ulong entityID, ref float speed);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_GetAcceleration(ulong entityID, out float accel);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_SetAcceleration(ulong entityID, ref float accel);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_GetStoppingDistance(ulong entityID, out float dist);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_SetStoppingDistance(ulong entityID, ref float dist);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool NavAgentComponent_HasPath(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_ClearTarget(ulong entityID);
		#endregion

		#region SaveGame
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int SaveGame_Save(string slotName, string displayName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int SaveGame_Load(string slotName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int SaveGame_QuickSave();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int SaveGame_QuickLoad();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool SaveGame_DeleteSave(string slotName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool SaveGame_ValidateSave(string slotName);
		#endregion
	}
}
