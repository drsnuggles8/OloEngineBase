namespace OloEngine
{
	/// <summary>
	/// Provides access to the engine's 3D shader library for loading, querying, and hot-reloading shaders.
	/// </summary>
	public static class ShaderLibrary3D
	{
		public static bool LoadShader(string filepath)
			=> InternalCalls.ShaderLibrary3D_LoadShader(filepath);

		public static bool Exists(string name)
			=> InternalCalls.ShaderLibrary3D_Exists(name);

		public static string GetShaderName(string name)
			=> InternalCalls.ShaderLibrary3D_GetShaderName(name);

		public static void ReloadAll()
			=> InternalCalls.ShaderLibrary3D_ReloadAll();

		public static void ReloadShader(string name)
			=> InternalCalls.ShaderLibrary3D_ReloadShader(name);

		public static uint ShaderCount
			=> InternalCalls.ShaderLibrary3D_GetShaderCount();
	}

	/// <summary>
	/// Provides access to the engine's 2D shader library for loading, querying, and hot-reloading shaders.
	/// </summary>
	public static class ShaderLibrary2D
	{
		public static bool LoadShader(string filepath)
			=> InternalCalls.ShaderLibrary2D_LoadShader(filepath);

		public static bool Exists(string name)
			=> InternalCalls.ShaderLibrary2D_Exists(name);

		public static string GetShaderName(string name)
			=> InternalCalls.ShaderLibrary2D_GetShaderName(name);

		public static void ReloadAll()
			=> InternalCalls.ShaderLibrary2D_ReloadAll();

		public static void ReloadShader(string name)
			=> InternalCalls.ShaderLibrary2D_ReloadShader(name);

		public static uint ShaderCount
			=> InternalCalls.ShaderLibrary2D_GetShaderCount();
	}

	/// <summary>
	/// Backward-compatible alias — forwards to <see cref="ShaderLibrary3D"/>.
	/// </summary>
	public static class ShaderLibrary
	{
		public static bool LoadShader(string filepath) => ShaderLibrary3D.LoadShader(filepath);
		public static bool Exists(string name) => ShaderLibrary3D.Exists(name);
		public static string GetShaderName(string name) => ShaderLibrary3D.GetShaderName(name);
		public static void ReloadAll() => ShaderLibrary3D.ReloadAll();
		public static void ReloadShader(string name) => ShaderLibrary3D.ReloadShader(name);
		public static uint ShaderCount => ShaderLibrary3D.ShaderCount;
	}
}
