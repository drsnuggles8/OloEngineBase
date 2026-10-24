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
	/// What a pick volume ray hit (issue #1559): the character, the bone whose
	/// fitted capsule the ray entered first, that bone's region (its own name when
	/// it is in none), and where.
	/// </summary>
	public struct PickVolumeHit
	{
		public ulong EntityID;
		public string Region;
		public string Bone;
		public Vector3 Position;
		public Vector3 Normal;
		public float Distance;
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

		/// <summary>
		/// Every entity <paramref name="entity"/> touches now, or began touching since
		/// the scripts last ran (issue #1559): a contact that starts and ends inside one
		/// frame's physics steps is still reported once. Empty when there are none.
		/// </summary>
		/// <summary>
		/// The first pick volume (PickVolumesComponent) a ray enters: a capsule per bone
		/// of a skinned character, carried by its current pose. No physics body involved.
		/// </summary>
		public static bool RaycastPickVolumes(Vector3 origin, Vector3 direction, float maxDistance, out PickVolumeHit hit)
		{
			hit = new PickVolumeHit();
			if (maxDistance <= 0.0f)
				return false;
			return InternalCalls.Physics_RaycastPickVolumes(ref origin, ref direction, maxDistance,
				out hit.Position, out hit.Normal, out hit.Distance, out hit.EntityID, out hit.Region, out hit.Bone);
		}

		public static ulong[] GetContacts(Entity entity)
		{
			if (entity == null)
				return System.Array.Empty<ulong>();
			return InternalCalls.Physics_GetContacts(entity.ID) ?? System.Array.Empty<ulong>();
		}
	}
}
