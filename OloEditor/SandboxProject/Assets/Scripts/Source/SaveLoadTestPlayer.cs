using System;
using System.IO;

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
			using var ms = new MemoryStream();
			using var bw = new BinaryWriter(ms);
			bw.Write(Health);
			bw.Write(Score);
			bw.Write(PlayTime);
			Console.WriteLine($"SaveLoadTestPlayer.OnSave - Health={Health}, Score={Score}, PlayTime={PlayTime:F1}");
			return ms.ToArray();
		}

		public void OnLoad(byte[] data)
		{
			if (data == null || data.Length < 12)
				return;

			using var ms = new MemoryStream(data);
			using var br = new BinaryReader(ms);
			Health = br.ReadSingle();
			Score = br.ReadInt32();
			PlayTime = br.ReadSingle();
			m_PKeyPreviouslyDown = false;
			Console.WriteLine($"SaveLoadTestPlayer.OnLoad - Health={Health}, Score={Score}, PlayTime={PlayTime:F1}");
		}
	}
}
