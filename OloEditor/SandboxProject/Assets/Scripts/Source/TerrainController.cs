using System;

using OloEngine;

namespace Sandbox
{
	// Demonstrates issue #293 — driving TerrainComponent's procedural-generation
	// params from a gameplay script (C# side). Attach to the terrain entity.
	//
	// On create it applies a few authored knobs and rolls a fresh seed so a "new
	// game" looks distinct from the editor-authored preview; press R at runtime to
	// regenerate the world with a new seed. Setting params alone does nothing — the
	// engine rebuilds the height field on the next tick only after Regenerate().
	public class TerrainController : Entity
	{
		private TerrainComponent m_Terrain;
		private readonly Random m_Random = new Random();

		// Tunable from the inspector; applied on first run.
		public int Octaves = 7;
		public float RidgeBlend = 0.6f;   // 0 = rolling hills, 1 = sharp ridged mountains
		public float HeightScale = 120.0f;

		void OnCreate()
		{
			if (!HasComponent<TerrainComponent>())
			{
				Console.WriteLine($"TerrainController: entity {ID} has no TerrainComponent");
				return;
			}

			m_Terrain = GetComponent<TerrainComponent>();
			m_Terrain.ProceduralEnabled = true;
			m_Terrain.Octaves = (uint)Math.Max(1, Octaves);
			m_Terrain.RidgeBlend = RidgeBlend;
			m_Terrain.HeightScale = HeightScale;
			Reseed();
		}

		void OnUpdate(float ts)
		{
			// Roll a fresh world on demand (e.g. a "regenerate" button / level reset).
			if (m_Terrain != null && Input.IsKeyJustPressed(KeyCode.R))
				Reseed();
		}

		private void Reseed()
		{
			m_Terrain.Seed = m_Random.Next();
			m_Terrain.Regenerate(); // rebuilds the height field on the next tick
			Console.WriteLine($"TerrainController: regenerated with seed {m_Terrain.Seed}");
		}
	}
}
