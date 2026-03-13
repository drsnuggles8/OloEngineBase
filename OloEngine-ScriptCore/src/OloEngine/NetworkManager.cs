namespace OloEngine
{
	public static class NetworkManager
	{
		public static bool IsServer => InternalCalls.Network_IsServer();
		public static bool IsClient => InternalCalls.Network_IsClient();
		public static bool IsConnected => InternalCalls.Network_IsConnected();

		public static bool Connect(string address, ushort port)
			=> InternalCalls.Network_Connect(address, port);

		public static void Disconnect()
			=> InternalCalls.Network_Disconnect();

		public static bool StartServer(ushort port)
			=> InternalCalls.Network_StartServer(port);

		public static void StopServer()
			=> InternalCalls.Network_StopServer();
	}
}
