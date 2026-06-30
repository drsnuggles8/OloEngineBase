namespace OloEngine
{
	// Named input contexts. Only one is active at a time; switching contexts swaps
	// which action map drives IsActionPressed/GetActionAxisValue. Mirrors the C++
	// OloEngine::InputContextType enum — keep the values in sync.
	public enum InputContext
	{
		Gameplay = 0,
		Menu = 1,
		Vehicle = 2,
		Custom = 3
	}

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

		// Hard switch of the active action-map context. A key held across the switch
		// does not register as "just pressed" in the newly-activated context.
		public static void SetInputContext(InputContext context)
		{
			InternalCalls.Input_SetInputContext((int)context);
		}

		public static InputContext GetInputContext()
		{
			return (InputContext)InternalCalls.Input_GetInputContext();
		}

		// Push a context onto the stack (e.g. a menu over gameplay), making it active.
		public static void PushInputContext(InputContext context)
		{
			InternalCalls.Input_PushInputContext((int)context);
		}

		// Pop the active context, restoring the one beneath. Returns false if only the
		// base context remains (it is never popped).
		public static bool PopInputContext()
		{
			return InternalCalls.Input_PopInputContext();
		}

		public static int GetInputContextDepth()
		{
			return InternalCalls.Input_GetInputContextDepth();
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
