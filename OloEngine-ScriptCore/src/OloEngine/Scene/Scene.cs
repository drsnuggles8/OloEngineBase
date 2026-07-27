namespace OloEngine
{
	/// <summary>
	/// Runtime spawning entry points (issue #643).
	///
	/// <para><b>Spawns and destroys are deferred.</b> Each call queues the request
	/// and returns immediately; the engine applies the queue at a safe point
	/// inside the script phase of the same tick, once every script's
	/// <c>OnUpdate</c> has returned. This is not an optimisation — the engine
	/// walks the script component pools while dispatching <c>OnUpdate</c>, and
	/// creating or destroying an entity in the middle of that walk invalidates
	/// the iteration.</para>
	///
	/// <para>What that means for your script:</para>
	/// <list type="bullet">
	/// <item>The returned <see cref="Entity"/> carries the real, final ID — store
	/// it, compare it, hand it around. <see cref="Entity.IsValid"/> already
	/// reports <c>true</c> for it.</item>
	/// <item>You cannot read or write its components until the next tick,
	/// because it does not exist in the scene yet. Pass the spawn transform to
	/// the call instead of assigning <c>Translation</c> afterwards.</item>
	/// <item>By the time the tick's physics, transform propagation and rendering
	/// run, the entity is fully live — a spawn is visible in the same frame.</item>
	/// </list>
	/// </summary>
	public static class Scene
	{
		/// <summary>Spawn a bare entity (transform + tag) at the origin.</summary>
		/// <returns>The spawned entity, or null if there is no active scene.</returns>
		public static Entity CreateEntity(string name)
		{
			return CreateEntity(name, Vector3.Zero);
		}

		/// <summary>Spawn a bare entity (transform + tag) at <paramref name="position"/>.</summary>
		/// <returns>The spawned entity, or null if there is no active scene.</returns>
		public static Entity CreateEntity(string name, Vector3 position)
		{
			ulong id = InternalCalls.Scene_CreateEntity(name, ref position);
			return id == 0 ? null : new Entity(id);
		}

		/// <summary>Spawn a prefab at the origin, unrotated, unscaled.</summary>
		public static Entity Instantiate(ulong prefabHandle)
		{
			return Instantiate(prefabHandle, Vector3.Zero, Vector3.Zero, new Vector3(1.0f));
		}

		/// <summary>Spawn a prefab at <paramref name="position"/>.</summary>
		public static Entity Instantiate(ulong prefabHandle, Vector3 position)
		{
			return Instantiate(prefabHandle, position, Vector3.Zero, new Vector3(1.0f));
		}

		/// <summary>Spawn a prefab at <paramref name="position"/> with the given
		/// Euler <paramref name="rotation"/> (radians) and <paramref name="scale"/>.
		/// The transform replaces the prefab's authored one outright.</summary>
		public static Entity Instantiate(ulong prefabHandle, Vector3 position, Vector3 rotation, Vector3 scale)
		{
			ulong id = InternalCalls.Scene_InstantiatePrefab(prefabHandle, ref position, ref rotation, ref scale);
			return id == 0 ? null : new Entity(id);
		}

		/// <summary>
		/// Look up a prefab's asset handle by project-relative path, e.g.
		/// <c>"Prefabs/Projectile.oprefab"</c>.
		///
		/// <para><b>Editor only.</b> A packed runtime serves assets by handle and
		/// has no path index, so this returns 0 there. Ship-safe scripts should
		/// carry the handle in a serialized <c>ulong</c> field instead of
		/// resolving a path at runtime.</para>
		/// </summary>
		/// <returns>The asset handle, or 0 if unavailable.</returns>
		public static ulong FindPrefabByPath(string path)
		{
			return InternalCalls.Prefab_FindByPath(path);
		}

		/// <summary>
		/// Destroy an entity by raw id, for code that holds an id rather than an
		/// <see cref="Entity"/> — the mirror of Lua's <c>Scene.DestroyEntity</c>.
		/// Prefer <see cref="Entity.Destroy()"/> when you have the entity.
		///
		/// <para>Deferred and idempotent, exactly like the other spawn/destroy
		/// calls: the entity and its children go away once the engine drains its
		/// command queue this tick.</para>
		/// </summary>
		public static void DestroyEntity(ulong entityID)
		{
			InternalCalls.Entity_Destroy(entityID);
		}
	}
}
