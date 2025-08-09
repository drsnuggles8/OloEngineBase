#pragma once

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
        void DecRefCount() const;
        u32 GetRefCount() const;

    private:
        mutable std::atomic<u32> m_RefCount = 0;
    };

    namespace RefUtils
	{
        void AddToLiveReferences(void* instance);
        void RemoveFromLiveReferences(void* instance);
        bool IsLive(void* instance);
    }

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
        operator bool() const { return m_Instance != nullptr; }

        T* operator->() { return m_Instance; }
        const T* operator->() const { return m_Instance; }

        T& operator*() { return *m_Instance; }
        const T& operator*() const { return *m_Instance; }

        T* Raw() { return m_Instance; }
        const T* Raw() const { return m_Instance; }
        
        // Standard smart pointer interface compatibility
        T* get() { return m_Instance; }
        const T* get() const { return m_Instance; }

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
            T2* cast_result = dynamic_cast<T2*>(m_Instance);
            if (cast_result)
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
            return Ref<T>(new(typeid(T).name()) T(std::forward<Args>(args)...));
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
                m_Instance->DecRefCount();
                
                if (m_Instance->GetRefCount() == 0)
                {
                    RefUtils::RemoveFromLiveReferences(m_Instance);
                    delete m_Instance;
                    // Note: m_Instance is not set to nullptr here to avoid race conditions.
                    // Each Ref instance manages its own pointer lifetime through 
                    // constructors, destructors, and assignment operators.
                }
            }
        }

        // Helper method to safely decrement and potentially delete an old instance
        void SafeDecRefAndDelete(T* oldInstance) const
        {
            if (oldInstance)
            {
                oldInstance->DecRefCount();
                if (oldInstance->GetRefCount() == 0)
                {
                    RefUtils::RemoveFromLiveReferences(oldInstance);
                    delete oldInstance;
                }
            }
        }

        template<class T2>
        friend class Ref;
        
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

        bool IsValid() const { return m_Instance ? RefUtils::IsLive(m_Instance) : false; }
        operator bool() const { return IsValid(); }

        Ref<T> Lock() const
        {
            if (IsValid())
                return Ref<T>(m_Instance);
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

}
