using System;
using OloEngine;

namespace Sandbox
{
	public class PlayerController : Entity
	{
		private TransformComponent m_Transform;
		private AbilityComponent m_Abilities;
		private NavAgentComponent m_NavAgent;
		private DamageFlashHelper m_DamageFlash;

		public float MoveSpeed = 5.0f;

		// Click-to-move state
		private bool m_IsNavigating = false;
		private bool m_WasMouseDown = false; // Track mouse state for single-click detection

		// VFX entity references (found by name)
		private Entity m_SlashVFX;
		private Entity m_FireballVFX;
		private Entity m_HealVFX;
		private float m_VFXTimer = 0.0f;
		private Entity m_ActiveVFX = null;

		// Death state
		private bool m_IsDead = false;

		void OnCreate()
		{
			m_Transform = GetComponent<TransformComponent>();
			m_Abilities = GetComponent<AbilityComponent>();

			if (HasComponent<NavAgentComponent>())
			{
				m_NavAgent = GetComponent<NavAgentComponent>();
				m_NavAgent.LockYAxis = true; // Top-down game: move on XZ only
			}

			MaterialComponent material = null;
			if (HasComponent<MaterialComponent>())
				material = GetComponent<MaterialComponent>();
			m_DamageFlash = new DamageFlashHelper(material);

			m_SlashVFX = FindEntityByName("Slash VFX");
			m_FireballVFX = FindEntityByName("Fireball VFX");
			m_HealVFX = FindEntityByName("Heal VFX");

			Debug.Log("[Player] Ready - WASD/Click to move, 1=Slash, 2=Fireball, 3=Heal");
		}

