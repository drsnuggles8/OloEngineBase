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
        uint32_t GetRefCount() const;

    private:
        mutable std::atomic<uint32_t> m_RefCount = 0;
    };

    namespace RefUtils
	{
        void AddToLiveReferences(void* instance);
        void RemoveFromLiveReferences(void* instance);
        bool IsLive(void* instance);
    }

    template<typename T>
    class AssetRef
    {
    public:
        using element_type = T;

        // Constructors
        AssetRef() : m_Instance(nullptr) {}
        
        AssetRef(std::nullptr_t) : m_Instance(nullptr) {}

        explicit AssetRef(T* instance) : m_Instance(instance)
        {
            static_assert(std::is_base_of_v<RefCounted, T>, "Class is not RefCounted!");
            IncRef();
        }

        AssetRef(const AssetRef<T>& other) : m_Instance(other.m_Instance)
        {
            IncRef();
        }

        AssetRef(AssetRef<T>&& other) noexcept : m_Instance(other.m_Instance)
        {
            other.m_Instance = nullptr;
        }

        template<typename T2>
        AssetRef(const AssetRef<T2>& other) : m_Instance(static_cast<T*>(other.m_Instance))
        {
            static_assert(std::is_convertible_v<T2*, T*>, "T2* must be convertible to T*");
            IncRef();
        }

        template<typename T2>
        AssetRef(AssetRef<T2>&& other) noexcept : m_Instance(static_cast<T*>(other.m_Instance))
        {
            static_assert(std::is_convertible_v<T2*, T*>, "T2* must be convertible to T*");
            other.m_Instance = nullptr;
        }

        ~AssetRef()
        {
            DecRef();
        }

        // Assignment operators
        AssetRef& operator=(std::nullptr_t)
        {
            Reset();
            return *this;
        }

        AssetRef& operator=(const AssetRef<T>& other)
        {
            if (this != &other)
            {
                // Increment first to handle self-assignment edge cases
                T* oldInstance = m_Instance;
                m_Instance = other.m_Instance;
                IncRef();
                // Now decrement the old reference
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
            return *this;
        }

        AssetRef& operator=(AssetRef<T>&& other) noexcept
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
        AssetRef& operator=(const AssetRef<T2>& other)
        {
            static_assert(std::is_convertible_v<T2*, T*>, "T2* must be convertible to T*");
            T* oldInstance = m_Instance;
            m_Instance = static_cast<T*>(other.m_Instance);
            IncRef();
            if (oldInstance)
            {
                oldInstance->DecRefCount();
                if (oldInstance->GetRefCount() == 0)
                {
                    RefUtils::RemoveFromLiveReferences(oldInstance);
                    delete oldInstance;
                }
            }
            return *this;
        }

        template<typename T2>
        AssetRef& operator=(AssetRef<T2>&& other) noexcept
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

        // Modifiers
        void Reset(T* instance = nullptr)
        {
            DecRef();
            m_Instance = instance;
            IncRef();
        }

        // Casting
        template<typename T2>
        AssetRef<T2> As() const
        {
            return AssetRef<T2>(*this);
        }

        // Factory
        template<typename... Args>
        static AssetRef<T> Create(Args&&... args)
        {
#if OLO_TRACK_MEMORY && defined(OLO_PLATFORM_WINDOWS)
            return AssetRef<T>(new(typeid(T).name()) T(std::forward<Args>(args)...));
#else
            return AssetRef<T>(new T(std::forward<Args>(args)...));
#endif
        }

        // Comparison
        bool operator==(const AssetRef<T>& other) const
        {
            return m_Instance == other.m_Instance;
        }

        bool operator!=(const AssetRef<T>& other) const
        {
            return !(*this == other);
        }

        bool EqualsObject(const AssetRef<T>& other)
        {
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
                    m_Instance = nullptr;
                }
            }
        }

        template<class T2>
        friend class AssetRef;
        
        mutable T* m_Instance;
    };
    
    template<typename T>
    class WeakAssetRef
    {
    public:
        WeakAssetRef() = default;

        WeakAssetRef(const AssetRef<T>& ref) : m_Instance(ref.Raw()) {}

        explicit WeakAssetRef(T* instance) : m_Instance(instance) {}

        T* operator->() { return m_Instance; }
        const T* operator->() const { return m_Instance; }

        T& operator*() { return *m_Instance; }
        const T& operator*() const { return *m_Instance; }

        bool IsValid() const { return m_Instance ? RefUtils::IsLive(m_Instance) : false; }
        operator bool() const { return IsValid(); }

        AssetRef<T> Lock() const
        {
            if (IsValid())
                return AssetRef<T>(m_Instance);
            return nullptr;
        }

        template<typename T2>
        WeakAssetRef<T2> As() const
        {
            return WeakAssetRef<T2>(dynamic_cast<T2*>(m_Instance));
        }

    private:
        T* m_Instance = nullptr;
    };

    // Convenience aliases for when you eventually want to transition
    template<typename T>
    using AssetPtr = AssetRef<T>;
    
    template<typename T>
    using WeakAssetPtr = WeakAssetRef<T>;

}
