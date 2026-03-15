using System;

using OloEngine;

namespace Sandbox
{
	/// <summary>
	/// Test script demonstrating ISaveable implementation for save/load testing.
	/// Tracks player health and score, which persist across save/load cycles.
	/// </summary>
	public class SaveLoadTestPlayer : Entity, ISaveable
	{
		private TransformComponent m_Transform;
		private bool m_PKeyPreviouslyDown = false;

		public float Health = 100.0f;
		public int Score = 0;
		public float PlayTime = 0.0f;

		void OnCreate()
		{
			Console.WriteLine($"SaveLoadTestPlayer.OnCreate - {ID}");
			m_Transform = GetComponent<TransformComponent>();
		}

		void OnUpdate(float ts)
		{
			PlayTime += ts;

			// Simple movement for testing
			float speed = 5.0f;
			Vector3 velocity = Vector3.Zero;

			if (Input.IsKeyDown(KeyCode.W))
				velocity.Y = 1.0f;
			if (Input.IsKeyDown(KeyCode.S))
				velocity.Y = -1.0f;
			if (Input.IsKeyDown(KeyCode.A))
				velocity.X = -1.0f;
			if (Input.IsKeyDown(KeyCode.D))
				velocity.X = 1.0f;

			Vector3 translation = m_Transform.Translation;
			translation += velocity * speed * ts;
			m_Transform.Translation = translation;

			// Pressing P increases score (for testing save/load of custom state)
			bool pDown = Input.IsKeyDown(KeyCode.P);
			if (pDown && !m_PKeyPreviouslyDown)
				Score += 1;
			m_PKeyPreviouslyDown = pDown;
		}

		public byte[] OnSave()
		{
			// Simple serialization: health (4 bytes) + score (4 bytes) + playtime (4 bytes)
			byte[] data = new byte[12];
			Buffer.BlockCopy(BitConverter.GetBytes(Health), 0, data, 0, 4);
			Buffer.BlockCopy(BitConverter.GetBytes(Score), 0, data, 4, 4);
			Buffer.BlockCopy(BitConverter.GetBytes(PlayTime), 0, data, 8, 4);
			Console.WriteLine($"SaveLoadTestPlayer.OnSave - Health={Health}, Score={Score}, PlayTime={PlayTime:F1}");
			return data;
		}

		public void OnLoad(byte[] data)
		{
			if (data == null || data.Length < 12)
				return;

			Health = BitConverter.ToSingle(data, 0);
			Score = BitConverter.ToInt32(data, 4);
			PlayTime = BitConverter.ToSingle(data, 8);
			Console.WriteLine($"SaveLoadTestPlayer.OnLoad - Health={Health}, Score={Score}, PlayTime={PlayTime:F1}");
		}
	}
}
