using System;
using OloEngine;

namespace Sandbox
{
	public class PlayerController : Entity
	{
		private TransformComponent m_Transform;
		private AbilityComponent m_Abilities;

		public float MoveSpeed = 5.0f;

		// One-shot key tracking
		private bool m_Key1Was = false;
		private bool m_Key2Was = false;
		private bool m_Key3Was = false;
		private bool m_KeyTabWas = false;

		void OnCreate()
		{
			m_Transform = GetComponent<TransformComponent>();
			m_Abilities = GetComponent<AbilityComponent>();

			Debug.Log("[PlayerController] Ready - WASD to move, 1=Slash, 2=Fireball, 3=Heal, Tab=Stats");
		}

		void OnUpdate(float ts)
		{
			if (m_Transform == null)
				return;

			// ── WASD direct movement ──
			float dx = 0.0f;
			float dz = 0.0f;

			if (Input.IsKeyDown(KeyCode.W)) dz = -1.0f;
			if (Input.IsKeyDown(KeyCode.S)) dz = 1.0f;
			if (Input.IsKeyDown(KeyCode.A)) dx = -1.0f;
			if (Input.IsKeyDown(KeyCode.D)) dx = 1.0f;

			float dirLenSq = dx * dx + dz * dz;
			if (dirLenSq > 0.01f)
			{
				float invLen = 1.0f / (float)Math.Sqrt(dirLenSq);
				dx *= invLen;
				dz *= invLen;

				Vector3 pos = m_Transform.Translation;
				pos.X += dx * MoveSpeed * ts;
				pos.Z += dz * MoveSpeed * ts;
				m_Transform.Translation = pos;
			}

			// ── Ability hotkeys (one-shot: fires once per key press) ──
			if (m_Abilities != null)
			{
				bool key1 = Input.IsKeyDown(KeyCode.D1);
				bool key2 = Input.IsKeyDown(KeyCode.D2);
				bool key3 = Input.IsKeyDown(KeyCode.D3);
				bool keyTab = Input.IsKeyDown(KeyCode.Tab);

				if (key1 && !m_Key1Was)
				{
					if (m_Abilities.TryActivateAbility("Ability.Melee.Slash"))
						Debug.Log("[Player] Slash!");
					else
						Debug.Log("[Player] Slash on cooldown");
				}

				if (key2 && !m_Key2Was)
				{
					if (m_Abilities.TryActivateAbility("Ability.Spell.Fire.Fireball"))
						Debug.Log("[Player] Fireball!");
					else
						Debug.Log("[Player] Fireball - cooldown or not enough mana");
				}

				if (key3 && !m_Key3Was)
				{
					if (m_Abilities.TryActivateAbility("Ability.Spell.Heal"))
						Debug.Log("[Player] Heal!");
					else
						Debug.Log("[Player] Heal - cooldown or not enough mana");
				}

				if (keyTab && !m_KeyTabWas)
				{
					float hp = m_Abilities.GetCurrentAttribute("Health");
					float maxHp = m_Abilities.GetCurrentAttribute("MaxHealth");
					float mana = m_Abilities.GetCurrentAttribute("Mana");
					float maxMana = m_Abilities.GetCurrentAttribute("MaxMana");
					float armor = m_Abilities.GetCurrentAttribute("Armor");
					Debug.Log($"[Player] HP: {hp:F0}/{maxHp:F0}  Mana: {mana:F0}/{maxMana:F0}  Armor: {armor:F0}");
				}

				m_Key1Was = key1;
				m_Key2Was = key2;
				m_Key3Was = key3;
				m_KeyTabWas = keyTab;
			}
		}
	}
}
