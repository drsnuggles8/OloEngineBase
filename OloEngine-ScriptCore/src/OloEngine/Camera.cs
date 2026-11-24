namespace OloEngine
{
	/// <summary>
	/// Camera utility functions for screen-to-world conversions.
	/// </summary>
	public static class Camera
	{
		/// <summary>
		/// Convert normalised screen coordinates [0,1] to a world-space ray.
		/// screenPos: (0,0) = bottom-left, (1,1) = top-right.
		/// </summary>
		public static bool ScreenToWorldRay(ulong cameraEntityID, Vector2 screenPos, out Vector3 origin, out Vector3 direction)
		{
			return InternalCalls.Camera_ScreenToWorldRay(cameraEntityID, ref screenPos, out origin, out direction);
		}

		/// <summary>Cast through the primary camera's last rendered frame, including its
		/// interpolated pose. Uses bottom-left normalized coordinates and returns the origin in the
		/// current scene frame. Returns false before a frame renders.</summary>
		public static bool RenderedScreenToWorldRay(Vector2 screenPos, out Vector3 origin, out Vector3 direction)
		{
			return InternalCalls.Camera_RenderedScreenToWorldRay(ref screenPos, out origin, out direction);
		}
	}
}
