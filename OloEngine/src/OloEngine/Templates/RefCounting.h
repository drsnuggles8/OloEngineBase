/**
 * @file RefCounting.h
 * @brief Thread-safe reference counting utilities
 * 
 * Provides base classes and smart pointers for reference-counted objects:
 * - FReturnedRefCountValue: Wrapper for refcount values (deprecated access)
 * - TTransactionalAtomicRefCount: Core atomic refcount with transactional support
 * - IRefCountedObject: Virtual interface for ref-counted objects
 * - FRefCountBase: Base class with virtual destructor
 * - FRefCountedObject: Non-atomic ref-counted base (legacy)
 * - FThreadSafeRefCountedObject: Atomic ref-counted base
 * - TRefCountingMixin: CRTP mixin for adding ref-counting
 * - TRefCountPtr<T>: Smart pointer for ref-counted objects
 * 
 * Ported from Unreal Engine 5.7's Templates/RefCounting.h
 */

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include <atomic>
#include <type_traits>

namespace OloEngine
{
	// Forward declaration
	namespace Private
	{
		void CheckRefCountIsNonZero();
	}

	// ========================================================================
	// FReturnedRefCountValue
	// ========================================================================

	/**
	 * Simple wrapper class which holds a refcount; emits a deprecation warning when accessed.
	 * 
	 * It is unsafe to rely on the value of a refcount for any logic, and a non-deprecated
	 * getter function should never be added. In a multi-threaded context, the refcount could
	 * change after inspection.
	 */
	struct FReturnedRefCountValue
	{
		explicit FReturnedRefCountValue(u32 InRefCount)
			: RefCount(InRefCount)
		{
		}

		FReturnedRefCountValue(const FReturnedRefCountValue& Other) = default;
		FReturnedRefCountValue(FReturnedRefCountValue&& Other) = default;
		FReturnedRefCountValue& operator=(const FReturnedRefCountValue& Other) = default;
		FReturnedRefCountValue& operator=(FReturnedRefCountValue&& Other) = default;

		/**
		 * Implicit conversion to u32 - deprecated but provided for compatibility
		 * @note Inspecting an object's refcount is deprecated.
		 */
		operator u32() const
		{
			return RefCount;
		}

		/**
		 * Check that refcount is at least a certain value.
		 * @note It's harmless to check if your refcount is at least a certain amount.
		 */
		void CheckAtLeast(u32 N) const
		{
			OLO_CORE_ASSERT(RefCount >= N, "RefCount check failed");
		}

	private:
		u32 RefCount = 0;
	};

	// ========================================================================
	// TTransactionalAtomicRefCount
	// ========================================================================

	namespace Private
	{
		/**
		 * TTransactionalAtomicRefCount manages an atomic refcount value.
		 * This is used by FRefCountBase, FThreadSafeRefCountedObject and TRefCountingMixin (in thread-safe mode).
		 * 
		 * @note AutoRTFM/transactional memory is not supported in OloEngine, so this is a 
		 *       simplified version without transaction support.
		 */
		template <typename AtomicType>
		class TTransactionalAtomicRefCount
		{
		public:
			template <auto DeleteFn>
			AtomicType AddRef() const
			{
				AtomicType Refs = RefCount.fetch_add(1, std::memory_order_relaxed);
				return Refs + 1;
			}

			template <auto DeleteFn>
			AtomicType Release() const
			{
#ifdef OLO_ENABLE_ASSERTS
				if (RefCount.load(std::memory_order_relaxed) == 0)
				{
					CheckRefCountIsNonZero();
				}
#endif
				return ImmediatelyRelease<DeleteFn>();
			}

			AtomicType GetRefCount() const
			{
				// A 'live' reference count is unstable by nature and so there's no benefit
				// to try and enforce memory ordering around the reading of it.
				return RefCount.load(std::memory_order_relaxed);
			}

		private:
			template <auto DeleteFn>
			AtomicType ImmediatelyRelease() const
			{
				// fetch_sub returns the refcount _before_ it was decremented. std::memory_order_acq_rel is
				// used so that, if we do end up executing the destructor, it's not possible for side effects 
				// from executing the destructor to end up being visible before we've determined that the 
				// reference count is actually zero.
				AtomicType RefsBeforeRelease = RefCount.fetch_sub(1, std::memory_order_acq_rel);

#ifdef OLO_ENABLE_ASSERTS
				// A check-failure is issued if an object is over-released.
				if (RefsBeforeRelease == 0)
				{
					CheckRefCountIsNonZero();
				}
#endif
				// We immediately free the object if its refcount has become zero.
				if (RefsBeforeRelease == 1)
				{
					DeleteFn(this);
				}
				return RefsBeforeRelease;
			}

