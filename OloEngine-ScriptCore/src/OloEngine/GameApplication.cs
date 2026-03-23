namespace OloEngine
{
	public static class GameApplication
	{
		public static float TimeScale
		{
			get => InternalCalls.Application_GetTimeScale();
			set => InternalCalls.Application_SetTimeScale(value);
		}

		public static void Quit()
		{
			InternalCalls.Application_QuitGame();
		}
	}

	public static class SceneManager
	{
		public static void ReloadCurrentScene()
		{
			InternalCalls.Scene_ReloadCurrentScene();
		}
	}
}
