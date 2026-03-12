namespace OloEngine
{
    /// <summary>
    /// Provides access to the engine's networking subsystem from C# scripts.
    /// </summary>
    public static class NetworkManager
    {
        /// <summary>Gets whether this instance is acting as a server.</summary>
        public static bool IsServer => InternalCalls.Network_IsServer();

        /// <summary>Gets whether this instance is acting as a client.</summary>
        public static bool IsClient => InternalCalls.Network_IsClient();

        /// <summary>Gets whether a connection is currently active.</summary>
        public static bool IsConnected => InternalCalls.Network_IsConnected();

        /// <summary>Start listening for incoming connections on the specified port.</summary>
        public static bool StartServer(ushort port)
            => InternalCalls.Network_StartServer(port);

        /// <summary>Stop the server and disconnect all clients.</summary>
        public static void StopServer()
            => InternalCalls.Network_StopServer();

        /// <summary>Connect to a remote server at the given address and port.</summary>
        public static bool Connect(string address, ushort port)
            => InternalCalls.Network_Connect(address, port);

        /// <summary>Disconnect from the server.</summary>
        public static void Disconnect()
            => InternalCalls.Network_Disconnect();
    }
}
