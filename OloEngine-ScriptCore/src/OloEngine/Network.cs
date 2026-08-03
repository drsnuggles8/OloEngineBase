using System;
using System.Collections.Generic;

namespace OloEngine
{
	/// <summary>
	/// Where a registered RPC may run, and therefore who may invoke it.
	/// Must stay in sync with ERpcTarget in RpcTypes.h — the value is passed
	/// across the internal call as a raw int.
	/// </summary>
	public enum RpcTarget
	{
		/// <summary>Client -> server. The server runs it against the authoritative simulation.</summary>
		Server = 0,
		/// <summary>Server -> one client. A client that tries to invoke it is refused.</summary>
		Client = 1,
		/// <summary>Server -> every client, and locally on the server too.</summary>
		Multicast = 2
	}

	/// <summary>
	/// Who owns an entity's simulation. Must stay in sync with ENetworkAuthority in
	/// Components.h — the value crosses the internal call as a raw int.
	/// </summary>
	public enum NetworkAuthority
	{
		/// <summary>The server simulates it; clients interpolate it.</summary>
		Server = 0,
		/// <summary>The owning client predicts it; the server still validates every input.</summary>
		Client = 1,
		/// <summary>Both may write; the server remains the tiebreaker.</summary>
		Shared = 2
	}

	/// <summary>Delivery guarantee requested from the transport. Mirrors ERpcReliability.</summary>
	public enum RpcReliability
	{
		Reliable = 0,
		Unreliable = 1
	}

	/// <summary>Everything a handler needs to know about one invocation.</summary>
	public readonly struct RpcContext
	{
		/// <summary>The client that sent this call, or 0 when it came from the server.</summary>
		public uint SenderClientID { get; }
		/// <summary>The entity the call is bound to, or 0 for a global RPC.</summary>
		public ulong EntityID { get; }

		internal RpcContext(uint senderClientID, ulong entityID)
		{
			SenderClientID = senderClientID;
			EntityID = entityID;
		}
	}

	public delegate void RpcHandler(RpcContext context, object[] args);

	/// <summary>
	/// Script-facing surface for the server-authoritative multiplayer loop.
	///
	/// Registering an RPC stores the delegate here and tells native about the
	/// descriptor. Incoming calls come back through <see cref="DispatchRPC"/>, which
	/// native invokes by name — one bridge method rather than one per RPC, since a
	/// managed delegate cannot be held by the native registry.
	///
	/// Both ends of a connection must register the same name: the wire carries only
	/// its hash, so a name the receiver never registered is dropped rather than
	/// guessed at.
	/// </summary>
	public static class Network
	{
		private static readonly Dictionary<string, RpcHandler> s_Handlers = new Dictionary<string, RpcHandler>();

		public static bool IsServer => InternalCalls.Network_IsServer();
		public static bool IsClient => InternalCalls.Network_IsClient();
		public static bool IsConnected => InternalCalls.Network_IsConnected();

		/// <summary>The id the server assigned this client; 0 before it arrives, or on a pure server.</summary>
		public static uint LocalClientID => InternalCalls.Network_GetLocalClientID();

		/// <summary>The server's current replication tick.</summary>
		public static uint CurrentTick => InternalCalls.Network_GetCurrentTick();

		public static bool Connect(string address, ushort port)
			=> InternalCalls.Network_Connect(address, port);

		public static void Disconnect()
			=> InternalCalls.Network_Disconnect();

		public static bool StartServer(ushort port)
			=> InternalCalls.Network_StartServer(port);

		public static void StopServer()
			=> InternalCalls.Network_StopServer();

		/// <summary>Server-side: create a replicated entity. Returns its UUID, or 0 on failure.</summary>
		public static ulong Spawn(string archetype, string name, uint ownerClientID,
								  NetworkAuthority authority = NetworkAuthority.Server)
			=> InternalCalls.Network_Spawn(archetype, name, ownerClientID, (int)authority);

		/// <summary>Server-side: destroy a replicated entity and tell every client that knows it.</summary>
		public static void Despawn(ulong entityID)
			=> InternalCalls.Network_Despawn(entityID);

		/// <summary>
		/// Client-side: record an input, apply it locally (prediction) and send it to
		/// the server. The server applies the identical bytes through the same
		/// callback, so the predicted and authoritative results agree by construction.
		/// </summary>
		public static void SendInput(ulong entityID, byte[] data)
			=> InternalCalls.Network_SendInput(entityID, data);

		/// <summary>
		/// Register (or replace) an RPC. Re-registering a name rebinds its handler,
		/// so a script reload does not leak the previous delegate.
		/// </summary>
		public static void RegisterRPC(string name, RpcTarget target, RpcHandler handler,
									   RpcReliability reliability = RpcReliability.Reliable,
									   bool requiresOwnership = true)
		{
			if (string.IsNullOrEmpty(name))
				throw new ArgumentException("RPC name must not be empty", nameof(name));
			if (handler == null)
				throw new ArgumentNullException(nameof(handler));

			s_Handlers[name] = handler;
			InternalCalls.Network_RegisterRPC(name, (int)target, (int)reliability, requiresOwnership);
		}

		/// <summary>
		/// Invoke a registered RPC. Routing follows the descriptor's target, and an
		/// authority violation (a client invoking a Client/Multicast RPC) is refused
		/// rather than sent. Returns false when the call was refused.
		///
		/// Supported argument types: bool, int, long, ulong, float, double, string,
		/// Vector3. Anything else is refused rather than silently coerced.
		/// </summary>
		public static bool InvokeRPC(string name, ulong entityID = 0, object[] args = null, uint targetClientID = 0)
			=> InternalCalls.Network_InvokeRPC(name, entityID, args ?? Array.Empty<object>(), targetClientID);

		/// <summary>
		/// Called from native when an RPC arrives. Not part of the public API — it is
		/// resolved by name from ScriptGlue and must keep this exact signature.
		/// </summary>
		internal static void DispatchRPC(string name, uint senderClientID, ulong entityID, object[] args)
		{
			if (!s_Handlers.TryGetValue(name, out RpcHandler handler))
			{
				Debug.LogWarning($"[Network] No C# handler registered for RPC '{name}'");
				return;
			}

			handler(new RpcContext(senderClientID, entityID), args ?? Array.Empty<object>());
		}
	}
}
