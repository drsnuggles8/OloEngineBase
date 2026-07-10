#pragma once

#include "OloEngine/Core/Base.h" // u32 typedef used in this header

#include <atomic>
#include <type_traits>
#include <cstdint>

namespace OloEngine
{
    class RefCounted
    {
      public:
        RefCounted() = default;
        virtual ~RefCounted() = default;

        void IncRefCount() const;

        /// Atomically decrements the reference count and returns the count
        /// *after* the decrement. Callers MUST use this return value (not a
        /// separate GetRefCount() call) to decide whether they just released
        /// the last reference — reading the count back via a second, separate
        /// load is a TOCTOU race: two threads racing the last two DecRefCount()
        /// calls could each observe 0 and both delete the object (see #596).
        /// Only called from RefUtils::Release(), which additionally must call
        /// it *while holding* the live-reference registry's mutex — see that
        /// function's comment for why the decrement can't happen lock-free.
        u32 DecRefCount() const;
        u32 GetRefCount() const;

      private:
        mutable std::atomic<u32> m_RefCount = 0;
    };

    namespace RefUtils
    {
        void AddToLiveReferences(void* instance);
        bool IsLive(void* instance);

        /// Called by Ref<T>::DecRef()/SafeDecRefAndDelete() to release a reference.
        /// Decrements `instance`'s refcount *and* checks/updates the live-reference
        /// registry as one atomic operation (both happen under the same mutex
        /// TryLockLive() uses) — the decrement deliberately does NOT happen before
        /// taking that lock, because a concurrent TryLockLive() could otherwise
        /// resurrect `instance`, release it again, and delete it before this call
        /// ever acquires the lock, leaving `instance` dangling by the time it does.
        /// Returns true iff the caller now has exclusive responsibility to delete
        /// `instance`.
        bool Release(RefCounted* instance);

        /// Called by WeakRef<T>::Lock(). Atomically checks whether `instance` is still
        /// live and, if so, increments its refcount — all while holding the same mutex
        /// Release() uses, so this can never resurrect an instance that has already
        /// committed to destruction, and Release() can never delete an instance this
        /// just resurrected. Returns false if `instance` is dead (or concurrently being
        /// destroyed), in which case the caller must not touch it.
        bool TryLockLive(RefCounted* instance);
    } // namespace RefUtils

    template<typename T>
    class WeakRef;

    /// Thread-safe smart pointer for RefCounted objects.
    /// The underlying RefCounted object uses atomic reference counting,
    /// making it safe for multiple Ref instances to reference the same
    /// object from different threads. However, individual Ref instances
    /// are not thread-safe and should not be modified concurrently.
    template<typename T>
    class Ref
    {
      public:
        using element_type = T;

        // Constructors
        Ref() : m_Instance(nullptr) {}

        Ref(std::nullptr_t) : m_Instance(nullptr) {}

        explicit Ref(T* instance) : m_Instance(instance)
        {
            static_assert(std::is_base_of_v<RefCounted, T>, "Class is not RefCounted!");
            IncRef();
        }

        Ref(const Ref<T>& other) : m_Instance(other.m_Instance)
        {
            IncRef();
        }

        Ref(Ref<T>&& other) noexcept : m_Instance(other.m_Instance)
        {
            other.m_Instance = nullptr;
        }

        template<typename T2>
        Ref(const Ref<T2>& other) : m_Instance(static_cast<T*>(other.m_Instance))
        {
            static_assert(std::is_convertible_v<T2*, T*>, "T2* must be convertible to T*");
            IncRef();
        }

        template<typename T2>
        Ref(Ref<T2>&& other) noexcept : m_Instance(static_cast<T*>(other.m_Instance))
        {
            static_assert(std::is_convertible_v<T2*, T*>, "T2* must be convertible to T*");
            other.m_Instance = nullptr;
        }

        ~Ref()
        {
            DecRef();
        }

        // Assignment operators
        Ref& operator=(std::nullptr_t)
        {
            Reset();
            return *this;
        }

