using System;
using OloEngine;

namespace Sandbox
{
	public class GameManager : Entity
	{
		// UI overlay entities (found by name)
		private Entity m_PauseOverlay;
		private Entity m_DeathOverlay;
		private Entity m_VictoryOverlay;
		private Entity m_HUD;

		// Player reference
		private Entity m_Player;

		// Enemy tracking for win condition
		private Entity[] m_Enemies;

		// State
		private bool m_IsPaused = false;
		private bool m_IsPlayerDead = false;
		private bool m_IsVictory = false;

		// Colors
		private static readonly Vector4 Hidden = new Vector4(0, 0, 0, 0);
		private static readonly Vector4 PausePanelColor = new Vector4(0.0f, 0.0f, 0.05f, 0.85f);
		private static readonly Vector4 PauseTextColor = new Vector4(1.0f, 1.0f, 0.5f, 1.0f);
		private static readonly Vector4 DeathPanelColor = new Vector4(0.15f, 0.0f, 0.0f, 0.85f);
		private static readonly Vector4 DeathTextColor = new Vector4(1.0f, 0.2f, 0.2f, 1.0f);
		private static readonly Vector4 VictoryPanelColor = new Vector4(0.0f, 0.1f, 0.0f, 0.85f);
		private static readonly Vector4 VictoryTextColor = new Vector4(0.3f, 1.0f, 0.3f, 1.0f);

		void OnCreate()
		{
			m_PauseOverlay = FindEntityByName("Pause Overlay");
			m_DeathOverlay = FindEntityByName("Death Overlay");
			m_VictoryOverlay = FindEntityByName("Victory Overlay");
			m_HUD = FindEntityByName("HUD");
			m_Player = FindEntityByName("Player");

			WarnIfMissing(m_PauseOverlay, "Pause Overlay");
			WarnIfMissing(m_DeathOverlay, "Death Overlay");
			WarnIfMissing(m_VictoryOverlay, "Victory Overlay");
			WarnIfMissing(m_HUD, "HUD");
			WarnIfMissing(m_Player, "Player");

			// Gather enemies for win condition
			m_Enemies = new Entity[]
			{
				FindEntityByName("Goblin"),
				FindEntityByName("Fire Mage")
			};

			// Hide overlays at start
			SetOverlayVisible(m_PauseOverlay, false);
			SetOverlayVisible(m_DeathOverlay, false);
			SetOverlayVisible(m_VictoryOverlay, false);

			Debug.Log("[GameManager] Initialized - Esc=Pause, WASD/Click=Move, 1/2/3=Abilities");
		}

		void OnUpdate(float ts)
		{
			// ── Victory screen (R to restart) ──
			if (m_IsVictory)
			{
				if (Input.IsKeyJustPressed(KeyCode.R))
				{
					Debug.Log("[Game] Restarting scene...");
					GameApplication.TimeScale = 1.0f;
					SceneManager.ReloadCurrentScene();
				}
				return;
			}

			// ── Death screen (checked before pause — takes priority) ──
			if (m_IsPlayerDead)
			{
				if (Input.IsKeyJustPressed(KeyCode.R))
				{
					Debug.Log("[Game] Restarting scene...");
					GameApplication.TimeScale = 1.0f;
					SceneManager.ReloadCurrentScene();
				}
				return;
			}

			// ── Escape → toggle pause ──
			if (Input.IsKeyJustPressed(KeyCode.Escape))
			{
				m_IsPaused = !m_IsPaused;

				if (m_IsPaused)
				{
					GameApplication.TimeScale = 0.0f;
					SetUIText(m_PauseOverlay, "PAUSED\n\nESC - Resume\nQ - Quit Game");
					SetOverlayVisible(m_PauseOverlay, true, PausePanelColor, PauseTextColor);
					Debug.Log("[Game] Paused");
				}
				else
				{
					GameApplication.TimeScale = 1.0f;
					SetOverlayVisible(m_PauseOverlay, false);
					Debug.Log("[Game] Resumed");
				}
			}

			// ── While paused ──
			if (m_IsPaused)
			{
				if (Input.IsKeyJustPressed(KeyCode.Q))
				{
					Debug.Log("[Game] Quitting...");
					GameApplication.Quit();
				}
				return;
			}

			// ── Win condition: all enemies dead ──
			if (AreAllEnemiesDead())
			{
				m_IsVictory = true;

				if (m_IsPaused)
				{
					m_IsPaused = false;
					SetOverlayVisible(m_PauseOverlay, false);
				}

				GameApplication.TimeScale = 0.0f;
				SetUIText(m_VictoryOverlay, "VICTORY!\n\nAll enemies defeated\n\nPress R to Restart");
				SetOverlayVisible(m_VictoryOverlay, true, VictoryPanelColor, VictoryTextColor);
				Debug.Log("[Game] Victory - all enemies defeated!");
			}
		}

		/// <summary>
		/// Called by PlayerController when the player dies.
		/// </summary>
		public void OnPlayerDeath()
		{
			if (m_IsPlayerDead)
				return;

			m_IsPlayerDead = true;

			// Clear pause state if paused when death occurs
			if (m_IsPaused)
			{
				m_IsPaused = false;
				SetOverlayVisible(m_PauseOverlay, false);
			}

			GameApplication.TimeScale = 0.0f;

			SetUIText(m_DeathOverlay, "YOU DIED\n\nPress R to Restart");
			SetOverlayVisible(m_DeathOverlay, true, DeathPanelColor, DeathTextColor);

			Debug.Log("[Game] Player died - showing death screen");
		}

		private void SetOverlayVisible(Entity entity, bool visible, Vector4 panelColor = default, Vector4 textColor = default)
		{
			if (entity == null)
				return;

			if (visible)
			{
				if (entity.HasComponent<UIPanelComponent>())
					entity.GetComponent<UIPanelComponent>().BackgroundColor = panelColor;
				if (entity.HasComponent<UITextComponent>())
					entity.GetComponent<UITextComponent>().Color = textColor;
			}
			else
			{
				if (entity.HasComponent<UIPanelComponent>())
					entity.GetComponent<UIPanelComponent>().BackgroundColor = Hidden;
				if (entity.HasComponent<UITextComponent>())
					entity.GetComponent<UITextComponent>().Color = Hidden;
			}
		}

		private bool AreAllEnemiesDead()
		{
			if (m_Enemies == null || m_Enemies.Length == 0)
				return false;

			for (int i = 0; i < m_Enemies.Length; i++)
			{
				Entity e = m_Enemies[i];
				if (e == null || e.IsDestroyed)
					continue; // destroyed counts as dead

				if (!e.HasComponent<AbilityComponent>())
					continue; // no ability component = can't check tags, skip

				var ac = e.GetComponent<AbilityComponent>();
				if (ac.HasTag("State.Alive"))
					return false; // still alive
			}

			return true;
		}

		private void SetUIText(Entity entity, string text)
		{
			if (entity == null)
				return;

			if (entity.HasComponent<UITextComponent>())
				entity.GetComponent<UITextComponent>().Text = text;
		}

		private static void WarnIfMissing(Entity entity, string name)
		{
			if (entity == null)
				Debug.LogWarning($"[GameManager] Entity '{name}' not found — overlay/UI ops will no-op");
		}
	}
}
