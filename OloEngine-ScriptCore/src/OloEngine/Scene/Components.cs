using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text;
using System.Threading.Tasks;

namespace OloEngine
{
	public abstract class Component
	{
		public Entity Entity { get; internal set; }
	}

	public class TransformComponent : Component
	{
		public Vector3 Translation
		{
			get
			{
				InternalCalls.TransformComponent_GetTranslation(Entity.ID, out Vector3 translation);
				return translation;
			}
			set
			{
				InternalCalls.TransformComponent_SetTranslation(Entity.ID, ref value);
			}
		}
	}

	public class Rigidbody2DComponent : Component
	{

		public void ApplyLinearImpulse(Vector2 impulse, Vector2 worldPosition, bool wake)
		{
			InternalCalls.Rigidbody2DComponent_ApplyLinearImpulse(Entity.ID, ref impulse, ref worldPosition, wake);
		}

		public void ApplyLinearImpulse(Vector2 impulse, bool wake)
		{
			InternalCalls.Rigidbody2DComponent_ApplyLinearImpulseToCenter(Entity.ID, ref impulse, wake);
		}

	}

    public class TextComponent : Component
    {

        public string Text
        {
            get => InternalCalls.TextComponent_GetText(Entity.ID);
            set => InternalCalls.TextComponent_SetText(Entity.ID, value);
        }

        public Vector4 Color
        {
            get
            {
                InternalCalls.TextComponent_GetColor(Entity.ID, out Vector4 color);
                return color;
            }

            set
            {
                InternalCalls.TextComponent_SetColor(Entity.ID, ref value);
            }
        }

        public float Kerning
        {
            get => InternalCalls.TextComponent_GetKerning(Entity.ID);
            set => InternalCalls.TextComponent_SetKerning(Entity.ID, value);
        }

        public float LineSpacing
        {
            get => InternalCalls.TextComponent_GetLineSpacing(Entity.ID);
            set => InternalCalls.TextComponent_SetLineSpacing(Entity.ID, value);
        }

    }

    public class AudioSourceComponent : Component
	{
		/// <summary>
		/// Distance v Gain curve
		/// <br/>None: No attenuation is calculated
		/// <br/>Inverse: Inverse curve (Realistic and most suitable for games)
		/// <br/>Linear: Linear attenuation
		/// <br/>Exponential: Exponential curve
		/// </summary>
		public enum AttenuationModelType
		{
			None = 0,
			Inverse,
			Linear,
			Exponential
		};

