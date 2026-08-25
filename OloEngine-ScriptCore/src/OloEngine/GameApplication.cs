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

		/// <summary>
		/// Switch to another scene — the main-menu -> level -> next-level primitive.
		/// </summary>
		/// <param name="path">
		/// The scene to load. A bare name ("Level2"), a file name ("Level2.olo") or a
		/// path relative to the game's scene directory ("Scenes/Level2.olo") all work.
		/// </param>
		/// <remarks>
		/// DEFERRED, like <see cref="ReloadCurrentScene"/>: the request is recorded and
		/// applied after the current tick finishes, because the scene being torn down is
		/// the one your script is running in. Everything after this call still runs on the
		/// old scene, and any script state that isn't serialized into the new scene is gone
		/// once the swap happens. Calling it more than once in a tick keeps the last request.
		/// The switch is a hard cut — there is no fade or loading screen.
		/// </remarks>
		public static void LoadScene(string path)
		{
			if (string.IsNullOrWhiteSpace(path))
				return;

			InternalCalls.Scene_LoadScene(path);
		}

		/// <summary>
		/// Switch to an authored scene and restore a save into it before runtime startup.
		/// </summary>
		public static void LoadSceneFromSave(string path, string saveSlot)
		{
			if (string.IsNullOrWhiteSpace(path) || string.IsNullOrWhiteSpace(saveSlot))
				return;

			InternalCalls.Scene_LoadSceneFromSave(path, saveSlot);
		}
	}
}
