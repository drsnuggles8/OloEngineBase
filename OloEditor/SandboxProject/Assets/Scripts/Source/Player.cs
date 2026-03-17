using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

using OloEngine;

namespace Sandbox
{
	public class Player : Entity
	{
		private TransformComponent m_Transform;
		private Rigidbody2DComponent m_Rigidbody;

		public float Speed = 1.0f;
		public float Time = 0.0f;

		void OnCreate()
		{
			Console.WriteLine($"Player.OnCreate - {ID}");

			m_Transform = GetComponent<TransformComponent>();
			m_Rigidbody = GetComponent<Rigidbody2DComponent>();
		}

		void OnUpdate(float ts)
		{
			Time += ts;

			float speed = Speed;
			Vector3 velocity = Vector3.Zero;

			// Keyboard input
			if (Input.IsKeyDown(KeyCode.W))
				velocity.Y = 1.0f;
			else if (Input.IsKeyDown(KeyCode.S))
				velocity.Y = -1.0f;

			if (Input.IsKeyDown(KeyCode.A))
				velocity.X = -1.0f;
			else if (Input.IsKeyDown(KeyCode.D))
				velocity.X = 1.0f;

			// Gamepad input (left stick overrides keyboard if deflected)
			Vector2 stick = Input.GetGamepadLeftStick();
			if (stick.X * stick.X + stick.Y * stick.Y > 0.01f)
			{
				velocity.X = stick.X;
				velocity.Y = -stick.Y; // Y is inverted on gamepads
			}

			Entity cameraEntity = FindEntityByName("Camera");
			if (cameraEntity != null)
			{
				Camera camera = cameraEntity.As<Camera>();

				if (Input.IsKeyDown(KeyCode.Q) || Input.IsGamepadButtonPressed(GamepadButton.LeftBumper))
					camera.DistanceFromPlayer += speed * 2.0f * ts;
				else if (Input.IsKeyDown(KeyCode.E) || Input.IsGamepadButtonPressed(GamepadButton.RightBumper))
					camera.DistanceFromPlayer -= speed * 2.0f * ts;
			}

			velocity *= speed * ts;

			m_Rigidbody.ApplyLinearImpulse(velocity.XY, true);
		}

	}
}