        Ref& operator=(const Ref<T>& other)
        {
            if (this != &other)
            {
                // Increment first to handle self-assignment edge cases
                T* oldInstance = m_Instance;
                m_Instance = other.m_Instance;
                IncRef();
                // Now safely decrement the old reference
                SafeDecRefAndDelete(oldInstance);
            }
            return *this;
        }

        Ref& operator=(Ref<T>&& other) noexcept
        {
            if (this != &other)
            {
                DecRef();
                m_Instance = other.m_Instance;
                other.m_Instance = nullptr;
            }
            return *this;
        }

        template<typename T2>
        Ref& operator=(const Ref<T2>& other)
        {
            static_assert(std::is_convertible_v<T2*, T*>, "T2* must be convertible to T*");
            T* oldInstance = m_Instance;
            m_Instance = static_cast<T*>(other.m_Instance);
            IncRef();
            SafeDecRefAndDelete(oldInstance);
            return *this;
        }

        template<typename T2>
        Ref& operator=(Ref<T2>&& other) noexcept
        {
            static_assert(std::is_convertible_v<T2*, T*>, "T2* must be convertible to T*");
            DecRef();
            m_Instance = static_cast<T*>(other.m_Instance);
            other.m_Instance = nullptr;
            return *this;
        }

        // Observers
        operator bool() const
        {
            return m_Instance != nullptr;
        }

        T* operator->()
        {
            return m_Instance;
        }
        const T* operator->() const
        {
            return m_Instance;
        }

        T& operator*()
        {
            return *m_Instance;
        }
        const T& operator*() const
        {
            return *m_Instance;
        }

        T* Raw()
        {
            return m_Instance;
        }
        const T* Raw() const
        {
            return m_Instance;
        }

        // Standard smart pointer interface compatibility
        T* get()
        {
            return m_Instance;
        }
        const T* get() const
        {
            return m_Instance;
        }

        // Modifiers
        void Reset(T* instance = nullptr)
        {
            DecRef();
            m_Instance = instance;
            IncRef();
        }

        // Casting
        template<typename T2>
        Ref<T2> As() const
        {
            static_assert(std::is_base_of_v<RefCounted, T2>, "T2 must inherit from RefCounted");
            if (T2* cast_result = dynamic_cast<T2*>(m_Instance); cast_result)
            {
                Ref<T2> result;
                result.m_Instance = cast_result;
                result.IncRef();
                return result;
            }
            return nullptr;
        }

        // Factory
        template<typename... Args>
        static Ref<T> Create(Args&&... args)
        {
#if OLO_TRACK_MEMORY && defined(OLO_PLATFORM_WINDOWS)
            return Ref<T>(new (typeid(T).name()) T(std::forward<Args>(args)...));
#else
            return Ref<T>(new T(std::forward<Args>(args)...));
#endif
        }

        // Comparison
        bool operator==(const Ref<T>& other) const
        {
            return m_Instance == other.m_Instance;
        }

        bool operator!=(const Ref<T>& other) const
        {
            return !(*this == other);
        }

        /// Compare the actual objects pointed to by the references.
        /// Requires that type T has operator== defined.
        /// Both references must be valid (non-null) for comparison to occur.
        /// @param other The other Ref to compare against
        /// @return true if both references are valid and their objects are equal, false otherwise
        bool EqualsObject(const Ref<T>& other) const
        {
            static_assert(std::is_same_v<decltype(std::declval<T>() == std::declval<T>()), bool>,
                          "Type T must support operator== that returns bool");

            if (!m_Instance || !other.m_Instance)
                return false;

            return *m_Instance == *other.m_Instance;
        }

      private:
        void IncRef() const
        {
            if (m_Instance)
            {
                m_Instance->IncRefCount();
                RefUtils::AddToLiveReferences(m_Instance);
            }
        }

