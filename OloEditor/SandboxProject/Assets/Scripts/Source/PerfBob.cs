using System;

using OloEngine;

namespace Sandbox
{
	// Minimal per-frame scripting workload for the scripts_swarm_cs perf stress
	// scene (generate_perf_scenes.py): one Translation get + set per tick, so
	// N entities measure the Mono interop crossing cost, not script body cost.
	public class PerfBob : Entity
	{
		private float m_Time;

		void OnUpdate(float ts)
		{
			m_Time += ts;
			Vector3 translation = Translation;
			translation.Y += (float)Math.Sin(m_Time * 2.0f) * ts * 0.5f;
			Translation = translation;
		}
	}
}
