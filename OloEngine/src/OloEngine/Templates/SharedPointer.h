// SharedPointer.h - Smart pointer types for thread-safe reference counting
// Ported from UE5.7 Templates/SharedPointer.h

#pragma once

/**
 * @file SharedPointer.h
 * @brief Thread-safe reference-counted smart pointers
 * 
 * Provides TSharedPtr, TSharedRef, TWeakPtr, and TSharedFromThis that wrap
 * the standard library's shared_ptr, weak_ptr, and enable_shared_from_this.
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Assert.h"

#include <memory>
#include <utility>

namespace OloEngine
{
    // Forward declarations
    template <typename ObjectType>
    class TSharedPtr;

    template <typename ObjectType>
    class TSharedRef;

    template <typename ObjectType>
    class TWeakPtr;

    /**
     * @class TSharedFromThis
     * @brief Enables safe creation of TSharedPtr from 'this' pointer
     * 
     * Inherit from this class to enable calling AsShared() to get a TSharedPtr
     * to the current object from within member functions.
     */
    template <typename ObjectType>
    class TSharedFromThis : public std::enable_shared_from_this<ObjectType>
    {
    public:
        /**
         * @brief Get a shared pointer to this object
         * @return TSharedPtr to this object
         */
        TSharedPtr<ObjectType> AsShared()
        {
            return TSharedPtr<ObjectType>(std::enable_shared_from_this<ObjectType>::shared_from_this());
        }

        /**
         * @brief Get a shared pointer to this object (const version)
         * @return TSharedPtr to const this object
         */
        TSharedPtr<const ObjectType> AsShared() const
        {
            return TSharedPtr<const ObjectType>(std::enable_shared_from_this<ObjectType>::shared_from_this());
        }

        /**
         * @brief Try to get a weak pointer to this object
         * @return TWeakPtr to this object
         */
        TWeakPtr<ObjectType> AsWeak()
        {
            return TWeakPtr<ObjectType>(std::enable_shared_from_this<ObjectType>::weak_from_this());
        }

    protected:
        TSharedFromThis() = default;
        TSharedFromThis(const TSharedFromThis&) = default;
        TSharedFromThis& operator=(const TSharedFromThis&) = default;
        ~TSharedFromThis() = default;
    };

    /**
     * @class TSharedPtr
     * @brief Reference-counted smart pointer (nullable)
     */
    template <typename ObjectType>
    class TSharedPtr
    {
    public:
        using ElementType = ObjectType;

        TSharedPtr() = default;
        TSharedPtr(std::nullptr_t) : m_Ptr(nullptr) {}

        explicit TSharedPtr(ObjectType* InPtr) : m_Ptr(InPtr) {}

        // Construct from std::shared_ptr
        TSharedPtr(std::shared_ptr<ObjectType> InPtr) : m_Ptr(std::move(InPtr)) {}

        // Copy/move constructors
        TSharedPtr(const TSharedPtr& Other) = default;
        TSharedPtr(TSharedPtr&& Other) noexcept = default;
        TSharedPtr& operator=(const TSharedPtr& Other) = default;
        TSharedPtr& operator=(TSharedPtr&& Other) noexcept = default;

        // Converting constructors for derived types
        template <typename OtherType>
        TSharedPtr(const TSharedPtr<OtherType>& Other)
            : m_Ptr(Other.m_Ptr)
        {
        }

        template <typename OtherType>
        TSharedPtr(TSharedPtr<OtherType>&& Other)
            : m_Ptr(std::move(Other.m_Ptr))
        {
        }

        ~TSharedPtr() = default;

        /** Access the object */
        ObjectType* Get() const { return m_Ptr.get(); }
        ObjectType* operator->() const { return m_Ptr.get(); }
        ObjectType& operator*() const { return *m_Ptr; }

        /** Check validity */
        bool IsValid() const { return m_Ptr != nullptr; }
        explicit operator bool() const { return IsValid(); }

        /** Reset the pointer */
        void Reset() { m_Ptr.reset(); }

        /** Get the reference count */
        i64 GetSharedReferenceCount() const { return m_Ptr.use_count(); }

        /** Comparison operators */
        bool operator==(const TSharedPtr& Other) const { return m_Ptr == Other.m_Ptr; }
        bool operator!=(const TSharedPtr& Other) const { return m_Ptr != Other.m_Ptr; }
        bool operator==(std::nullptr_t) const { return m_Ptr == nullptr; }
        bool operator!=(std::nullptr_t) const { return m_Ptr != nullptr; }

        /** Get the underlying shared_ptr */
        const std::shared_ptr<ObjectType>& GetSharedPtr() const { return m_Ptr; }

    private:
        template <typename OtherType>
        friend class TSharedPtr;

        template <typename OtherType>
        friend class TSharedRef;

        template <typename OtherType>
        friend class TWeakPtr;

        template <typename OtherType, typename... ArgTypes>
        friend TSharedPtr<OtherType> MakeShared(ArgTypes&&... Args);

        std::shared_ptr<ObjectType> m_Ptr;
    };

    /**
     * @class TSharedRef
     * @brief Reference-counted smart pointer that is never null
     */
    template <typename ObjectType>
    class TSharedRef
    {
    public:
        using ElementType = ObjectType;

        // No default constructor - must always be valid

        explicit TSharedRef(ObjectType* InPtr) : m_Ptr(InPtr)
        {
            OLO_CORE_ASSERT(m_Ptr != nullptr, "TSharedRef cannot be null");
        }

        // Construct from std::shared_ptr
        TSharedRef(std::shared_ptr<ObjectType> InPtr) : m_Ptr(std::move(InPtr))
        {
            OLO_CORE_ASSERT(m_Ptr != nullptr, "TSharedRef cannot be null");
        }

        // Construct from TSharedPtr (asserts if null)
        explicit TSharedRef(const TSharedPtr<ObjectType>& InPtr)
            : m_Ptr(InPtr.m_Ptr)
        {
            OLO_CORE_ASSERT(m_Ptr != nullptr, "TSharedRef cannot be null");
        }

        // Copy/move constructors
        TSharedRef(const TSharedRef& Other) = default;
        TSharedRef(TSharedRef&& Other) noexcept = default;
        TSharedRef& operator=(const TSharedRef& Other) = default;
        TSharedRef& operator=(TSharedRef&& Other) noexcept = default;

        // Converting constructors for derived types
        template <typename OtherType>
        TSharedRef(const TSharedRef<OtherType>& Other)
            : m_Ptr(Other.m_Ptr)
        {
        }

        template <typename OtherType>
        TSharedRef(TSharedRef<OtherType>&& Other)
            : m_Ptr(std::move(Other.m_Ptr))
        {
        }

        ~TSharedRef() = default;

        /** Access the object */
        ObjectType* Get() const { return m_Ptr.get(); }
        ObjectType* operator->() const { return m_Ptr.get(); }
        ObjectType& operator*() const { return *m_Ptr; }

        /** Get the reference count */
        i64 GetSharedReferenceCount() const { return m_Ptr.use_count(); }

        /** Comparison operators */
        bool operator==(const TSharedRef& Other) const { return m_Ptr == Other.m_Ptr; }
        bool operator!=(const TSharedRef& Other) const { return m_Ptr != Other.m_Ptr; }

        /** Convert to TSharedPtr */
        TSharedPtr<ObjectType> ToSharedPtr() const { return TSharedPtr<ObjectType>(m_Ptr); }
        operator TSharedPtr<ObjectType>() const { return ToSharedPtr(); }

    private:
        template <typename OtherType>
        friend class TSharedRef;

        template <typename OtherType>
        friend class TSharedPtr;

        template <typename OtherType, typename... ArgTypes>
        friend TSharedRef<OtherType> MakeShared(ArgTypes&&... Args);

        std::shared_ptr<ObjectType> m_Ptr;
    };

    /**
     * @class TWeakPtr
     * @brief Weak reference to a shared object
     */
    template <typename ObjectType>
    class TWeakPtr
    {
    public:
        using ElementType = ObjectType;

        TWeakPtr() = default;
        TWeakPtr(std::nullptr_t) : m_Ptr() {}

        // Construct from std::weak_ptr
        TWeakPtr(std::weak_ptr<ObjectType> InPtr) : m_Ptr(std::move(InPtr)) {}

        // Construct from TSharedPtr
        TWeakPtr(const TSharedPtr<ObjectType>& InPtr) : m_Ptr(InPtr.m_Ptr) {}

        // Construct from TSharedRef
        TWeakPtr(const TSharedRef<ObjectType>& InPtr) : m_Ptr(InPtr.m_Ptr) {}

        // Copy/move constructors
        TWeakPtr(const TWeakPtr& Other) = default;
        TWeakPtr(TWeakPtr&& Other) noexcept = default;
        TWeakPtr& operator=(const TWeakPtr& Other) = default;
        TWeakPtr& operator=(TWeakPtr&& Other) noexcept = default;

        ~TWeakPtr() = default;

        /** Get a shared pointer if still valid */
        TSharedPtr<ObjectType> Pin() const
        {
            return TSharedPtr<ObjectType>(m_Ptr.lock());
        }

        /** Check if the referenced object still exists */
        bool IsValid() const { return !m_Ptr.expired(); }

        /** Reset the weak reference */
        void Reset() { m_Ptr.reset(); }

    private:
        std::weak_ptr<ObjectType> m_Ptr;
    };

    /**
     * @brief Create a shared pointer with in-place construction
     */
    template <typename ObjectType, typename... ArgTypes>
    TSharedPtr<ObjectType> MakeShared(ArgTypes&&... Args)
    {
        return TSharedPtr<ObjectType>(std::make_shared<ObjectType>(std::forward<ArgTypes>(Args)...));
    }

    /**
     * @brief Create a shared ref with in-place construction
     */
    template <typename ObjectType, typename... ArgTypes>
    TSharedRef<ObjectType> MakeShareable(ArgTypes&&... Args)
    {
        return TSharedRef<ObjectType>(std::make_shared<ObjectType>(std::forward<ArgTypes>(Args)...));
    }

} // namespace OloEngine