			mutable std::atomic<AtomicType> RefCount{ 0 };
		};
	} // namespace Private

	// ========================================================================
	// IRefCountedObject
	// ========================================================================

	/**
	 * A virtual interface for ref counted objects to implement.
	 */
	class IRefCountedObject
	{
	public:
		virtual ~IRefCountedObject() = default;
		virtual FReturnedRefCountValue AddRef() const = 0;
		virtual u32 Release() const = 0;
		virtual u32 GetRefCount() const = 0;
	};

	// ========================================================================
	// FRefCountBase
	// ========================================================================

	/**
	 * Base class implementing thread-safe reference counting.
	 */
	class FRefCountBase : Private::TTransactionalAtomicRefCount<u32>
	{
	public:
		FRefCountBase() = default;
		virtual ~FRefCountBase() = default;

		FRefCountBase(const FRefCountBase& Rhs) = delete;
		FRefCountBase& operator=(const FRefCountBase& Rhs) = delete;

		FReturnedRefCountValue AddRef() const
		{
			return FReturnedRefCountValue{ Super::AddRef<DeleteThis>() };
		}

		u32 Release() const
		{
			return Super::Release<DeleteThis>();
		}

		u32 GetRefCount() const
		{
			return Super::GetRefCount();
		}

	private:
		using Super = Private::TTransactionalAtomicRefCount<u32>;

		static void DeleteThis(const Super* This)
		{
			delete static_cast<const FRefCountBase*>(This);
		}
	};

	// ========================================================================
	// FRefCountedObject
	// ========================================================================

	/**
	 * The base class of reference counted objects.
	 *
	 * This class should not be used for new code as it does not use atomic operations to update 
	 * the reference count.
	 */
	class FRefCountedObject
	{
	public:
		FRefCountedObject() : NumRefs(0) {}
		virtual ~FRefCountedObject() { OLO_CORE_ASSERT(!NumRefs, "Object destroyed with non-zero ref count"); }

		FRefCountedObject(const FRefCountedObject& Rhs) = delete;
		FRefCountedObject& operator=(const FRefCountedObject& Rhs) = delete;

		FReturnedRefCountValue AddRef() const
		{
			return FReturnedRefCountValue{ u32(++NumRefs) };
		}

		u32 Release() const
		{
			u32 Refs = u32(--NumRefs);
			if (Refs == 0)
			{
				delete this;
			}
			return Refs;
		}

		u32 GetRefCount() const
		{
			return u32(NumRefs);
		}

	private:
		mutable i32 NumRefs;
	};

	// ========================================================================
	// FThreadSafeRefCountedObject
	// ========================================================================

	/**
	 * Like FRefCountedObject, but the reference count is thread-safe.
	 */
	class FThreadSafeRefCountedObject : Private::TTransactionalAtomicRefCount<u32>
	{
	public:
		FThreadSafeRefCountedObject() = default;

		FThreadSafeRefCountedObject(const FThreadSafeRefCountedObject& Rhs) = delete;
		FThreadSafeRefCountedObject& operator=(const FThreadSafeRefCountedObject& Rhs) = delete;

		virtual ~FThreadSafeRefCountedObject()
		{
			OLO_CORE_ASSERT(Super::GetRefCount() == 0, "Object destroyed with non-zero ref count");
		}

		FReturnedRefCountValue AddRef() const
		{
			return FReturnedRefCountValue{ Super::AddRef<DeleteThis>() };
		}

		u32 Release() const
		{
			return Super::Release<DeleteThis>();
		}

		u32 GetRefCount() const
		{
			return Super::GetRefCount();
		}

	private:
		using Super = Private::TTransactionalAtomicRefCount<u32>;

		static void DeleteThis(const Super* This)
		{
			delete static_cast<const FThreadSafeRefCountedObject*>(This);
		}
	};

	// ========================================================================
	// ERefCountingMode
	// ========================================================================

	/**
	 * ERefCountingMode is used select between either 'fast' or 'thread safe' ref-counting types.
	 * This is only used at compile time to select between template specializations.
	 */
	enum class ERefCountingMode : u8
	{
		/** Forced to be not thread-safe. */
		NotThreadSafe = 0,

		/** Thread-safe: never spin locks, but slower. */
		ThreadSafe = 1
	};

	// ========================================================================
	// TRefCountingMixin
	// ========================================================================

