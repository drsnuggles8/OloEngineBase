using System;
using System.Runtime.CompilerServices;

namespace OloEngine
{
	public static partial class InternalCalls
	{
        #region NativeLog
        [MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NativeLog(string message, int parameter);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NativeLog_Vector(ref Vector3 parameter, out Vector3 outResult);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float NativeLog_VectorDot(ref Vector3 parameter);
        #endregion

        #region Entity
        [MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Entity_HasComponent(ulong entityID, Type componentType);

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool Entity_IsValid(ulong entityID);

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern ulong Entity_FindEntityByName(string name);

		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern object GetScriptInstance(ulong entityID);
        #endregion

        #region Rigidbody2DComponent
        [MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Rigidbody2DComponent_ApplyLinearImpulse(ulong entityID, ref Vector2 impulse, ref Vector2 point, bool wake);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void Rigidbody2DComponent_ApplyLinearImpulseToCenter(ulong entityID, ref Vector2 impulse, bool wake);
        #endregion

        #region Input
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsKeyDown(KeyCode keycode);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsKeyJustPressed(KeyCode keycode);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsKeyJustReleased(KeyCode keycode);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern void Input_GetMousePosition(out Vector2 position);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern void Input_GetWindowSize(out Vector2 size);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsMouseButtonDown(int button);
        #endregion

        #region Gamepad
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsGamepadButtonPressed(byte button, int gamepadIndex);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsGamepadButtonJustPressed(byte button, int gamepadIndex);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsGamepadButtonJustReleased(byte button, int gamepadIndex);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern float Input_GetGamepadAxis(byte axis, int gamepadIndex);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern void Input_GetGamepadLeftStick(int gamepadIndex, out Vector2 stick);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern void Input_GetGamepadRightStick(int gamepadIndex, out Vector2 stick);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsGamepadConnected(int gamepadIndex);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern int Input_GetGamepadConnectedCount();
        #endregion

        #region InputActionMapping
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsActionPressed(string actionName);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsActionJustPressed(string actionName);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool Input_IsActionJustReleased(string actionName);
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern float Input_GetActionAxisValue(string actionName);
        #endregion

        #region AudioSourceComponent (hand-written action methods only)
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSourceComponent_SetCone(ulong entityID, ref float innerConeAngle, ref float outerConeAngle, ref float outerConeGain);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSourceComponent_IsPlaying(ulong entityID, out bool v);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSourceComponent_Play(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSourceComponent_Pause(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSourceComponent_UnPause(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioSourceComponent_Stop(ulong entityID);
		#endregion

		#region AudioEvents
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern ulong AudioEvents_PostTrigger(string eventName, ulong objectID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioEvents_StopEvent(ulong eventID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioEvents_PauseEvent(ulong eventID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioEvents_ResumeEvent(ulong eventID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void AudioEvents_StopAll();
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool AudioEvents_IsEventActive(ulong eventID);
		#endregion

		#region LightProbeVolumeComponent (hand-written action methods only)
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

		#region StreamingVolumeComponent (hand-written action methods only)
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

		#region InstancedMeshComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern int InstancedMeshComponent_GetInstanceCount(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void InstancedMeshComponent_AddInstance(ulong entityID,
			ref Vector3 position, ref Vector3 eulerRotation, ref Vector3 scale,
			ref Vector4 color, float custom, int instanceEntityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void InstancedMeshComponent_ClearInstances(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool InstancedMeshComponent_GetCastShadows(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void InstancedMeshComponent_SetCastShadows(ulong entityID, bool value);
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

		#region NavAgentComponent (hand-written action methods only)
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool NavAgentComponent_HasPath(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void NavAgentComponent_ClearTarget(ulong entityID);
		#endregion

		#region UIWorldAnchorComponent (hand-written action methods only)
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern ulong UIWorldAnchorComponent_GetTargetEntity(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void UIWorldAnchorComponent_SetTargetEntity(ulong entityID, ulong targetEntityID);
		#endregion

		#region IKTargetComponent
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool IKTargetComponent_GetAimIKEnabled(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimIKEnabled(ulong entityID, bool enabled);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern uint IKTargetComponent_GetAimBoneIndex(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimBoneIndex(ulong entityID, uint index);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_GetAimTarget(ulong entityID, out Vector3 target);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimTarget(ulong entityID, ref Vector3 target);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_GetAimAxis(ulong entityID, out Vector3 axis);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimAxis(ulong entityID, ref Vector3 axis);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_GetAimOffset(ulong entityID, out Vector3 offset);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimOffset(ulong entityID, ref Vector3 offset);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_GetAimPoleVector(ulong entityID, out Vector3 poleVector);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimPoleVector(ulong entityID, ref Vector3 poleVector);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern uint IKTargetComponent_GetAimChainLength(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimChainLength(ulong entityID, uint length);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float IKTargetComponent_GetAimChainFactor(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimChainFactor(ulong entityID, float factor);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern ulong IKTargetComponent_GetAimTargetEntity(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimTargetEntity(ulong entityID, ulong targetID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float IKTargetComponent_GetAimWeight(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetAimWeight(ulong entityID, float weight);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern bool IKTargetComponent_GetLimbIKEnabled(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetLimbIKEnabled(ulong entityID, bool enabled);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern uint IKTargetComponent_GetLimbBoneIndex(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetLimbBoneIndex(ulong entityID, uint index);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_GetLimbTarget(ulong entityID, out Vector3 target);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetLimbTarget(ulong entityID, ref Vector3 target);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern uint IKTargetComponent_GetLimbChainLength(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetLimbChainLength(ulong entityID, uint length);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern ulong IKTargetComponent_GetLimbTargetEntity(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetLimbTargetEntity(ulong entityID, ulong targetID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern float IKTargetComponent_GetLimbWeight(ulong entityID);
		[MethodImpl(MethodImplOptions.InternalCall)]
		internal static extern void IKTargetComponent_SetLimbWeight(ulong entityID, float weight);
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
