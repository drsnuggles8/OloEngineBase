using System;
using OloEngine;

namespace Sandbox
{
	public class FireMageAI : Entity
	{
		private TransformComponent m_Transform;
		private AbilityComponent m_Abilities;

		public float AggroRange = 10.0f;
		public float CastRange = 6.0f;

		private float m_CastTimer = 0.0f;
		private float m_ShieldTimer = 0.0f;
		private float m_MoveSpeed = 2.5f;
		private bool m_Logged = false;
		private Entity m_Player = null;

		void OnCreate()
		{
			m_Transform = GetComponent<TransformComponent>();
			m_Abilities = GetComponent<AbilityComponent>();
			Debug.Log("[FireMageAI] Spawned - watching...");
		}

		void OnUpdate(float ts)
		{
			if (m_Transform == null || m_Abilities == null)
				return;

			if (!m_Abilities.HasTag("State.Alive"))
				return;

			if (m_Player == null)
				m_Player = FindEntityByName("Player");
			if (m_Player == null)
				return;

			Vector3 myPos = m_Transform.Translation;
			Vector3 playerPos = m_Player.Translation;

			float dx = playerPos.X - myPos.X;
			float dz = playerPos.Z - myPos.Z;
			float distSq = dx * dx + dz * dz;

			if (distSq > AggroRange * AggroRange)
			{
				m_Logged = false;
				return;
			}

			if (!m_Logged)
			{
				Debug.Log("[FireMage] Player in range! Preparing spells...");
				m_Logged = true;
			}

			float dist = (float)Math.Sqrt(distSq);
			float invDist = 1.0f / Math.Max(dist, 0.01f);

			// Self-buff: cast Fire Shield if not active
			m_ShieldTimer -= ts;
			if (m_ShieldTimer <= 0.0f && !m_Abilities.HasTag("Buff.FireShield"))
			{
				if (m_Abilities.TryActivateAbility("Ability.Buff.FireShield"))
				{
					Debug.Log("[FireMage] Fire Shield!");
					m_ShieldTimer = 15.0f;
				}
			}

			// Movement via direct translation (no NavMesh needed)
			if (dist < CastRange * 0.5f)
			{
				// Too close — retreat away from player
				Vector3 pos = myPos;
				pos.X += -dx * invDist * m_MoveSpeed * ts;
				pos.Z += -dz * invDist * m_MoveSpeed * ts;
				m_Transform.Translation = pos;
			}
			else if (dist > CastRange)
			{
				// Too far — move closer
				Vector3 pos = myPos;
				pos.X += dx * invDist * m_MoveSpeed * ts;
				pos.Z += dz * invDist * m_MoveSpeed * ts;
				m_Transform.Translation = pos;
			}
			// else: good range, stand still and cast

			// Cast Fire Bolt when in range
			m_CastTimer -= ts;
			if (dist <= CastRange && m_CastTimer <= 0.0f)
			{
				if (m_Abilities.TryActivateAbility("Ability.Spell.Fire.FireBolt"))
				{
					Debug.Log("[FireMage] Fire Bolt!");
					m_CastTimer = 3.0f;
				}
			}
		}
	}
}