        void DecRef() const
        {
            if (m_Instance)
            {
                // Clear this Ref's pointer up front — DecRef() is always followed
                // by either this Ref being destroyed or m_Instance being
                // reassigned, so no caller should ever be able to observe
                // m_Instance pointing at an object that's mid-release or already
                // deleted (the SonarQube-flagged dangling-pointer pattern).
                T* instance = m_Instance;
                m_Instance = nullptr;

                if (RefUtils::Release(instance))
                {
                    delete instance;
                }
            }
        }

        // Helper method to safely decrement and potentially delete an old instance
        void SafeDecRefAndDelete(T* oldInstance) const
        {
            if (oldInstance)
            {
                if (RefUtils::Release(oldInstance))
                {
                    delete oldInstance;
                }
            }
        }

        // Tag used by WeakRef<T>::Lock() to adopt a pointer whose refcount and
        // live-registration were already performed atomically by
        // RefUtils::TryLockLive() — avoids double-incrementing.
        struct AdoptLockedTag
        {
        };
        Ref(T* instance, AdoptLockedTag) : m_Instance(instance) {}

        template<class T2>
        friend class Ref;

        friend class WeakRef<T>;

        mutable T* m_Instance;
    };

    template<typename T>
    class WeakRef
    {
      public:
        WeakRef() = default;

        WeakRef(const Ref<T>& ref) : m_Instance(const_cast<T*>(ref.get())) {}

        explicit WeakRef(T* instance) : m_Instance(instance) {}

        /// Access the object via pointer.
        /// WARNING: This does not check if the object is still alive!
        /// Call IsValid() first or use Lock() for safe access.
        T* operator->()
        {
            OLO_CORE_ASSERT(IsValid(), "WeakRef: Accessing invalid/deleted object! Call IsValid() first.");
            return m_Instance;
        }

        /// Access the object via pointer (const version).
        /// WARNING: This does not check if the object is still alive!
        /// Call IsValid() first or use Lock() for safe access.
        const T* operator->() const
        {
            OLO_CORE_ASSERT(IsValid(), "WeakRef: Accessing invalid/deleted object! Call IsValid() first.");
            return m_Instance;
        }

        /// Dereference the object.
        /// WARNING: This does not check if the object is still alive!
        /// Call IsValid() first or use Lock() for safe access.
        T& operator*()
        {
            OLO_CORE_ASSERT(IsValid(), "WeakRef: Dereferencing invalid/deleted object! Call IsValid() first.");
            return *m_Instance;
        }

        /// Dereference the object (const version).
        /// WARNING: This does not check if the object is still alive!
        /// Call IsValid() first or use Lock() for safe access.
        const T& operator*() const
        {
            OLO_CORE_ASSERT(IsValid(), "WeakRef: Dereferencing invalid/deleted object! Call IsValid() first.");
            return *m_Instance;
        }

        bool IsValid() const
        {
            return m_Instance ? RefUtils::IsLive(m_Instance) : false;
        }
        operator bool() const
        {
            return IsValid();
        }

        /// Attempts to resurrect a strong Ref to the referenced object. Unlike a plain
        /// `IsValid()` check followed by constructing a Ref (which races a concurrent
        /// last-DecRef release — see RefUtils::TryLockLive), this atomically checks
        /// liveness and increments the refcount as one step, so it either returns a
        /// genuinely valid Ref or nullptr — never a Ref to a partially/fully destroyed
        /// object.
        Ref<T> Lock() const
        {
            if (m_Instance && RefUtils::TryLockLive(m_Instance))
                return Ref<T>(m_Instance, typename Ref<T>::AdoptLockedTag{});
            return nullptr;
        }

        template<typename T2>
        WeakRef<T2> As() const
        {
            return WeakRef<T2>(dynamic_cast<T2*>(m_Instance));
        }

      private:
        T* m_Instance = nullptr;
    };

    // Convenience aliases for backward compatibility
    template<typename T>
    using AssetRef = Ref<T>;

    template<typename T>
    using WeakAssetRef = WeakRef<T>;

    // Additional aliases
    template<typename T>
    using AssetPtr = Ref<T>;

    template<typename T>
    using WeakAssetPtr = WeakRef<T>;

} // namespace OloEngine
