namespace OloEngine
{
	public static class GameApplication
	{
		public static float TimeScale
		{
			get => InternalCalls.Application_GetTimeScale();
			set
			{
				float v = value;
				if (float.IsNaN(v) || float.IsInfinity(v))
					v = 1.0f;
				if (v < 0.0f)
					v = 0.0f;
				InternalCalls.Application_SetTimeScale(v);
			}
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
