using OloEngine;

namespace Sandbox
{
	/// <summary>
	/// Shared damage-flash logic: detects health drops and flashes the material red.
	/// </summary>
	public class DamageFlashHelper
	{
		private MaterialComponent m_Material;
		private Vector4 m_OriginalColor;
		private float m_FlashTimer;
		private float m_LastHealth = -1.0f;
		private float m_Duration;

		public DamageFlashHelper(MaterialComponent material, float duration = 0.15f)
		{
			m_Material = material;
			m_OriginalColor = material != null ? material.AlbedoColor : new Vector4(1, 1, 1, 1);
			m_Duration = duration;
		}

		/// <summary>
		/// Call every frame. Detects health decrease and manages flash cooldown.
		/// </summary>
		public void Update(float ts, float currentHealth)
		{
			if (m_Material == null)
				return;

			if (m_LastHealth >= 0.0f && currentHealth < m_LastHealth)
			{
				m_Material.AlbedoColor = new Vector4(1.0f, 0.3f, 0.3f, 1.0f);
				m_FlashTimer = m_Duration;
			}
			m_LastHealth = currentHealth;

			if (m_FlashTimer > 0.0f)
			{
				m_FlashTimer -= ts;
				if (m_FlashTimer <= 0.0f)
					m_Material.AlbedoColor = m_OriginalColor;
			}
		}

		/// <summary>
		/// Trigger flash with a custom color (e.g. for ability activation).
		/// </summary>
		public void Flash(Vector4 color)
		{
			if (m_Material != null)
			{
				m_Material.AlbedoColor = color;
				m_FlashTimer = m_Duration;
			}
		}
	}
}
