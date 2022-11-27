using System;
using System.Runtime.CompilerServices;

namespace OloEngine
{
	public static class InternalCalls
	{
		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static bool Entity_HasComponent(ulong entityID, Type componentType);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static ulong Entity_FindEntityByName(string name);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static object GetScriptInstance(ulong entityID);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void TransformComponent_GetTranslation(ulong entityID, out Vector3 translation);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void TransformComponent_SetTranslation(ulong entityID, ref Vector3 translation);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void Rigidbody2DComponent_ApplyLinearImpulse(ulong entityID, ref Vector2 impulse, ref Vector2 point, bool wake);

		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static void Rigidbody2DComponent_ApplyLinearImpulseToCenter(ulong entityID, ref Vector2 impulse, bool wake);
		[MethodImplAttribute(MethodImplOptions.InternalCall)]
		internal extern static bool Input_IsKeyDown(KeyCode keycode);

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
	}
}