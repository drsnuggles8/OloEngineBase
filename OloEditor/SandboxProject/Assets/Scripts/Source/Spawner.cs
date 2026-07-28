using System;
using OloEngine;

namespace Sandbox
{
	// Runtime spawning demo (issue #643) — the C# half of the C#/Lua parity
	// pair; LuaScripts/LuaSpawner.lua is the Lua twin and does the same thing.
	//
	// Attach to any entity. Every SpawnInterval seconds it spawns one entity in
	// a ring around itself, and once RingSize are alive it retires the oldest —
	// the classic projectile / wave-spawner shape, in miniature.
	//
	// Set PrefabHandle in the editor to spawn a prefab (with its whole child
	// hierarchy) instead of a bare entity. Handle rather than path on purpose:
	// a packed runtime serves assets by handle and has no path index, so
	// Scene.FindPrefabByPath only works in the editor.
	//
	// The one rule that shapes this script: spawns and destroys are DEFERRED.
	// The entity you ask for does not exist until the engine drains its command
	// queue at the end of the script phase, so you cannot configure it after
	// the call — pass the transform to the spawn instead. It IS live by the
	// time physics and rendering run later in the same tick.
	public class Spawner : Entity
	{
		// Seconds between spawns.
		public float SpawnInterval = 0.5f;
		// How many spawned entities stay alive before the oldest is retired.
		public int RingSize = 8;
		// Radius of the spawn ring around this entity.
		public float Radius = 3.0f;
		// Prefab to spawn; 0 spawns a bare (transform + tag) entity instead.
		public ulong PrefabHandle = 0;

		private float m_Timer;
		private float m_Angle;
		private Entity[] m_Spawned;
		private int m_Head;
		private int m_Count;

		void OnCreate()
		{
			m_Spawned = new Entity[Math.Max(RingSize, 1)];
			Debug.Log($"[Spawner] Ready — every {SpawnInterval}s, keeping {m_Spawned.Length} alive.");
		}

		void OnUpdate(float ts)
		{
			m_Timer += ts;
			if (m_Timer < SpawnInterval)
				return;
			m_Timer = 0.0f;

			// Retire the oldest before spawning the replacement, so the live
			// count never overshoots. Both requests land in the same drain, in
			// this order.
			if (m_Count == m_Spawned.Length)
			{
				Entity oldest = m_Spawned[m_Head];
				// Null-safe, and safe to call on something already destroyed.
				Entity.Destroy(oldest);
				m_Spawned[m_Head] = null;
				m_Count--;
			}

			Entity spawned = SpawnOne(NextRingPosition());
			if (spawned == null)
				return;

			m_Spawned[m_Head] = spawned;
			m_Head = (m_Head + 1) % m_Spawned.Length;
			m_Count++;
		}

		private Vector3 NextRingPosition()
		{
			m_Angle += 0.7f;
			Vector3 origin = Translation;
			return new Vector3(
				origin.X + (float)Math.Cos(m_Angle) * Radius,
				origin.Y,
				origin.Z + (float)Math.Sin(m_Angle) * Radius);
		}

		private Entity SpawnOne(Vector3 position)
		{
			if (PrefabHandle != 0)
			{
				// Spawns the prefab's whole hierarchy; the transform we pass
				// replaces the prefab's authored one. Destroying the root later
				// takes the children with it.
				return Scene.Instantiate(PrefabHandle, position);
			}

			// A bare entity: transform + tag only. Add components to it from
			// the NEXT tick onwards, once the drain has created it.
			return Scene.CreateEntity("SpawnedByScript", position);
		}
	}
}
