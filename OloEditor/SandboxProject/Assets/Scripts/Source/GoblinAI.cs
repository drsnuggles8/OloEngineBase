using System;
using OloEngine;

namespace Sandbox
{
	public class GoblinAI : Entity
	{
		private TransformComponent m_Transform;
		private AbilityComponent m_Abilities;
		private MaterialComponent m_Material;

		public float AggroRange = 8.0f;
		public float AttackRange = 2.0f;

		private float m_AttackTimer = 0.0f;
		private float m_MoveSpeed = 3.0f;
		private bool m_Logged = false;
		private Entity m_Player = null;

		// Color flash
		private Vector4 m_OriginalColor;
		private float m_FlashTimer = 0.0f;
		private float m_LastHealth = -1.0f;

		void OnCreate()
		{
			m_Transform = GetComponent<TransformComponent>();
			m_Abilities = GetComponent<AbilityComponent>();

			if (HasComponent<MaterialComponent>())
			{
				m_Material = GetComponent<MaterialComponent>();
				m_OriginalColor = m_Material.AlbedoColor;
			}

			Debug.Log("[GoblinAI] Spawned - lurking...");
		}

		void OnUpdate(float ts)
		{
			if (m_Transform == null || m_Abilities == null)
				return;

			if (!m_Abilities.HasTag("State.Alive"))
				return;

			// Damage flash detection
			float curHealth = m_Abilities.GetCurrentAttribute("Health");
			if (m_LastHealth >= 0.0f && curHealth < m_LastHealth && m_Material != null)
			{
				m_Material.AlbedoColor = new Vector4(1.0f, 0.3f, 0.3f, 1.0f);
				m_FlashTimer = 0.15f;
			}
			m_LastHealth = curHealth;

			if (m_FlashTimer > 0.0f && m_Material != null)
			{
				m_FlashTimer -= ts;
				if (m_FlashTimer <= 0.0f)
					m_Material.AlbedoColor = m_OriginalColor;
			}

			if (m_Player == null || m_Player.IsDestroyed)
				m_Player = FindEntityByName("Player");
			if (m_Player == null)
				return;

			Vector3 myPos = m_Transform.Translation;
			Vector3 playerPos = m_Player.Translation;

			float dx = playerPos.X - myPos.X;
			float dz = playerPos.Z - myPos.Z;
			float distSq = dx * dx + dz * dz;

			if (distSq < AggroRange * AggroRange)
			{
				float dist = (float)Math.Sqrt(distSq);

				if (!m_Logged)
				{
					Debug.Log("[Goblin] Player spotted! Charging!");
					m_Logged = true;
				}

				// Move toward player via direct translation (no NavMesh needed)
				if (dist > AttackRange * 0.8f)
				{
					float invDist = 1.0f / Math.Max(dist, 0.01f);
					Vector3 pos = myPos;
					pos.X += dx * invDist * m_MoveSpeed * ts;
					pos.Z += dz * invDist * m_MoveSpeed * ts;
					m_Transform.Translation = pos;
				}

				// Attack if close enough
				if (dist < AttackRange)
				{
					m_AttackTimer -= ts;
					if (m_AttackTimer <= 0.0f)
					{
						if (m_Abilities.TryActivateAbility("Ability.Melee.Bite"))
						{
							Debug.Log("[Goblin] Bite!");
							m_AttackTimer = 2.0f;
						}
					}
				}
			}
			else
			{
				m_Logged = false;
			}

		}
	}
}
