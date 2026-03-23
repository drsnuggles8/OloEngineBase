namespace OloEngine
{
	public class Input
	{
		public static bool IsKeyDown(KeyCode keycode)
		{
			return InternalCalls.Input_IsKeyDown(keycode);
		}

		public static bool IsKeyJustPressed(KeyCode keycode)
		{
			return InternalCalls.Input_IsKeyJustPressed(keycode);
		}

		public static bool IsKeyJustReleased(KeyCode keycode)
		{
			return InternalCalls.Input_IsKeyJustReleased(keycode);
		}

		public static Vector2 GetMousePosition()
		{
			InternalCalls.Input_GetMousePosition(out Vector2 position);
			return position;
		}

		public static bool IsMouseButtonDown(int button)
		{
			return InternalCalls.Input_IsMouseButtonDown(button);
		}

        public static Vector2 GetWindowSize()
        {
            InternalCalls.Input_GetWindowSize(out Vector2 size);
            return size;
        }

		public static bool IsActionPressed(string actionName)
		{
			return InternalCalls.Input_IsActionPressed(actionName);
		}

		public static bool IsActionJustPressed(string actionName)
		{
			return InternalCalls.Input_IsActionJustPressed(actionName);
		}

		public static bool IsActionJustReleased(string actionName)
		{
			return InternalCalls.Input_IsActionJustReleased(actionName);
		}

		public static float GetActionAxisValue(string actionName)
		{
			return InternalCalls.Input_GetActionAxisValue(actionName);
		}

		// --- Gamepad ---

		public static bool IsGamepadButtonPressed(GamepadButton button, int gamepadIndex = 0)
		{
			return InternalCalls.Input_IsGamepadButtonPressed((byte)button, gamepadIndex);
		}

		public static bool IsGamepadButtonJustPressed(GamepadButton button, int gamepadIndex = 0)
		{
			return InternalCalls.Input_IsGamepadButtonJustPressed((byte)button, gamepadIndex);
		}

		public static bool IsGamepadButtonJustReleased(GamepadButton button, int gamepadIndex = 0)
		{
			return InternalCalls.Input_IsGamepadButtonJustReleased((byte)button, gamepadIndex);
		}

		public static float GetGamepadAxis(GamepadAxis axis, int gamepadIndex = 0)
		{
			return InternalCalls.Input_GetGamepadAxis((byte)axis, gamepadIndex);
		}

		public static Vector2 GetGamepadLeftStick(int gamepadIndex = 0)
		{
			InternalCalls.Input_GetGamepadLeftStick(gamepadIndex, out Vector2 stick);
			return stick;
		}

		public static Vector2 GetGamepadRightStick(int gamepadIndex = 0)
		{
			InternalCalls.Input_GetGamepadRightStick(gamepadIndex, out Vector2 stick);
			return stick;
		}

		public static bool IsGamepadConnected(int gamepadIndex = 0)
		{
			return InternalCalls.Input_IsGamepadConnected(gamepadIndex);
		}

		public static int GetGamepadConnectedCount()
		{
			return InternalCalls.Input_GetGamepadConnectedCount();
		}
	}
}