		void OnUpdate(float ts)
		{
			if (m_Transform == null || m_Abilities == null)
				return;

			// Death check
			if (!m_IsDead && m_Abilities.GetCurrentAttribute("Health") <= 0.0f)
			{
				m_IsDead = true;
				m_Abilities.RemoveTag("State.Alive");

				// Stop any active VFX
				if (m_ActiveVFX != null)
				{
					if (m_ActiveVFX.HasComponent<ParticleSystemComponent>())
						m_ActiveVFX.GetComponent<ParticleSystemComponent>().Playing = false;
					m_ActiveVFX = null;
					m_VFXTimer = 0.0f;
				}

				Debug.Log("[Player] Died!");

				// Notify GameManager
				Entity gm = FindEntityByName("GameManager");
				if (gm != null)
				{
					var gmScript = gm.As<GameManager>();
					if (gmScript != null)
						gmScript.OnPlayerDeath();
				}
				return;
			}

			if (m_IsDead)
				return;

			// Skip gameplay input while paused
			if (GameApplication.TimeScale == 0.0f)
				return;

			// ── Click-to-move (triggers once per click, not every held frame) ──
			bool mouseDown = Input.IsMouseButtonDown(0);
			bool mouseJustPressed = mouseDown && !m_WasMouseDown;
			m_WasMouseDown = mouseDown;

			if (mouseJustPressed)
			{
				Vector2 mousePos = Input.GetMousePosition();
				Entity cameraEntity = FindEntityByName("Camera");
				if (cameraEntity != null)
				{
					// Convert screen position to normalized [0,1] coordinates
					Vector2 windowSize = Input.GetWindowSize();
					if (windowSize.X > 0.0f && windowSize.Y > 0.0f)
					{
						Vector2 screenNorm = new Vector2(mousePos.X / windowSize.X, 1.0f - mousePos.Y / windowSize.Y);
						if (OloEngine.Camera.ScreenToWorldRay(cameraEntity.ID, screenNorm, out Vector3 origin, out Vector3 direction))
						{
							// Raycast to find ground hit
							RaycastHit hitResult;
							bool didHit = Physics.Raycast(origin, direction, 500.0f, out hitResult);
							if (didHit && m_NavAgent != null)
							{
								m_NavAgent.TargetPosition = hitResult.Position;
								m_IsNavigating = true;
							}
						}
					}
				}
			}

			// ── WASD override (cancels navigation) ──
			float dx = 0.0f;
			float dz = 0.0f;

			if (Input.IsKeyDown(KeyCode.W)) dz = -1.0f;
			if (Input.IsKeyDown(KeyCode.S)) dz = 1.0f;
			if (Input.IsKeyDown(KeyCode.A)) dx = -1.0f;
			if (Input.IsKeyDown(KeyCode.D)) dx = 1.0f;

			float dirLenSq = dx * dx + dz * dz;
			if (dirLenSq > 0.01f)
			{
				// WASD pressed — cancel nav and move directly
				if (m_IsNavigating && m_NavAgent != null)
				{
					m_NavAgent.ClearTarget();
					m_IsNavigating = false;
				}

				float invLen = 1.0f / (float)Math.Sqrt(dirLenSq);
				dx *= invLen;
				dz *= invLen;

				Vector3 pos = m_Transform.Translation;
				pos.X += dx * MoveSpeed * ts;
				pos.Z += dz * MoveSpeed * ts;
				m_Transform.Translation = pos;
			}

			// ── Ability hotkeys (one-shot) ──
			if (Input.IsKeyJustPressed(KeyCode.D1))
			{
				if (m_Abilities.TryActivateAbility("Ability.Melee.Slash"))
				{
					Debug.Log("[Player] Slash!");
					TriggerVFX(m_SlashVFX, 0.3f);
					m_DamageFlash.Flash(new Vector4(1.0f, 1.0f, 0.3f, 1.0f));
				}
			}

			if (Input.IsKeyJustPressed(KeyCode.D2))
			{
				if (m_Abilities.TryActivateAbility("Ability.Spell.Fire.Fireball"))
				{
					Debug.Log("[Player] Fireball!");
					TriggerVFX(m_FireballVFX, 0.5f);
					m_DamageFlash.Flash(new Vector4(1.0f, 0.4f, 0.1f, 1.0f));
				}
			}

			if (Input.IsKeyJustPressed(KeyCode.D3))
			{
				if (m_Abilities.TryActivateAbility("Ability.Spell.Heal"))
				{
					Debug.Log("[Player] Heal!");
					TriggerVFX(m_HealVFX, 0.6f);
					m_DamageFlash.Flash(new Vector4(0.3f, 1.0f, 0.3f, 1.0f));
				}
			}

			// ── Update VFX timer ──
			if (m_ActiveVFX != null)
			{
				m_VFXTimer -= ts;
				if (m_VFXTimer <= 0.0f)
				{
					if (m_ActiveVFX.HasComponent<ParticleSystemComponent>())
						m_ActiveVFX.GetComponent<ParticleSystemComponent>().Playing = false;
					m_ActiveVFX = null;
				}
			}

			// ── Color flash lerp back ──
			m_DamageFlash.Update(ts, m_Abilities.GetCurrentAttribute("Health"));
		}

		private void TriggerVFX(Entity vfxEntity, float duration)
		{
			if (vfxEntity == null)
				return;

			// Stop previous VFX if still playing
			if (m_ActiveVFX != null && m_ActiveVFX != vfxEntity)
			{
				if (m_ActiveVFX.HasComponent<ParticleSystemComponent>())
					m_ActiveVFX.GetComponent<ParticleSystemComponent>().Playing = false;
			}

			// Position VFX at player
			if (vfxEntity.HasComponent<TransformComponent>())
			{
				var vfxTransform = vfxEntity.GetComponent<TransformComponent>();
				Vector3 pos = m_Transform.Translation;
				pos.Y += 1.0f; // Offset up
				vfxTransform.Translation = pos;
			}

			if (vfxEntity.HasComponent<ParticleSystemComponent>())
			{
				vfxEntity.GetComponent<ParticleSystemComponent>().Playing = true;
				m_ActiveVFX = vfxEntity;
				m_VFXTimer = duration;
			}
		}
	}
}
