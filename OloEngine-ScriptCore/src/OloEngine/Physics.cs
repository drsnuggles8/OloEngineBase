namespace OloEngine
{
	/// <summary>
	/// Result of a physics raycast query.
	/// </summary>
	public struct RaycastHit
	{
		public Vector3 Position;
		public Vector3 Normal;
		public float Distance;
		public ulong EntityID;
	}

	/// <summary>
	/// Provides physics query functions (raycasting, shape casts, etc.).
	/// </summary>
	public static class Physics
	{
		/// <summary>
		/// Cast a ray from origin in direction up to maxDistance.
		/// Returns true if something was hit; result is written to hit.
		/// </summary>
		public static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit)
		{
			hit = new RaycastHit();

			// Validate direction — near-zero vectors cause undefined behavior
			float lenSq = direction.X * direction.X + direction.Y * direction.Y + direction.Z * direction.Z;
			if (lenSq < 1e-12f)
				return false;

			if (maxDistance <= 0.0f)
				return false;

			// Normalize direction
			float invLen = 1.0f / (float)System.Math.Sqrt(lenSq);
			direction = new Vector3(direction.X * invLen, direction.Y * invLen, direction.Z * invLen);

			return InternalCalls.Physics_Raycast(
				ref origin, ref direction, maxDistance,
				out hit.Position, out hit.Normal, out hit.Distance, out hit.EntityID);
		}
	}
}
