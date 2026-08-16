using System.Globalization;

namespace OloEngine
{
	/// <summary>
	/// Talks to an entity's node graph from C# (issue #634).
	/// </summary>
	/// <remarks>
	/// Values cross the native boundary as strings so one pair of internal calls
	/// covers every graph pin type; the engine coerces on the way in, exactly as
	/// it does for a YAML-authored default. That means <see cref="SetFloat"/> on
	/// a graph variable declared as Int truncates rather than failing.
	///
	/// Every call is a no-op (returning false / the fallback) outside a runtime
	/// session, or when the entity carries no VisualScriptComponent.
	/// </remarks>
	public static class VisualScript
	{
		/// <summary>
		/// Queues a Custom Event on <paramref name="target"/>'s graph. Pass a null
		/// entity to broadcast to every graph in the scene.
		/// </summary>
		/// <remarks>
		/// Queued, not dispatched: this is normally called from OnUpdate, which
		/// runs inside the engine's script iteration, and a graph reacting to the
		/// event may spawn or destroy entities.
		/// </remarks>
		public static void SendEvent(Entity target, string name, string payload = "")
		{
			if (string.IsNullOrEmpty(name))
				return;

			InternalCalls.VisualScript_SendEvent(target?.ID ?? 0, name, payload ?? string.Empty);
		}

		/// <summary>Reads a graph blackboard variable as text. Null when it does not exist.</summary>
		public static string GetString(Entity entity, string name)
		{
			if (entity == null || string.IsNullOrEmpty(name))
				return null;

			return InternalCalls.VisualScript_GetVariable(entity.ID, name);
		}

		/// <summary>Reads a graph blackboard variable as a float.</summary>
		public static float GetFloat(Entity entity, string name, float fallback = 0.0f)
		{
			string raw = GetString(entity, name);
			// InvariantCulture on both sides: the engine writes with std::to_chars
			// (always '.'), so parsing under a comma-decimal locale would silently
			// read 1.5 as 15.
			return float.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out float value) ? value : fallback;
		}

		/// <summary>Reads a graph blackboard variable as an int.</summary>
		public static int GetInt(Entity entity, string name, int fallback = 0)
		{
			string raw = GetString(entity, name);
			return int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out int value) ? value : fallback;
		}

		/// <summary>Reads a graph blackboard variable as a bool.</summary>
		public static bool GetBool(Entity entity, string name, bool fallback = false)
		{
			string raw = GetString(entity, name);
			if (raw == "true")
				return true;
			if (raw == "false")
				return false;
			return fallback;
		}

		/// <summary>Writes a graph blackboard variable. False when it does not exist.</summary>
		public static bool SetString(Entity entity, string name, string value)
		{
			if (entity == null || string.IsNullOrEmpty(name))
				return false;

			return InternalCalls.VisualScript_SetVariable(entity.ID, name, value ?? string.Empty);
		}

		/// <summary>Writes a graph blackboard variable from a float.</summary>
		public static bool SetFloat(Entity entity, string name, float value)
		{
			// "G9" is the documented round-trip format for float: 9 significant
			// digits always recover the exact value, and unlike "R" it has no
			// history of losing the last digit on some runtimes.
			return SetString(entity, name, value.ToString("G9", CultureInfo.InvariantCulture));
		}

		/// <summary>Writes a graph blackboard variable from an int.</summary>
		public static bool SetInt(Entity entity, string name, int value)
		{
			return SetString(entity, name, value.ToString(CultureInfo.InvariantCulture));
		}

		/// <summary>Writes a graph blackboard variable from a bool.</summary>
		public static bool SetBool(Entity entity, string name, bool value)
		{
			return SetString(entity, name, value ? "true" : "false");
		}
	}
}
