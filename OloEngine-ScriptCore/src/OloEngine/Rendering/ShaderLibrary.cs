namespace OloEngine
{
	/// <summary>
	/// Provides access to the engine's shader library for loading, querying, and hot-reloading shaders.
	/// </summary>
	public static class ShaderLibrary
	{
		public static bool LoadShader(string filepath)
			=> InternalCalls.ShaderLibrary_LoadShader(filepath);

		public static bool Exists(string name)
			=> InternalCalls.ShaderLibrary_Exists(name);

		public static string GetShaderName(string name)
			=> InternalCalls.ShaderLibrary_GetShaderName(name);

		public static void ReloadAll()
			=> InternalCalls.ShaderLibrary_ReloadAll();

		public static void ReloadShader(string name)
			=> InternalCalls.ShaderLibrary_ReloadShader(name);

		public static uint ShaderCount
			=> InternalCalls.ShaderLibrary_GetShaderCount();
	}
}
