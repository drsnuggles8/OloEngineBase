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
}