	/**
	 * Ref-counting mixin, designed to add ref-counting to an object without requiring a virtual destructor.
	 * Is thread-safe by default, and can support custom deleters via T::StaticDestroyObject.
	 * 
	 * Basic Example:
	 *  struct FMyRefCountedObject : public TRefCountingMixin<FMyRefCountedObject>
	 *  {
	 *      // ...
	 *  };
	 * 
	 * Deleter Example:
	 *  struct FMyRefCountedPooledObject : public TRefCountingMixin<FMyRefCountedPooledObject>
	 *  {
	 *      static void StaticDestroyObject(const FMyRefCountedPooledObject* Obj)
	 *      {
	 *          GPool->ReturnToPool(Obj);
	 *      }
	 *  };
	 */
	template <typename T, ERefCountingMode Mode = ERefCountingMode::ThreadSafe>
	class TRefCountingMixin;

	/**
	 * Thread-safe specialization
	 */
	template <typename T>
	class TRefCountingMixin<T, ERefCountingMode::ThreadSafe> : Private::TTransactionalAtomicRefCount<u32>
	{
	public:
		TRefCountingMixin() = default;

		TRefCountingMixin(const TRefCountingMixin&) = delete;
		TRefCountingMixin& operator=(const TRefCountingMixin&) = delete;

		FReturnedRefCountValue AddRef() const
		{
			return FReturnedRefCountValue{ Super::template AddRef<StaticDestroyMixin>() };
		}

		u32 Release() const
		{
			return Super::template Release<StaticDestroyMixin>();
		}

		u32 GetRefCount() const
		{
			return Super::GetRefCount();
		}

		static void StaticDestroyObject(const T* Obj)
		{
			delete Obj;
		}

	private:
		using Super = Private::TTransactionalAtomicRefCount<u32>;

		static void StaticDestroyMixin(const Super* This)
		{
			// This static_cast is traversing two levels of the class hierarchy.
			// We are casting from our parent class (TTransactionalAtomicRefCount*) to our subclass (T*).
			T::StaticDestroyObject(static_cast<const T*>(This));
		}
	};

	/**
	 * Not-thread-safe specialization
	 */
	template <typename T>
	class TRefCountingMixin<T, ERefCountingMode::NotThreadSafe>
	{
	public:
		TRefCountingMixin() = default;

		TRefCountingMixin(const TRefCountingMixin&) = delete;
		TRefCountingMixin& operator=(const TRefCountingMixin&) = delete;

		FReturnedRefCountValue AddRef() const
		{
			return FReturnedRefCountValue{ ++RefCount };
		}

		u32 Release() const
		{
			OLO_CORE_ASSERT(RefCount > 0, "Release called on zero ref count");

			if (--RefCount == 0)
			{
				StaticDestroyMixin(this);
			}

			// Note: TRefCountPtr doesn't use the return value
			return 0;
		}

		u32 GetRefCount() const
		{
			return RefCount;
		}

		static void StaticDestroyObject(const T* Obj)
		{
			delete Obj;
		}

	private:
		static void StaticDestroyMixin(const TRefCountingMixin* This)
		{
			T::StaticDestroyObject(static_cast<const T*>(This));
		}

		mutable u32 RefCount{ 0 };
	};

	// ========================================================================
	// TRefCountPtr
	// ========================================================================

	/**
	 * A smart pointer to an object which implements AddRef/Release.
	 */
	template<typename ReferencedType>
	class TRefCountPtr
	{
		using ReferenceType = ReferencedType*;

	public:
		OLO_FINLINE TRefCountPtr() = default;

		TRefCountPtr(ReferencedType* InReference, bool bAddRef = true)
		{
			Reference = InReference;
			if (Reference && bAddRef)
			{
				Reference->AddRef();
			}
		}

		TRefCountPtr(const TRefCountPtr& Copy)
		{
			Reference = Copy.Reference;
			if (Reference)
			{
				Reference->AddRef();
			}
		}

		template<typename CopyReferencedType>
		explicit TRefCountPtr(const TRefCountPtr<CopyReferencedType>& Copy)
		{
			Reference = static_cast<ReferencedType*>(Copy.GetReference());
			if (Reference)
			{
				Reference->AddRef();
			}
		}

		TRefCountPtr(TRefCountPtr&& Move)
		{
			Reference = Move.Reference;
			Move.Reference = nullptr;
		}

		template<typename MoveReferencedType>
		explicit TRefCountPtr(TRefCountPtr<MoveReferencedType>&& Move)
		{
			Reference = static_cast<ReferencedType*>(Move.GetReference());
			Move.Reference = nullptr;
		}

		~TRefCountPtr()
		{
			if (Reference)
			{
				Reference->Release();
			}
		}