		/// <summary>
		/// The volume of the audio source.
		/// </summary>
		public float volume
		{
			get
			{
				InternalCalls.AudioSource_GetVolume(Entity.ID, out float v);
				return v;
			}

			set
			{
				InternalCalls.AudioSource_SetVolume(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// The pitch of the audio source.
		/// </summary>
		public float pitch
		{
			get
			{
				InternalCalls.AudioSource_GetPitch(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetPitch(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// If set to true, the audio source will automatically start playing on awake.
		/// </summary>
		public bool playOnAwake
		{
			get
			{
				InternalCalls.AudioSource_GetPlayOnAwake(Entity.ID, out bool v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetPlayOnAwake(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Is the audio clip looping?
		/// </summary>
		public bool looping
		{
			get
			{
				InternalCalls.AudioSource_GetLooping(Entity.ID, out bool v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetLooping(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Enables or disables spatialization.
		/// </summary>
		public bool spatialization
		{
			get
			{
				InternalCalls.AudioSource_GetSpatialization(Entity.ID, out bool v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetSpatialization(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Attenuation model describes how sound is attenuated over distance.
		/// </summary>
		public AttenuationModelType attenuationModel
		{
			get
			{
				InternalCalls.AudioSource_GetAttenuationModel(Entity.ID, out int v);
				return (AttenuationModelType)v;
			}
			set
			{
				int v = (int)value;
				InternalCalls.AudioSource_SetAttenuationModel(Entity.ID, ref v);
			}
		}

		/// <summary>
		/// How the AudioSource attenuates over distance.
		/// </summary>
		public float rollOff
		{
			get
			{
				InternalCalls.AudioSource_GetRollOff(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetRollOff(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Min gain the audio source can have.
		/// </summary>
		public float minGain
		{
			get
			{
				InternalCalls.AudioSource_GetMinGain(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetMinGain(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Max gain attainable
		/// </summary>
		public float maxGain
		{
			get
			{
				InternalCalls.AudioSource_GetMaxGain(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetMaxGain(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Within the Min distance the AudioSource will cease to grow louder in volume.
		/// </summary>
		public float minDistance
		{
			get
			{
				InternalCalls.AudioSource_GetMinDistance(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetMinDistance(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// (Logarithmic rolloff) MaxDistance is the distance a sound stops attenuating at.
		/// </summary>
		public float maxDistance
		{
			get
			{
				InternalCalls.AudioSource_GetMaxDistance(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetMaxDistance(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Sets the inner angle (in radians) of a 3d stereo or multichannel sound in speaker space.
		/// </summary>
		public float coneInnerAngle
		{
			get
			{
				InternalCalls.AudioSource_GetConeInnerAngle(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetConeInnerAngle(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Sets the outer angle (in radians) of a 3d stereo or multichannel sound in speaker space.
		/// </summary>
		public float coneOuterAngle
		{
			get
			{
				InternalCalls.AudioSource_GetConeOuterAngle(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetConeOuterAngle(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Gain at the outermost angle of the spread.
		/// </summary>
		public float coneOuterGain
		{
			get
			{
				InternalCalls.AudioSource_GetConeOuterGain(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetConeOuterGain(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Sets the Doppler scale for this AudioSource.
		/// </summary>
		public float dopplerFactor
		{
			get
			{
				InternalCalls.AudioSource_GetDopplerFactor(Entity.ID, out float v);
				return v;
			}
			set
			{
				InternalCalls.AudioSource_SetDopplerFactor(Entity.ID, ref value);
			}
		}

		/// <summary>
		/// Is the clip playing right now?
		/// </summary>
		/// <returns>Playing status</returns>
		public bool IsPlaying()
		{
			InternalCalls.AudioSource_IsPlaying(Entity.ID, out bool v);
			return v;
		}

		/// <summary>
		/// Plays the clip.
		/// </summary>
		public void Play()
		{
			InternalCalls.AudioSource_Play(Entity.ID);
		}

		/// <summary>
		/// 	Pauses playing the clip.
		/// </summary>
		public void Pause()
		{
			InternalCalls.AudioSource_Pause(Entity.ID);
		}

		/// <summary>
		/// Unpause the paused playback of this AudioSource.
		/// </summary>
		public void UnPause()
		{
			InternalCalls.AudioSource_UnPause(Entity.ID);
		}

		/// <summary>
		/// Stops playing the clip.
		/// </summary>
		public void Stop()
		{
			InternalCalls.AudioSource_Stop(Entity.ID);
		}
	}

	// ── UI Components ────────────────────────────────────────────────────

	public class UICanvasComponent : Component
	{
		public int SortOrder
		{
			get => InternalCalls.UICanvasComponent_GetSortOrder(Entity.ID);
			set => InternalCalls.UICanvasComponent_SetSortOrder(Entity.ID, value);
		}
	}

	public class UIRectTransformComponent : Component
	{
		public Vector2 AnchorMin
		{
			get
			{
				InternalCalls.UIRectTransformComponent_GetAnchorMin(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIRectTransformComponent_SetAnchorMin(Entity.ID, ref value);
		}

		public Vector2 AnchorMax
		{
			get
			{
				InternalCalls.UIRectTransformComponent_GetAnchorMax(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIRectTransformComponent_SetAnchorMax(Entity.ID, ref value);
		}

		public Vector2 AnchoredPosition
		{
			get
			{
				InternalCalls.UIRectTransformComponent_GetAnchoredPosition(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIRectTransformComponent_SetAnchoredPosition(Entity.ID, ref value);
		}

		public Vector2 SizeDelta
		{
			get
			{
				InternalCalls.UIRectTransformComponent_GetSizeDelta(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIRectTransformComponent_SetSizeDelta(Entity.ID, ref value);
		}

		public Vector2 Pivot
		{
			get
			{
				InternalCalls.UIRectTransformComponent_GetPivot(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIRectTransformComponent_SetPivot(Entity.ID, ref value);
		}

		public float Rotation
		{
			get => InternalCalls.UIRectTransformComponent_GetRotation(Entity.ID);
			set => InternalCalls.UIRectTransformComponent_SetRotation(Entity.ID, value);
		}

		public Vector2 Scale
		{
			get
			{
				InternalCalls.UIRectTransformComponent_GetScale(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIRectTransformComponent_SetScale(Entity.ID, ref value);
		}
	}

	public class UIImageComponent : Component
	{
		public Vector4 Color
		{
			get
			{
				InternalCalls.UIImageComponent_GetColor(Entity.ID, out Vector4 v);
				return v;
			}
			set => InternalCalls.UIImageComponent_SetColor(Entity.ID, ref value);
		}
	}

	public class UIPanelComponent : Component
	{
		public Vector4 BackgroundColor
		{
			get
			{
				InternalCalls.UIPanelComponent_GetBackgroundColor(Entity.ID, out Vector4 v);
				return v;
			}
			set => InternalCalls.UIPanelComponent_SetBackgroundColor(Entity.ID, ref value);
		}
	}

	public class UITextComponent : Component
	{
		public string Text
		{
			get => InternalCalls.UITextComponent_GetText(Entity.ID);
			set => InternalCalls.UITextComponent_SetText(Entity.ID, value);
		}

		public float FontSize
		{
			get => InternalCalls.UITextComponent_GetFontSize(Entity.ID);
			set => InternalCalls.UITextComponent_SetFontSize(Entity.ID, value);
		}

		public Vector4 Color
		{
			get
			{
				InternalCalls.UITextComponent_GetColor(Entity.ID, out Vector4 v);
				return v;
			}
			set => InternalCalls.UITextComponent_SetColor(Entity.ID, ref value);
		}

		public float Kerning
		{
			get => InternalCalls.UITextComponent_GetKerning(Entity.ID);
			set => InternalCalls.UITextComponent_SetKerning(Entity.ID, value);
		}

		public float LineSpacing
		{
			get => InternalCalls.UITextComponent_GetLineSpacing(Entity.ID);
			set => InternalCalls.UITextComponent_SetLineSpacing(Entity.ID, value);
		}
	}

	public class UIButtonComponent : Component
	{
		public Vector4 NormalColor
		{
			get
			{
				InternalCalls.UIButtonComponent_GetNormalColor(Entity.ID, out Vector4 v);
				return v;
			}
			set => InternalCalls.UIButtonComponent_SetNormalColor(Entity.ID, ref value);
		}

		public Vector4 HoveredColor
		{
			get
			{
				InternalCalls.UIButtonComponent_GetHoveredColor(Entity.ID, out Vector4 v);
				return v;
			}
			set => InternalCalls.UIButtonComponent_SetHoveredColor(Entity.ID, ref value);
		}

		public Vector4 PressedColor
		{
			get
			{
				InternalCalls.UIButtonComponent_GetPressedColor(Entity.ID, out Vector4 v);
				return v;
			}
			set => InternalCalls.UIButtonComponent_SetPressedColor(Entity.ID, ref value);
		}

		public Vector4 DisabledColor
		{
			get
			{
				InternalCalls.UIButtonComponent_GetDisabledColor(Entity.ID, out Vector4 v);
				return v;
			}
			set => InternalCalls.UIButtonComponent_SetDisabledColor(Entity.ID, ref value);
		}

		public bool Interactable
		{
			get
			{
				InternalCalls.UIButtonComponent_GetInteractable(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.UIButtonComponent_SetInteractable(Entity.ID, ref value);
		}

		public int State
		{
			get => InternalCalls.UIButtonComponent_GetState(Entity.ID);
		}
	}

	public class UISliderComponent : Component
	{
		public float Value
		{
			get => InternalCalls.UISliderComponent_GetValue(Entity.ID);
			set => InternalCalls.UISliderComponent_SetValue(Entity.ID, value);
		}

		public float MinValue
		{
			get => InternalCalls.UISliderComponent_GetMinValue(Entity.ID);
			set => InternalCalls.UISliderComponent_SetMinValue(Entity.ID, value);
		}

		public float MaxValue
		{
			get => InternalCalls.UISliderComponent_GetMaxValue(Entity.ID);
			set => InternalCalls.UISliderComponent_SetMaxValue(Entity.ID, value);
		}

		public bool Interactable
		{
			get
			{
				InternalCalls.UISliderComponent_GetInteractable(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.UISliderComponent_SetInteractable(Entity.ID, ref value);
		}
	}

	public class UICheckboxComponent : Component
	{
		public bool IsChecked
		{
			get
			{
				InternalCalls.UICheckboxComponent_GetIsChecked(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.UICheckboxComponent_SetIsChecked(Entity.ID, ref value);
		}

		public bool Interactable
		{
			get
			{
				InternalCalls.UICheckboxComponent_GetInteractable(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.UICheckboxComponent_SetInteractable(Entity.ID, ref value);
		}
	}

	public class UIProgressBarComponent : Component
	{
		public float Value
		{
			get => InternalCalls.UIProgressBarComponent_GetValue(Entity.ID);
			set => InternalCalls.UIProgressBarComponent_SetValue(Entity.ID, value);
		}

		public float MinValue
		{
			get => InternalCalls.UIProgressBarComponent_GetMinValue(Entity.ID);
			set => InternalCalls.UIProgressBarComponent_SetMinValue(Entity.ID, value);
		}

		public float MaxValue
		{
			get => InternalCalls.UIProgressBarComponent_GetMaxValue(Entity.ID);
			set => InternalCalls.UIProgressBarComponent_SetMaxValue(Entity.ID, value);
		}
	}

	public class UIInputFieldComponent : Component
	{
		public string Text
		{
			get => InternalCalls.UIInputFieldComponent_GetText(Entity.ID);
			set => InternalCalls.UIInputFieldComponent_SetText(Entity.ID, value);
		}

		public string Placeholder
		{
			get => InternalCalls.UIInputFieldComponent_GetPlaceholder(Entity.ID);
			set => InternalCalls.UIInputFieldComponent_SetPlaceholder(Entity.ID, value);
		}

		public float FontSize
		{
			get => InternalCalls.UIInputFieldComponent_GetFontSize(Entity.ID);
			set => InternalCalls.UIInputFieldComponent_SetFontSize(Entity.ID, value);
		}

		public Vector4 TextColor
		{
			get
			{
				InternalCalls.UIInputFieldComponent_GetTextColor(Entity.ID, out Vector4 v);
				return v;
			}
			set => InternalCalls.UIInputFieldComponent_SetTextColor(Entity.ID, ref value);
		}

		public bool Interactable
		{
			get
			{
				InternalCalls.UIInputFieldComponent_GetInteractable(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.UIInputFieldComponent_SetInteractable(Entity.ID, ref value);
		}
	}

	public class UIScrollViewComponent : Component
	{
		public Vector2 ScrollPosition
		{
			get
			{
				InternalCalls.UIScrollViewComponent_GetScrollPosition(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIScrollViewComponent_SetScrollPosition(Entity.ID, ref value);
		}

		public Vector2 ContentSize
		{
			get
			{
				InternalCalls.UIScrollViewComponent_GetContentSize(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIScrollViewComponent_SetContentSize(Entity.ID, ref value);
		}

		public float ScrollSpeed
		{
			get => InternalCalls.UIScrollViewComponent_GetScrollSpeed(Entity.ID);
			set => InternalCalls.UIScrollViewComponent_SetScrollSpeed(Entity.ID, value);
		}
	}

	public class UIDropdownComponent : Component
	{
		public int SelectedIndex
		{
			get => InternalCalls.UIDropdownComponent_GetSelectedIndex(Entity.ID);
			set => InternalCalls.UIDropdownComponent_SetSelectedIndex(Entity.ID, value);
		}

		public bool Interactable
		{
			get
			{
				InternalCalls.UIDropdownComponent_GetInteractable(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.UIDropdownComponent_SetInteractable(Entity.ID, ref value);
		}
	}

	public class UIGridLayoutComponent : Component
	{
		public Vector2 CellSize
		{
			get
			{
				InternalCalls.UIGridLayoutComponent_GetCellSize(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIGridLayoutComponent_SetCellSize(Entity.ID, ref value);
		}

		public Vector2 Spacing
		{
			get
			{
				InternalCalls.UIGridLayoutComponent_GetSpacing(Entity.ID, out Vector2 v);
				return v;
			}
			set => InternalCalls.UIGridLayoutComponent_SetSpacing(Entity.ID, ref value);
		}

		public int ConstraintCount
		{
			get => InternalCalls.UIGridLayoutComponent_GetConstraintCount(Entity.ID);
			set => InternalCalls.UIGridLayoutComponent_SetConstraintCount(Entity.ID, value);
		}
	}

	public class UIToggleComponent : Component
	{
		public bool IsOn
		{
			get
			{
				InternalCalls.UIToggleComponent_GetIsOn(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.UIToggleComponent_SetIsOn(Entity.ID, ref value);
		}

		public bool Interactable
		{
			get
			{
				InternalCalls.UIToggleComponent_GetInteractable(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.UIToggleComponent_SetInteractable(Entity.ID, ref value);
		}
	}

	public class ParticleSystemComponent : Component
	{
		public bool Playing
		{
			get
			{
				InternalCalls.ParticleSystemComponent_GetPlaying(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.ParticleSystemComponent_SetPlaying(Entity.ID, ref value);
		}

		public bool Looping
		{
			get
			{
				InternalCalls.ParticleSystemComponent_GetLooping(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.ParticleSystemComponent_SetLooping(Entity.ID, ref value);
		}

		public float EmissionRate
		{
			get
			{
				InternalCalls.ParticleSystemComponent_GetEmissionRate(Entity.ID, out float v);
				return v;
			}
			set => InternalCalls.ParticleSystemComponent_SetEmissionRate(Entity.ID, ref value);
		}

		public float WindInfluence
		{
			get
			{
				InternalCalls.ParticleSystemComponent_GetWindInfluence(Entity.ID, out float v);
				return v;
			}
			set => InternalCalls.ParticleSystemComponent_SetWindInfluence(Entity.ID, ref value);
		}
	}

	public class LightProbeComponent : Component
	{
		public float InfluenceRadius
		{
			get
			{
				InternalCalls.LightProbeComponent_GetInfluenceRadius(Entity.ID, out float v);
				return v;
			}
			set => InternalCalls.LightProbeComponent_SetInfluenceRadius(Entity.ID, ref value);
		}

		public float Intensity
		{
			get
			{
				InternalCalls.LightProbeComponent_GetIntensity(Entity.ID, out float v);
				return v;
			}
			set => InternalCalls.LightProbeComponent_SetIntensity(Entity.ID, ref value);
		}

		public bool Active
		{
			get
			{
				InternalCalls.LightProbeComponent_GetActive(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.LightProbeComponent_SetActive(Entity.ID, ref value);
		}
	}

	public class LightProbeVolumeComponent : Component
	{
		public Vector3 BoundsMin
		{
			get
			{
				InternalCalls.LightProbeVolumeComponent_GetBoundsMin(Entity.ID, out Vector3 v);
				return v;
			}
			set => InternalCalls.LightProbeVolumeComponent_SetBoundsMin(Entity.ID, ref value);
		}

		public Vector3 BoundsMax
		{
			get
			{
				InternalCalls.LightProbeVolumeComponent_GetBoundsMax(Entity.ID, out Vector3 v);
				return v;
			}
			set => InternalCalls.LightProbeVolumeComponent_SetBoundsMax(Entity.ID, ref value);
		}

		public float Spacing
		{
			get
			{
				InternalCalls.LightProbeVolumeComponent_GetSpacing(Entity.ID, out float v);
				return v;
			}
			set => InternalCalls.LightProbeVolumeComponent_SetSpacing(Entity.ID, ref value);
		}

		public float Intensity
		{
			get
			{
				InternalCalls.LightProbeVolumeComponent_GetIntensity(Entity.ID, out float v);
				return v;
			}
			set => InternalCalls.LightProbeVolumeComponent_SetIntensity(Entity.ID, ref value);
		}

		public bool Active
		{
			get
			{
				InternalCalls.LightProbeVolumeComponent_GetActive(Entity.ID, out bool v);
				return v;
			}
			set => InternalCalls.LightProbeVolumeComponent_SetActive(Entity.ID, ref value);
		}

		public void Dirty()
		{
			InternalCalls.LightProbeVolumeComponent_Dirty(Entity.ID);
		}

		public int GetTotalProbeCount()
		{
			InternalCalls.LightProbeVolumeComponent_GetTotalProbeCount(Entity.ID, out int count);
			return count;
		}
	}

	/// <summary>
	/// Stub for the native WaterComponent. No scripting bindings yet.
	/// </summary>
	public class WaterComponent : Component
	{
	}

	/// <summary>
	/// Provides access to scene-level wind settings.
	/// Not a component — use the static members directly.
	/// </summary>
	public static class Wind
	{
		public static bool Enabled
		{
			get { InternalCalls.Scene_GetWindEnabled(out bool v); return v; }
			set => InternalCalls.Scene_SetWindEnabled(ref value);
		}

		public static Vector3 Direction
		{
			get { InternalCalls.Scene_GetWindDirection(out Vector3 v); return v; }
			set => InternalCalls.Scene_SetWindDirection(ref value);
		}

		public static float Speed
		{
			get { InternalCalls.Scene_GetWindSpeed(out float v); return v; }
			set => InternalCalls.Scene_SetWindSpeed(ref value);
		}

		public static float GustStrength
		{
			get { InternalCalls.Scene_GetWindGustStrength(out float v); return v; }
			set => InternalCalls.Scene_SetWindGustStrength(ref value);
		}

		public static float TurbulenceIntensity
		{
			get { InternalCalls.Scene_GetWindTurbulenceIntensity(out float v); return v; }
			set => InternalCalls.Scene_SetWindTurbulenceIntensity(ref value);
		}
	}

	public class StreamingVolumeComponent : Component
	{
		public float LoadRadius
		{
			get
			{
				InternalCalls.StreamingVolumeComponent_GetLoadRadius(Entity.ID, out float v);
				return v;
			}
			set => InternalCalls.StreamingVolumeComponent_SetLoadRadius(Entity.ID, ref value);
		}

		public float UnloadRadius
		{
			get
			{
				InternalCalls.StreamingVolumeComponent_GetUnloadRadius(Entity.ID, out float v);
				return v;
			}
			set => InternalCalls.StreamingVolumeComponent_SetUnloadRadius(Entity.ID, ref value);
		}

		public bool IsLoaded
		{
			get
			{
				InternalCalls.StreamingVolumeComponent_GetIsLoaded(Entity.ID, out bool v);
				return v;
			}
		}
	}

	/// <summary>
	/// Provides access to scene-level streaming.
	/// Not a component — use the static members directly.
	/// </summary>
	public static class SceneStreaming
	{
		public static bool Enabled
		{
			get { InternalCalls.Scene_GetStreamingEnabled(out bool v); return v; }
			set => InternalCalls.Scene_SetStreamingEnabled(ref value);
		}

		public static void LoadRegion(ulong regionId)
		{
			InternalCalls.Scene_LoadRegion(regionId);
		}

		public static void UnloadRegion(ulong regionId)
		{
			InternalCalls.Scene_UnloadRegion(regionId);
		}
	}

	public class DialogueComponent : Component
	{
		public ulong DialogueTree
		{
			get => InternalCalls.DialogueComponent_GetDialogueTree(Entity.ID);
			set => InternalCalls.DialogueComponent_SetDialogueTree(Entity.ID, value);
		}

		public bool AutoTrigger
		{
			get => InternalCalls.DialogueComponent_GetAutoTrigger(Entity.ID);
			set => InternalCalls.DialogueComponent_SetAutoTrigger(Entity.ID, value);
		}

		public float TriggerRadius
		{
			get => InternalCalls.DialogueComponent_GetTriggerRadius(Entity.ID);
			set => InternalCalls.DialogueComponent_SetTriggerRadius(Entity.ID, value);
		}

		public bool TriggerOnce
		{
			get => InternalCalls.DialogueComponent_GetTriggerOnce(Entity.ID);
			set => InternalCalls.DialogueComponent_SetTriggerOnce(Entity.ID, value);
		}

		public bool HasTriggered
		{
			get => InternalCalls.DialogueComponent_GetHasTriggered(Entity.ID);
			set => InternalCalls.DialogueComponent_SetHasTriggered(Entity.ID, value);
		}

		public void StartDialogue()
			=> InternalCalls.DialogueComponent_StartDialogue(Entity.ID);

		public void AdvanceDialogue()
			=> InternalCalls.DialogueComponent_AdvanceDialogue(Entity.ID);

		public void SelectChoice(int choiceIndex)
			=> InternalCalls.DialogueComponent_SelectChoice(Entity.ID, choiceIndex);

		public bool IsActive
			=> InternalCalls.DialogueComponent_IsDialogueActive(Entity.ID);

		public void EndDialogue()
			=> InternalCalls.DialogueComponent_EndDialogue(Entity.ID);
	}

	public static class DialogueVariables
	{
		public static bool GetBool(string key, bool defaultValue = false)
			=> InternalCalls.DialogueVariables_GetBool(key, defaultValue);
		public static void SetBool(string key, bool value)
			=> InternalCalls.DialogueVariables_SetBool(key, value);
		public static int GetInt(string key, int defaultValue = 0)
			=> InternalCalls.DialogueVariables_GetInt(key, defaultValue);
		public static void SetInt(string key, int value)
			=> InternalCalls.DialogueVariables_SetInt(key, value);
		public static float GetFloat(string key, float defaultValue = 0f)
			=> InternalCalls.DialogueVariables_GetFloat(key, defaultValue);
		public static void SetFloat(string key, float value)
			=> InternalCalls.DialogueVariables_SetFloat(key, value);
		public static string GetString(string key, string defaultValue = "")
			=> InternalCalls.DialogueVariables_GetString(key, defaultValue);
		public static void SetString(string key, string value)
			=> InternalCalls.DialogueVariables_SetString(key, value);
		public static bool Has(string key)
			=> InternalCalls.DialogueVariables_Has(key);
		public static void Clear()
			=> InternalCalls.DialogueVariables_Clear();
	}

	public class MaterialComponent : Component
	{
		public ulong ShaderGraphHandle
		{
			get => InternalCalls.MaterialComponent_GetShaderGraphHandle(Entity.ID);
			set => InternalCalls.MaterialComponent_SetShaderGraphHandle(Entity.ID, value);
		}
	}

	public class NavAgentComponent : Component
	{
		public Vector3 TargetPosition
		{
			get
			{
				InternalCalls.NavAgentComponent_GetTargetPosition(Entity.ID, out Vector3 target);
				return target;
			}
			set => InternalCalls.NavAgentComponent_SetTargetPosition(Entity.ID, ref value);
		}

		public float MaxSpeed
		{
			get
			{
				InternalCalls.NavAgentComponent_GetMaxSpeed(Entity.ID, out float speed);
				return speed;
			}
			set => InternalCalls.NavAgentComponent_SetMaxSpeed(Entity.ID, ref value);
		}

		public float Acceleration
		{
			get
			{
				InternalCalls.NavAgentComponent_GetAcceleration(Entity.ID, out float accel);
				return accel;
			}
			set => InternalCalls.NavAgentComponent_SetAcceleration(Entity.ID, ref value);
		}

		public float StoppingDistance
		{
			get
			{
				InternalCalls.NavAgentComponent_GetStoppingDistance(Entity.ID, out float dist);
				return dist;
			}
			set => InternalCalls.NavAgentComponent_SetStoppingDistance(Entity.ID, ref value);
		}

		public bool HasPath => InternalCalls.NavAgentComponent_HasPath(Entity.ID);

		public void ClearTarget() => InternalCalls.NavAgentComponent_ClearTarget(Entity.ID);
	}

	public class AnimationGraphComponent : Component
	{
		public void SetFloat(string paramName, float value)
			=> InternalCalls.AnimationGraphComponent_SetFloat(Entity.ID, paramName, value);

		public void SetBool(string paramName, bool value)
			=> InternalCalls.AnimationGraphComponent_SetBool(Entity.ID, paramName, value);

		public void SetInt(string paramName, int value)
			=> InternalCalls.AnimationGraphComponent_SetInt(Entity.ID, paramName, value);

		public void SetTrigger(string paramName)
			=> InternalCalls.AnimationGraphComponent_SetTrigger(Entity.ID, paramName);

		public float GetFloat(string paramName)
			=> InternalCalls.AnimationGraphComponent_GetFloat(Entity.ID, paramName);

		public bool GetBool(string paramName)
			=> InternalCalls.AnimationGraphComponent_GetBool(Entity.ID, paramName);

		public int GetInt(string paramName)
			=> InternalCalls.AnimationGraphComponent_GetInt(Entity.ID, paramName);

		public string GetCurrentState(int layerIndex = 0)
			=> InternalCalls.AnimationGraphComponent_GetCurrentState(Entity.ID, layerIndex);
	}

	public class MorphTargetComponent : Component
	{
		public void SetWeight(string targetName, float weight)
			=> InternalCalls.MorphTargetComponent_SetWeight(Entity.ID, targetName, weight);

		public float GetWeight(string targetName)
			=> InternalCalls.MorphTargetComponent_GetWeight(Entity.ID, targetName);

		public void ResetAll()
			=> InternalCalls.MorphTargetComponent_ResetAll(Entity.ID);

		public int TargetCount => InternalCalls.MorphTargetComponent_GetTargetCount(Entity.ID);

		public void ApplyExpression(string expressionName, float blend = 1.0f)
			=> InternalCalls.MorphTargetComponent_ApplyExpression(Entity.ID, expressionName, blend);
	}

	public class BehaviorTreeComponent : Component
	{
		public void SetBlackboardBool(string key, bool value)
			=> InternalCalls.BehaviorTreeComponent_SetBlackboardBool(Entity.ID, key, value);

		public bool GetBlackboardBool(string key)
			=> InternalCalls.BehaviorTreeComponent_GetBlackboardBool(Entity.ID, key);

		public void SetBlackboardInt(string key, int value)
			=> InternalCalls.BehaviorTreeComponent_SetBlackboardInt(Entity.ID, key, value);

		public int GetBlackboardInt(string key)
			=> InternalCalls.BehaviorTreeComponent_GetBlackboardInt(Entity.ID, key);

		public void SetBlackboardFloat(string key, float value)
			=> InternalCalls.BehaviorTreeComponent_SetBlackboardFloat(Entity.ID, key, value);

		public float GetBlackboardFloat(string key)
			=> InternalCalls.BehaviorTreeComponent_GetBlackboardFloat(Entity.ID, key);

		public void SetBlackboardString(string key, string value)
			=> InternalCalls.BehaviorTreeComponent_SetBlackboardString(Entity.ID, key, value);

		public string GetBlackboardString(string key)
			=> InternalCalls.BehaviorTreeComponent_GetBlackboardString(Entity.ID, key);

		public void SetBlackboardVec3(string key, Vector3 value)
			=> InternalCalls.BehaviorTreeComponent_SetBlackboardVec3(Entity.ID, key, ref value);

		public Vector3 GetBlackboardVec3(string key)
		{
			InternalCalls.BehaviorTreeComponent_GetBlackboardVec3(Entity.ID, key, out Vector3 result);
			return result;
		}

		public void RemoveBlackboardKey(string key)
			=> InternalCalls.BehaviorTreeComponent_RemoveBlackboardKey(Entity.ID, key);

		public bool HasBlackboardKey(string key)
			=> InternalCalls.BehaviorTreeComponent_HasBlackboardKey(Entity.ID, key);

		public bool IsRunning => InternalCalls.BehaviorTreeComponent_IsRunning(Entity.ID);
	}

	public class StateMachineComponent : Component
	{
		public void SetBlackboardBool(string key, bool value)
			=> InternalCalls.StateMachineComponent_SetBlackboardBool(Entity.ID, key, value);

		public bool GetBlackboardBool(string key)
			=> InternalCalls.StateMachineComponent_GetBlackboardBool(Entity.ID, key);

		public void SetBlackboardInt(string key, int value)
			=> InternalCalls.StateMachineComponent_SetBlackboardInt(Entity.ID, key, value);

		public int GetBlackboardInt(string key)
			=> InternalCalls.StateMachineComponent_GetBlackboardInt(Entity.ID, key);

		public void SetBlackboardFloat(string key, float value)
			=> InternalCalls.StateMachineComponent_SetBlackboardFloat(Entity.ID, key, value);

		public float GetBlackboardFloat(string key)
			=> InternalCalls.StateMachineComponent_GetBlackboardFloat(Entity.ID, key);

		public void SetBlackboardString(string key, string value)
			=> InternalCalls.StateMachineComponent_SetBlackboardString(Entity.ID, key, value);

		public string GetBlackboardString(string key)
			=> InternalCalls.StateMachineComponent_GetBlackboardString(Entity.ID, key);

		public void SetBlackboardVec3(string key, Vector3 value)
			=> InternalCalls.StateMachineComponent_SetBlackboardVec3(Entity.ID, key, ref value);

		public Vector3 GetBlackboardVec3(string key)
		{
			InternalCalls.StateMachineComponent_GetBlackboardVec3(Entity.ID, key, out Vector3 result);
			return result;
		}

		public void RemoveBlackboardKey(string key)
			=> InternalCalls.StateMachineComponent_RemoveBlackboardKey(Entity.ID, key);

		public bool HasBlackboardKey(string key)
			=> InternalCalls.StateMachineComponent_HasBlackboardKey(Entity.ID, key);

		public string CurrentState => InternalCalls.StateMachineComponent_GetCurrentState(Entity.ID);

		public void ForceTransition(string stateId)
			=> InternalCalls.StateMachineComponent_ForceTransition(Entity.ID, stateId);
	}
}
