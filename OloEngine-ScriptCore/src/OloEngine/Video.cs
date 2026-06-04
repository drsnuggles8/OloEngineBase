namespace OloEngine
{
	/// <summary>
	/// Fullscreen video playback (cutscenes, studio logos, splash screens). The path is
	/// resolved relative to the project's asset directory. Decode runs on a background
	/// thread; call from gameplay scripts to trigger and control a fullscreen overlay.
	/// </summary>
	public static class Video
	{
		/// <summary>Play a video fullscreen on top of the scene.</summary>
		public static void PlayFullscreen(string filePath, bool loop = false)
			=> InternalCalls.Video_PlayFullscreen(filePath, loop);

		/// <summary>Stop the active fullscreen video immediately.</summary>
		public static void Stop()
			=> InternalCalls.Video_Stop();

		/// <summary>Skip a skippable fullscreen video (stops it early).</summary>
		public static void Skip()
			=> InternalCalls.Video_Skip();

		/// <summary>True while a fullscreen video is playing.</summary>
		public static bool IsPlaying()
			=> InternalCalls.Video_IsPlaying();
	}
}
