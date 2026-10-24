using System;
using System.Runtime.CompilerServices;

namespace OloEngine
{
	public class Entity
	{
		protected Entity() { ID = 0; }

		internal Entity(ulong id)
		{
			ID = id;
		}

		public readonly ulong ID;

		public bool IsValid => ID != 0 && InternalCalls.Entity_IsValid(ID);
		public bool IsDestroyed => ID != 0 && !InternalCalls.Entity_IsValid(ID);

		public Vector3 Translation
		{
			get
			{
				InternalCalls.TransformComponent_GetTranslation(ID, out Vector3 result);
				return result;
			}
			set
			{
				InternalCalls.TransformComponent_SetTranslation(ID, ref value);
			}
		}

		public bool HasComponent<T>() where T : Component, new()
		{
			Type componentType = typeof(T);
			return InternalCalls.Entity_HasComponent(ID, componentType);
		}

		public T GetComponent<T>() where T : Component, new()
		{
			if (!HasComponent<T>())
				return null;

			T component = new T() { Entity = this };
			return component;
		}

		/// <summary>
		/// Carry this entity on <paramref name="parent"/> (issue #1559) -- on the bone
		/// named <paramref name="boneName"/> of the parent's skeleton, or on the parent
		/// itself when it is empty -- at the given local pose (rotation in radians).
		/// A dynamic body goes kinematic and follows; <see cref="Detach"/> lets it go
		/// at the speed it was carried at. Returns false, with a log line, when it did nothing.
		/// </summary>
		public bool AttachTo(Entity parent, string boneName, Vector3 localTranslation, Vector3 localRotation)
		{
			if (parent == null)
				return false;
			return InternalCalls.Entity_AttachTo(ID, parent.ID, boneName ?? string.Empty, ref localTranslation, ref localRotation) == 0;
		}

		/// <summary>
		/// <see cref="AttachTo"/>, keeping this entity where it is in the world: it rides
		/// <paramref name="parent"/> (or its bone) from the pose it has now.
		/// </summary>
		public bool AttachInPlace(Entity parent, string boneName)
		{
			if (parent == null)
				return false;
			return InternalCalls.Entity_AttachInPlace(ID, parent.ID, boneName ?? string.Empty) == 0;
		}

		/// <summary>Release this entity from whatever carries it, keeping its world pose and carried velocity.</summary>
		public bool Detach()
		{
			return InternalCalls.Entity_Detach(ID) == 0;
		}

		/// <summary>
		/// Where the entity is in the world: its composed position. <see cref="Translation"/>
		/// is relative to the parent, so an attached entity reads as its parent's origin.
		/// </summary>
		public Vector3 WorldTranslation
		{
			get
			{
				InternalCalls.Entity_GetWorldTranslation(ID, out Vector3 translation);
				return translation;
			}
		}

		public Entity FindEntityByName(string name)
		{
			ulong entityID = InternalCalls.Entity_FindEntityByName(name);
			if (entityID == 0)
				return null;

			return new Entity(entityID);
		}

		public T As<T>() where T : Entity, new()
		{
			object instance = InternalCalls.GetScriptInstance(ID);
			return instance as T;
		}

		/// <summary>
		/// Ask the engine to destroy this entity together with its children
		/// (issue #643).
		///
		/// <para><b>Deferred, like spawning.</b> The entity survives until the
		/// engine drains its command queue, which happens once every script's
		/// <c>OnUpdate</c> has returned this tick — so it is safe for a script
		/// to destroy the entity it is running on, and safe for two scripts to
		/// destroy the same target. <see cref="IsValid"/> reports <c>false</c>
		/// from the moment you call this, not from the moment it takes effect,
		/// so the rest of your <c>OnUpdate</c> sees a consistent answer.</para>
		///
		/// <para>The Lua/C# <c>OnDestroy</c> callback fires when the destroy is
		/// actually applied.</para>
		/// </summary>
		public void Destroy()
		{
			InternalCalls.Entity_Destroy(ID);
		}

		/// <summary>Destroy <paramref name="entity"/> (and its children). Null-safe.</summary>
		public static void Destroy(Entity entity)
		{
			if (entity != null)
				entity.Destroy();
		}

	}

}
