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
		internal extern static bool Entity_IsValid(ulong entityID);

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
		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void TransformComponent_GetRotation(ulong entityID, out Vector3 rotation);
		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void TransformComponent_SetRotation(ulong entityID, ref Vector3 rotation);
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
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsKeyJustPressed(KeyCode keycode);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsKeyJustReleased(KeyCode keycode);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void Input_GetMousePosition(out Vector2 position);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static void Input_GetWindowSize(out Vector2 size);
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal extern static bool Input_IsMouseButtonDown(int button);
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
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void MaterialComponent_GetAlbedoColor(ulong entityID, out Vector4 color);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void MaterialComponent_SetAlbedoColor(ulong entityID, ref Vector4 color);
		#endregion

		#region ShaderLibrary3D
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool ShaderLibrary3D_LoadShader(string filepath);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool ShaderLibrary3D_Exists(string name);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string ShaderLibrary3D_GetShaderName(string name);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ShaderLibrary3D_ReloadAll();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ShaderLibrary3D_ReloadShader(string name);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern uint ShaderLibrary3D_GetShaderCount();
		#endregion

		#region ShaderLibrary2D
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool ShaderLibrary2D_LoadShader(string filepath);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool ShaderLibrary2D_Exists(string name);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string ShaderLibrary2D_GetShaderName(string name);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ShaderLibrary2D_ReloadAll();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void ShaderLibrary2D_ReloadShader(string name);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern uint ShaderLibrary2D_GetShaderCount();
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
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool NavAgentComponent_GetLockYAxis(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_SetLockYAxis(ulong entityID, bool lockY);
		#endregion

		#region UIWorldAnchorComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern ulong UIWorldAnchorComponent_GetTargetEntity(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIWorldAnchorComponent_SetTargetEntity(ulong entityID, ulong targetEntityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIWorldAnchorComponent_GetWorldOffset(ulong entityID, out Vector3 offset);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIWorldAnchorComponent_SetWorldOffset(ulong entityID, ref Vector3 offset);
		#endregion

		#region NameplateComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool NameplateComponent_GetEnabled(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetEnabled(ulong entityID, bool enabled);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool NameplateComponent_GetShowHealthBar(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetShowHealthBar(ulong entityID, bool show);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool NameplateComponent_GetShowManaBar(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetShowManaBar(ulong entityID, bool show);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_GetWorldOffset(ulong entityID, out Vector3 offset);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetWorldOffset(ulong entityID, ref Vector3 offset);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_GetBarSize(ulong entityID, out Vector2 size);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetBarSize(ulong entityID, ref Vector2 size);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_GetHealthBarColor(ulong entityID, out Vector4 color);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetHealthBarColor(ulong entityID, ref Vector4 color);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_GetManaBarColor(ulong entityID, out Vector4 color);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetManaBarColor(ulong entityID, ref Vector4 color);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_GetBarBackgroundColor(ulong entityID, out Vector4 color);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetBarBackgroundColor(ulong entityID, ref Vector4 color);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float NameplateComponent_GetManaBarGap(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NameplateComponent_SetManaBarGap(ulong entityID, float gap);
		#endregion

		#region AnimationGraphComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AnimationGraphComponent_SetFloat(ulong entityID, string paramName, float value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AnimationGraphComponent_SetBool(ulong entityID, string paramName, bool value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AnimationGraphComponent_SetInt(ulong entityID, string paramName, int value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AnimationGraphComponent_SetTrigger(ulong entityID, string paramName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float AnimationGraphComponent_GetFloat(ulong entityID, string paramName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool AnimationGraphComponent_GetBool(ulong entityID, string paramName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int AnimationGraphComponent_GetInt(ulong entityID, string paramName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string AnimationGraphComponent_GetCurrentState(ulong entityID, int layerIndex);
		#endregion

		#region MorphTargetComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void MorphTargetComponent_SetWeight(ulong entityID, string targetName, float weight);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float MorphTargetComponent_GetWeight(ulong entityID, string targetName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void MorphTargetComponent_ResetAll(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int MorphTargetComponent_GetTargetCount(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void MorphTargetComponent_ApplyExpression(ulong entityID, string expressionName, float blend);
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

		#region BehaviorTreeComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void BehaviorTreeComponent_SetBlackboardBool(ulong entityID, string key, bool value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool BehaviorTreeComponent_GetBlackboardBool(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void BehaviorTreeComponent_SetBlackboardInt(ulong entityID, string key, int value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int BehaviorTreeComponent_GetBlackboardInt(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void BehaviorTreeComponent_SetBlackboardFloat(ulong entityID, string key, float value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float BehaviorTreeComponent_GetBlackboardFloat(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void BehaviorTreeComponent_SetBlackboardString(ulong entityID, string key, string value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string BehaviorTreeComponent_GetBlackboardString(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void BehaviorTreeComponent_SetBlackboardVec3(ulong entityID, string key, ref Vector3 value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void BehaviorTreeComponent_GetBlackboardVec3(ulong entityID, string key, out Vector3 result);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void BehaviorTreeComponent_RemoveBlackboardKey(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool BehaviorTreeComponent_HasBlackboardKey(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool BehaviorTreeComponent_IsRunning(ulong entityID);
		#endregion

		#region StateMachineComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StateMachineComponent_SetBlackboardBool(ulong entityID, string key, bool value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool StateMachineComponent_GetBlackboardBool(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StateMachineComponent_SetBlackboardInt(ulong entityID, string key, int value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int StateMachineComponent_GetBlackboardInt(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StateMachineComponent_SetBlackboardFloat(ulong entityID, string key, float value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float StateMachineComponent_GetBlackboardFloat(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StateMachineComponent_SetBlackboardString(ulong entityID, string key, string value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string StateMachineComponent_GetBlackboardString(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StateMachineComponent_SetBlackboardVec3(ulong entityID, string key, ref Vector3 value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StateMachineComponent_GetBlackboardVec3(ulong entityID, string key, out Vector3 result);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StateMachineComponent_RemoveBlackboardKey(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool StateMachineComponent_HasBlackboardKey(ulong entityID, string key);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string StateMachineComponent_GetCurrentState(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void StateMachineComponent_ForceTransition(ulong entityID, string stateId);
		#endregion

		#region Inventory
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool InventoryComponent_AddItem(ulong entityID, string itemId, int count);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool InventoryComponent_RemoveItem(ulong entityID, string itemId, int count);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool InventoryComponent_HasItem(ulong entityID, string itemId, int count);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int InventoryComponent_CountItem(ulong entityID, string itemId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int InventoryComponent_GetCurrency(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void InventoryComponent_SetCurrency(ulong entityID, int value);
		#endregion

		#region Quest
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool QuestJournalComponent_AcceptQuest(ulong entityID, string questId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool QuestJournalComponent_AbandonQuest(ulong entityID, string questId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool QuestJournalComponent_CompleteQuest(ulong entityID, string questId, string branchChoice);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool QuestJournalComponent_IsQuestActive(ulong entityID, string questId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool QuestJournalComponent_HasCompletedQuest(ulong entityID, string questId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_IncrementObjective(ulong entityID, string questId, string objectiveId, int amount);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_NotifyKill(ulong entityID, string targetTag);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_NotifyCollect(ulong entityID, string itemId, int count);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_NotifyInteract(ulong entityID, string interactableId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_NotifyReachLocation(ulong entityID, string locationId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_SetPlayerLevel(ulong entityID, int level);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int QuestJournalComponent_GetPlayerLevel(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_SetReputation(ulong entityID, string factionId, int value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int QuestJournalComponent_GetReputation(ulong entityID, string factionId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_SetItemCount(ulong entityID, string itemId, int count);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int QuestJournalComponent_GetItemCount(ulong entityID, string itemId);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_SetStat(ulong entityID, string statName, int value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int QuestJournalComponent_GetStat(ulong entityID, string statName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_SetPlayerClass(ulong entityID, string className);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string QuestJournalComponent_GetPlayerClass(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void QuestJournalComponent_SetPlayerFaction(ulong entityID, string factionName);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern string QuestJournalComponent_GetPlayerFaction(ulong entityID);
		#endregion

		#region Logging
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Log_LogMessage(int level, string message);
		#endregion

		#region AbilityComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float AbilityComponent_GetAttribute(ulong entityID, string name);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AbilityComponent_SetAttribute(ulong entityID, string name, float value);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float AbilityComponent_GetCurrentAttribute(ulong entityID, string name);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool AbilityComponent_TryActivateAbility(ulong entityID, string abilityTag);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool AbilityComponent_HasTag(ulong entityID, string tag);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AbilityComponent_AddTag(ulong entityID, string tag);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AbilityComponent_RemoveTag(ulong entityID, string tag);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float AbilityComponent_ApplyDamageToTarget(ulong sourceEntityID, ulong targetEntityID, float rawDamage, string damageTypeTag, bool isCritical);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool AbilityComponent_TryActivateAbilityOnTarget(ulong casterEntityID, string abilityTag, ulong targetEntityID);
		#endregion

		#region Physics
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Physics_Raycast(ref Vector3 origin, ref Vector3 direction, float maxDistance, out Vector3 hitPosition, out Vector3 hitNormal, out float hitDistance, out ulong hitEntityID);
		#endregion

		#region Camera
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Camera_ScreenToWorldRay(ulong cameraEntityID, ref Vector2 screenPos, out Vector3 origin, out Vector3 direction);
		#endregion

		#region Application
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float Application_GetTimeScale();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Application_SetTimeScale(float scale);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Application_QuitGame();
		#endregion

		#region Scene
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Scene_ReloadCurrentScene();
		#endregion
	}
}
