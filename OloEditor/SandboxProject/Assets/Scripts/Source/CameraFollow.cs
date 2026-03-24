using System;
using OloEngine;

namespace Sandbox
{
	public class CameraFollow : Entity
	{
		private TransformComponent m_Transform;
		private Entity m_Player;

		// Camera offset from player (overhead isometric-style)
		public float OffsetY = 12.0f;
		public float OffsetZ = 12.0f;
		public float SmoothSpeed = 5.0f;

		void OnCreate()
		{
			m_Transform = GetComponent<TransformComponent>();
			m_Player = FindEntityByName("Player");
		}

		void OnUpdate(float ts)
		{
			if (m_Transform == null || m_Player == null)
				return;

			Vector3 playerPos = m_Player.Translation;
			Vector3 targetPos = new Vector3(playerPos.X, playerPos.Y + OffsetY, playerPos.Z + OffsetZ);

			// Smooth follow with lerp
			Vector3 currentPos = m_Transform.Translation;
			float t = Math.Min(1.0f, SmoothSpeed * ts);
			currentPos.X += (targetPos.X - currentPos.X) * t;
			currentPos.Y += (targetPos.Y - currentPos.Y) * t;
			currentPos.Z += (targetPos.Z - currentPos.Z) * t;
			m_Transform.Translation = currentPos;
		}
	}
}
