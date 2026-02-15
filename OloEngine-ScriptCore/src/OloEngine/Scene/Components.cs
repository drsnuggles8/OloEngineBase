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
}
