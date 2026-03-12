namespace OloEngine
{
	public class Input
	{
		public static bool IsKeyDown(KeyCode keycode)
		{
			return InternalCalls.Input_IsKeyDown(keycode);
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
	}
}
