using System;
using OloEngine;

namespace Sandbox
{
	public class GameManager : Entity
	{
		// UI overlay entities (found by name)
		private Entity m_PauseOverlay;
		private Entity m_DeathOverlay;
		private Entity m_HUD;

		// Player reference
		private Entity m_Player;

		// State
		private bool m_IsPaused = false;
		private bool m_IsPlayerDead = false;

		// Colors
		private static readonly Vector4 Hidden = new Vector4(0, 0, 0, 0);
		private static readonly Vector4 PausePanelColor = new Vector4(0.0f, 0.0f, 0.0f, 0.7f);
		private static readonly Vector4 PauseTextColor = new Vector4(1.0f, 1.0f, 0.5f, 1.0f);
		private static readonly Vector4 DeathPanelColor = new Vector4(0.15f, 0.0f, 0.0f, 0.8f);
		private static readonly Vector4 DeathTextColor = new Vector4(1.0f, 0.2f, 0.2f, 1.0f);

		void OnCreate()
		{
			m_PauseOverlay = FindEntityByName("Pause Overlay");
			m_DeathOverlay = FindEntityByName("Death Overlay");
			m_HUD = FindEntityByName("HUD");
			m_Player = FindEntityByName("Player");

			// Hide overlays at start
			SetOverlayVisible(m_PauseOverlay, false);
			SetOverlayVisible(m_DeathOverlay, false);

			Debug.Log("[GameManager] Initialized - Esc=Pause, WASD/Click=Move, 1/2/3=Abilities");
		}

		void OnUpdate(float ts)
		{
			// ── Death screen (checked first — takes priority over pause) ──
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
		}

		/// <summary>
		/// Called by PlayerController when the player dies.
		/// </summary>
		public void OnPlayerDeath()
		{
			if (m_IsPlayerDead)
				return;

			m_IsPlayerDead = true;
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

		private void SetUIText(Entity entity, string text)
		{
			if (entity == null)
				return;

			if (entity.HasComponent<UITextComponent>())
				entity.GetComponent<UITextComponent>().Text = text;
		}
	}
}
