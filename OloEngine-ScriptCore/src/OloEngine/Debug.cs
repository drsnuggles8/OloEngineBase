namespace OloEngine
{
	public static class Debug
	{
		public static void Log(string message)
		{
			InternalCalls.Log_LogMessage(1, message);
		}

		public static void LogWarning(string message)
		{
			InternalCalls.Log_LogMessage(2, message);
		}

		public static void LogError(string message)
		{
			InternalCalls.Log_LogMessage(3, message);
		}
	}
}
