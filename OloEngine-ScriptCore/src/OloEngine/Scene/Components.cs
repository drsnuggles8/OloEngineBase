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

    public partial class AudioSourceComponent : Component
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
		/// Is the clip playing right now?
		/// </summary>
		/// <returns>Playing status</returns>
		public bool IsPlaying()
		{
			InternalCalls.AudioSourceComponent_IsPlaying(Entity.ID, out bool v);
			return v;
		}

		/// <summary>
		/// Gets or sets the attenuation model using the typed enum.
		/// Hides the generated int-based property.
		/// </summary>
		public new AttenuationModelType AttenuationModel
		{
			get => (AttenuationModelType)InternalCalls.AudioSourceComponent_GetAttenuationModel(Entity.ID);
			set => InternalCalls.AudioSourceComponent_SetAttenuationModel(Entity.ID, (int)value);
		}

		/// <summary>
		/// Plays the clip.
		/// </summary>
		public void Play()
		{
			InternalCalls.AudioSourceComponent_Play(Entity.ID);
		}

		/// <summary>
		/// 	Pauses playing the clip.
		/// </summary>
		public void Pause()
		{
			InternalCalls.AudioSourceComponent_Pause(Entity.ID);
		}

		/// <summary>
		/// Unpause the paused playback of this AudioSource.
		/// </summary>
		public void UnPause()
		{
			InternalCalls.AudioSourceComponent_UnPause(Entity.ID);
		}

		/// <summary>
		/// Stops playing the clip.
		/// </summary>
		public void Stop()
		{
			InternalCalls.AudioSourceComponent_Stop(Entity.ID);
		}
	}

	// ── UI Components ────────────────────────────────────────────────────
	// (All UI property classes are fully generated in Components.Generated.cs)

	public partial class LightProbeVolumeComponent : Component
	{
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

	public partial class StreamingVolumeComponent : Component
	{
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

	public partial class MaterialComponent : Component
	{
		public ulong ShaderGraphHandle
		{
			get => InternalCalls.MaterialComponent_GetShaderGraphHandle(Entity.ID);
			set => InternalCalls.MaterialComponent_SetShaderGraphHandle(Entity.ID, value);
		}
	}

	public partial class NavAgentComponent : Component
	{
		public bool HasPath => InternalCalls.NavAgentComponent_HasPath(Entity.ID);

		public void ClearTarget() => InternalCalls.NavAgentComponent_ClearTarget(Entity.ID);
	}

	public partial class UIWorldAnchorComponent : Component
	{
		public ulong TargetEntity
		{
			get => InternalCalls.UIWorldAnchorComponent_GetTargetEntity(Entity.ID);
			set => InternalCalls.UIWorldAnchorComponent_SetTargetEntity(Entity.ID, value);
		}
	}

	public class IKTargetComponent : Component
	{
		public bool AimIKEnabled
		{
			get => InternalCalls.IKTargetComponent_GetAimIKEnabled(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetAimIKEnabled(Entity.ID, value);
		}

		public uint AimBoneIndex
		{
			get => InternalCalls.IKTargetComponent_GetAimBoneIndex(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetAimBoneIndex(Entity.ID, value);
		}

		public Vector3 AimTarget
		{
			get
			{
				InternalCalls.IKTargetComponent_GetAimTarget(Entity.ID, out Vector3 v);
				return v;
			}
			set => InternalCalls.IKTargetComponent_SetAimTarget(Entity.ID, ref value);
		}

		public Vector3 AimAxis
		{
			get
			{
				InternalCalls.IKTargetComponent_GetAimAxis(Entity.ID, out Vector3 v);
				return v;
			}
			set => InternalCalls.IKTargetComponent_SetAimAxis(Entity.ID, ref value);
		}

		public Vector3 AimOffset
		{
			get
			{
				InternalCalls.IKTargetComponent_GetAimOffset(Entity.ID, out Vector3 v);
				return v;
			}
			set => InternalCalls.IKTargetComponent_SetAimOffset(Entity.ID, ref value);
		}

		public Vector3 AimPoleVector
		{
			get
			{
				InternalCalls.IKTargetComponent_GetAimPoleVector(Entity.ID, out Vector3 v);
				return v;
			}
			set => InternalCalls.IKTargetComponent_SetAimPoleVector(Entity.ID, ref value);
		}

		public uint AimChainLength
		{
			get => InternalCalls.IKTargetComponent_GetAimChainLength(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetAimChainLength(Entity.ID, value);
		}

		public float AimChainFactor
		{
			get => InternalCalls.IKTargetComponent_GetAimChainFactor(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetAimChainFactor(Entity.ID, value);
		}

		public ulong AimTargetEntity
		{
			get => InternalCalls.IKTargetComponent_GetAimTargetEntity(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetAimTargetEntity(Entity.ID, value);
		}

		public float AimWeight
		{
			get => InternalCalls.IKTargetComponent_GetAimWeight(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetAimWeight(Entity.ID, value);
		}

		public bool LimbIKEnabled
		{
			get => InternalCalls.IKTargetComponent_GetLimbIKEnabled(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetLimbIKEnabled(Entity.ID, value);
		}

		public uint LimbBoneIndex
		{
			get => InternalCalls.IKTargetComponent_GetLimbBoneIndex(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetLimbBoneIndex(Entity.ID, value);
		}

		public Vector3 LimbTarget
		{
			get
			{
				InternalCalls.IKTargetComponent_GetLimbTarget(Entity.ID, out Vector3 v);
				return v;
			}
			set => InternalCalls.IKTargetComponent_SetLimbTarget(Entity.ID, ref value);
		}

		public uint LimbChainLength
		{
			get => InternalCalls.IKTargetComponent_GetLimbChainLength(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetLimbChainLength(Entity.ID, value);
		}

		public ulong LimbTargetEntity
		{
			get => InternalCalls.IKTargetComponent_GetLimbTargetEntity(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetLimbTargetEntity(Entity.ID, value);
		}

		public float LimbWeight
		{
			get => InternalCalls.IKTargetComponent_GetLimbWeight(Entity.ID);
			set => InternalCalls.IKTargetComponent_SetLimbWeight(Entity.ID, value);
		}
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

	public class InventoryComponent : Component
	{
		public bool AddItem(string itemId, int count = 1)
		{
			if (string.IsNullOrWhiteSpace(itemId) || count <= 0) return false;
			return InternalCalls.InventoryComponent_AddItem(Entity.ID, itemId, count);
		}

		public bool RemoveItem(string itemId, int count = 1)
		{
			if (string.IsNullOrWhiteSpace(itemId) || count <= 0) return false;
			return InternalCalls.InventoryComponent_RemoveItem(Entity.ID, itemId, count);
		}

		public bool HasItem(string itemId, int count = 1)
		{
			if (string.IsNullOrWhiteSpace(itemId) || count <= 0) return false;
			return InternalCalls.InventoryComponent_HasItem(Entity.ID, itemId, count);
		}

		public int CountItem(string itemId)
		{
			if (string.IsNullOrWhiteSpace(itemId)) return 0;
			return InternalCalls.InventoryComponent_CountItem(Entity.ID, itemId);
		}

		public int Currency
		{
			get => InternalCalls.InventoryComponent_GetCurrency(Entity.ID);
			set => InternalCalls.InventoryComponent_SetCurrency(Entity.ID, value);
		}
	}

	public class ItemPickupComponent : Component { }
	public class ItemContainerComponent : Component { }

	public class QuestJournalComponent : Component
	{
		public bool AcceptQuest(string questId)
		{
			if (string.IsNullOrWhiteSpace(questId)) return false;
			return InternalCalls.QuestJournalComponent_AcceptQuest(Entity.ID, questId);
		}

		public bool AbandonQuest(string questId)
		{
			if (string.IsNullOrWhiteSpace(questId)) return false;
			return InternalCalls.QuestJournalComponent_AbandonQuest(Entity.ID, questId);
		}

		public bool CompleteQuest(string questId, string branchChoice = "")
		{
			if (string.IsNullOrWhiteSpace(questId)) return false;
			return InternalCalls.QuestJournalComponent_CompleteQuest(Entity.ID, questId, branchChoice ?? "");
		}

		public bool IsQuestActive(string questId)
		{
			if (string.IsNullOrWhiteSpace(questId)) return false;
			return InternalCalls.QuestJournalComponent_IsQuestActive(Entity.ID, questId);
		}

		public bool HasCompletedQuest(string questId)
		{
			if (string.IsNullOrWhiteSpace(questId)) return false;
			return InternalCalls.QuestJournalComponent_HasCompletedQuest(Entity.ID, questId);
		}

		public void IncrementObjective(string questId, string objectiveId, int amount = 1)
		{
			if (string.IsNullOrWhiteSpace(questId) || string.IsNullOrWhiteSpace(objectiveId)) return;
			if (amount <= 0) return;
			InternalCalls.QuestJournalComponent_IncrementObjective(Entity.ID, questId, objectiveId, amount);
		}

		public void NotifyKill(string targetTag)
		{
			if (string.IsNullOrWhiteSpace(targetTag)) return;
			InternalCalls.QuestJournalComponent_NotifyKill(Entity.ID, targetTag);
		}

		public void NotifyCollect(string itemId, int count = 1)
		{
			if (string.IsNullOrWhiteSpace(itemId)) return;
			if (count <= 0) return;
			InternalCalls.QuestJournalComponent_NotifyCollect(Entity.ID, itemId, count);
		}

		public void NotifyInteract(string interactableId)
		{
			if (string.IsNullOrWhiteSpace(interactableId)) return;
			InternalCalls.QuestJournalComponent_NotifyInteract(Entity.ID, interactableId);
		}

		public void NotifyReachLocation(string locationId)
		{
			if (string.IsNullOrWhiteSpace(locationId)) return;
			InternalCalls.QuestJournalComponent_NotifyReachLocation(Entity.ID, locationId);
		}

		public void SetPlayerLevel(int level)
		{
			InternalCalls.QuestJournalComponent_SetPlayerLevel(Entity.ID, level);
		}

		public int GetPlayerLevel()
		{
			return InternalCalls.QuestJournalComponent_GetPlayerLevel(Entity.ID);
		}

		public void SetReputation(string factionId, int value)
		{
			if (string.IsNullOrWhiteSpace(factionId)) return;
			InternalCalls.QuestJournalComponent_SetReputation(Entity.ID, factionId, value);
		}

		public int GetReputation(string factionId)
		{
			if (string.IsNullOrWhiteSpace(factionId)) return 0;
			return InternalCalls.QuestJournalComponent_GetReputation(Entity.ID, factionId);
		}

		public void SetItemCount(string itemId, int count)
		{
			if (string.IsNullOrWhiteSpace(itemId)) return;
			InternalCalls.QuestJournalComponent_SetItemCount(Entity.ID, itemId, count);
		}

		public int GetItemCount(string itemId)
		{
			if (string.IsNullOrWhiteSpace(itemId)) return 0;
			return InternalCalls.QuestJournalComponent_GetItemCount(Entity.ID, itemId);
		}

		public void SetStat(string statName, int value)
		{
			if (string.IsNullOrWhiteSpace(statName)) return;
			InternalCalls.QuestJournalComponent_SetStat(Entity.ID, statName, value);
		}

		public int GetStat(string statName)
		{
			if (string.IsNullOrWhiteSpace(statName)) return 0;
			return InternalCalls.QuestJournalComponent_GetStat(Entity.ID, statName);
		}

		public void SetPlayerClass(string className)
		{
			if (string.IsNullOrWhiteSpace(className)) return;
			InternalCalls.QuestJournalComponent_SetPlayerClass(Entity.ID, className);
		}

		public string GetPlayerClass()
		{
			return InternalCalls.QuestJournalComponent_GetPlayerClass(Entity.ID);
		}

		public void SetPlayerFaction(string factionName)
		{
			if (string.IsNullOrWhiteSpace(factionName)) return;
			InternalCalls.QuestJournalComponent_SetPlayerFaction(Entity.ID, factionName);
		}

		public string GetPlayerFaction()
		{
			return InternalCalls.QuestJournalComponent_GetPlayerFaction(Entity.ID);
		}
	}

	public class QuestGiverComponent : Component { }

	public class AbilityComponent : Component
	{
		public float GetAttribute(string name)
		{
			if (string.IsNullOrWhiteSpace(name)) return 0.0f;
			return InternalCalls.AbilityComponent_GetAttribute(Entity.ID, name);
		}

		public void SetAttribute(string name, float value)
		{
			if (string.IsNullOrWhiteSpace(name)) return;
			InternalCalls.AbilityComponent_SetAttribute(Entity.ID, name, value);
		}

		public float GetCurrentAttribute(string name)
		{
			if (string.IsNullOrWhiteSpace(name)) return 0.0f;
			return InternalCalls.AbilityComponent_GetCurrentAttribute(Entity.ID, name);
		}

		public bool TryActivateAbility(string abilityTag)
		{
			if (string.IsNullOrWhiteSpace(abilityTag)) return false;
			return InternalCalls.AbilityComponent_TryActivateAbility(Entity.ID, abilityTag);
		}

		public bool HasTag(string tag)
		{
			if (string.IsNullOrWhiteSpace(tag)) return false;
			return InternalCalls.AbilityComponent_HasTag(Entity.ID, tag);
		}

		public void AddTag(string tag)
		{
			if (string.IsNullOrWhiteSpace(tag)) return;
			InternalCalls.AbilityComponent_AddTag(Entity.ID, tag);
		}

		public void RemoveTag(string tag)
		{
			if (string.IsNullOrWhiteSpace(tag)) return;
			InternalCalls.AbilityComponent_RemoveTag(Entity.ID, tag);
		}

		/// <summary>
		/// Apply calculated damage from this entity to a target entity.
		/// Uses the DamageCalculation pipeline (attack power, crit, defense, resistance).
		/// Returns the final damage dealt after all calculations.
		/// </summary>
		public float ApplyDamageToTarget(ulong targetEntityID, float rawDamage, string damageType = null, bool isCritical = false)
		{
			return InternalCalls.AbilityComponent_ApplyDamageToTarget(Entity.ID, targetEntityID, rawDamage, string.IsNullOrWhiteSpace(damageType) ? "Physical" : damageType, isCritical);
		}

		/// <summary>
		/// Activate an ability on this entity (checks cooldowns/costs) but apply its effects to the target.
		/// </summary>
		public bool TryActivateAbilityOnTarget(string abilityTag, ulong targetEntityID)
		{
			if (string.IsNullOrWhiteSpace(abilityTag)) return false;
			return InternalCalls.AbilityComponent_TryActivateAbilityOnTarget(Entity.ID, abilityTag, targetEntityID);
		}
	}

}