		TRefCountPtr& operator=(ReferencedType* InReference)
		{
			if (Reference != InReference)
			{
				// Call AddRef before Release, in case the new reference is the same as the old reference.
				ReferencedType* OldReference = Reference;
				Reference = InReference;
				if (Reference)
				{
					Reference->AddRef();
				}
				if (OldReference)
				{
					OldReference->Release();
				}
			}
			return *this;
		}

		OLO_FINLINE TRefCountPtr& operator=(const TRefCountPtr& InPtr)
		{
			return *this = InPtr.Reference;
		}

		template<typename CopyReferencedType>
		OLO_FINLINE TRefCountPtr& operator=(const TRefCountPtr<CopyReferencedType>& InPtr)
		{
			return *this = InPtr.GetReference();
		}

		TRefCountPtr& operator=(TRefCountPtr&& InPtr)
		{
			if (this != &InPtr)
			{
				ReferencedType* OldReference = Reference;
				Reference = InPtr.Reference;
				InPtr.Reference = nullptr;
				if (OldReference)
				{
					OldReference->Release();
				}
			}
			return *this;
		}

		template<typename MoveReferencedType>
		TRefCountPtr& operator=(TRefCountPtr<MoveReferencedType>&& InPtr)
		{
			// InPtr is a different type (or we would have called the other operator), so we need not test &InPtr != this
			ReferencedType* OldReference = Reference;
			Reference = InPtr.Reference;
			InPtr.Reference = nullptr;
			if (OldReference)
			{
				OldReference->Release();
			}
			return *this;
		}

		OLO_FINLINE ReferencedType* operator->() const
		{
			return Reference;
		}

		OLO_FINLINE operator ReferenceType() const
		{
			return Reference;
		}

		ReferencedType** GetInitReference()
		{
			*this = nullptr;
			return &Reference;
		}

		OLO_FINLINE ReferencedType* GetReference() const
		{
			return Reference;
		}

		OLO_FINLINE friend bool IsValidRef(const TRefCountPtr& InReference)
		{
			return InReference.Reference != nullptr;
		}

		OLO_FINLINE bool IsValid() const
		{
			return Reference != nullptr;
		}

		OLO_FINLINE void SafeRelease()
		{
			*this = nullptr;
		}

		u32 GetRefCount()
		{
			u32 Result = 0;
			if (Reference)
			{
				Result = Reference->GetRefCount();
				OLO_CORE_ASSERT(Result > 0, "Zero ref count on live pointer");
			}
			return Result;
		}

		void Swap(TRefCountPtr& InPtr) // this does not change the reference count, and so is faster
		{
			ReferencedType* OldReference = Reference;
			Reference = InPtr.Reference;
			InPtr.Reference = OldReference;
		}

	private:
		ReferencedType* Reference = nullptr;

		template <typename OtherType>
		friend class TRefCountPtr;

	public:
		OLO_FINLINE bool operator==(const TRefCountPtr& B) const
		{
			return GetReference() == B.GetReference();
		}

		OLO_FINLINE bool operator==(ReferencedType* B) const
		{
			return GetReference() == B;
		}

		OLO_FINLINE bool operator!=(const TRefCountPtr& B) const
		{
			return GetReference() != B.GetReference();
		}

		OLO_FINLINE bool operator!=(ReferencedType* B) const
		{
			return GetReference() != B;
		}
	};

	// ========================================================================
	// Non-member operators and functions
	// ========================================================================

	template<typename ReferencedType>
	OLO_FINLINE bool operator==(ReferencedType* A, const TRefCountPtr<ReferencedType>& B)
	{
		return A == B.GetReference();
	}

	template<typename ReferencedType>
	OLO_FINLINE bool operator!=(ReferencedType* A, const TRefCountPtr<ReferencedType>& B)
	{
		return A != B.GetReference();
	}

	template<typename ReferencedType>
	OLO_FINLINE u32 GetTypeHash(const TRefCountPtr<ReferencedType>& InPtr)
	{
		return GetTypeHash(InPtr.GetReference());
	}

	/**
	 * Creates a new ref-counted object and returns it wrapped in a TRefCountPtr.
	 * 
	 * @tparam T The type to create
	 * @tparam TArgs Constructor argument types
	 * @param Args Constructor arguments
	 * @return TRefCountPtr<T> owning the new object
	 */
	template <
		typename T,
		typename... TArgs,
		typename = std::enable_if_t<!std::is_array_v<T>>
	>
	[[nodiscard]] TRefCountPtr<T> MakeRefCount(TArgs&&... Args)
	{
		T* NewObject = new T(Forward<TArgs>(Args)...);
		return TRefCountPtr<T>(NewObject);
	}

} // namespace OloEngine
