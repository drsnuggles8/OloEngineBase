using System;

using OloEngine;

namespace Sandbox
{
	public class GamepadPlayer : Entity
	{
		private TransformComponent m_Transform;

		public float MoveSpeed = 8.0f;

		void OnCreate()
		{
			Console.WriteLine($"GamepadPlayer.OnCreate - {ID}");
			m_Transform = GetComponent<TransformComponent>();
		}

		void OnUpdate(float ts)
		{
			Vector3 translation = m_Transform.Translation;
			// --- Sprint (right trigger scales speed) ---
			float speed = MoveSpeed;
			float triggerValue = Input.GetGamepadAxis(GamepadAxis.RightTrigger);
			if (triggerValue > 0.1f)
			{
				speed *= 1.0f + triggerValue * 1.5f;
			}

			// --- Movement via InputActions (works with KB + gamepad) ---
			if (Input.IsActionPressed("MoveUp"))
				translation.Z -= speed * ts;
			if (Input.IsActionPressed("MoveDown"))
				translation.Z += speed * ts;
			if (Input.IsActionPressed("MoveLeft"))
				translation.X -= speed * ts;
			if (Input.IsActionPressed("MoveRight"))
				translation.X += speed * ts;

			// --- Analog stick movement (smooth, analog speed) ---
			Vector2 stick = Input.GetGamepadLeftStick();
			if (stick.X * stick.X + stick.Y * stick.Y > 0.01f)
			{
				translation.X += stick.X * speed * ts;
				translation.Z += stick.Y * speed * ts;
			}

			// --- Jump (simple vertical impulse) ---
			if (Input.IsActionJustPressed("Jump"))
			{
				translation.Y += 2.0f;
				Console.WriteLine("Jump!");
			}

			// Gravity (simple)
			if (translation.Y > 0.5f)
				translation.Y -= 9.8f * ts;
			if (translation.Y < 0.5f)
				translation.Y = 0.5f;

			m_Transform.Translation = translation;
		}
	}
}
