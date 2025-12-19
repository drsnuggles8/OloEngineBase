// @file Function.h
// @brief Function wrappers with inline storage optimization
// 
// Provides callable wrappers with configurable inline storage to avoid
// heap allocation for small functors:
// - TFunctionRef<T> : Non-owning reference to a callable (zero-copy, zero-alloc)
// - TFunction<T> : Owning copy of a callable with inline storage
// - TUniqueFunction<T> : Move-only owning callable with inline storage
// 
// Ported from Unreal Engine 5.7's Templates/Function.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Memory/UnrealMemory.h"
#include "OloEngine/Templates/Invoke.h"
#include "OloEngine/Templates/UnrealTemplate.h"
#include "OloEngine/Misc/IntrusiveUnsetOptionalState.h"
// Note: Do NOT include FunctionRef.h - this file provides complete implementations
// of TFunctionRef, TFunction, and TUniqueFunction with inline storage support

#include <type_traits>
#include <utility>
#include <new>
#include <cstring>

// UE macro for nullptr type
#ifndef TYPE_OF_NULLPTR
#define TYPE_OF_NULLPTR std::nullptr_t
#endif

namespace OloEngine
{
	// ========================================================================
	// Forward Declarations
	// ========================================================================

	template <typename FuncType>
	class TFunctionRef;

	template <typename FuncType>
	class TFunction;

	template <typename FuncType>
	class TUniqueFunction;

	// ========================================================================
	// Type Traits
	// ========================================================================

	template <typename T>
	struct TIsTFunctionRef : std::false_type {};

	template <typename T>
	struct TIsTFunctionRef<TFunctionRef<T>> : std::true_type {};

	template <typename T>
	struct TIsTFunctionRef<const T> : TIsTFunctionRef<T> {};

	template <typename T>
	struct TIsTFunctionRef<volatile T> : TIsTFunctionRef<T> {};

	template <typename T>
	struct TIsTFunctionRef<const volatile T> : TIsTFunctionRef<T> {};

	template <typename T>
	inline constexpr bool TIsTFunctionRef_v = TIsTFunctionRef<T>::value;

	template <typename T>
	struct TIsTFunction : std::false_type {};

	template <typename T>
	struct TIsTFunction<TFunction<T>> : std::true_type {};

	template <typename T>
	struct TIsTFunction<const T> : TIsTFunction<T> {};

	template <typename T>
	struct TIsTFunction<volatile T> : TIsTFunction<T> {};

	template <typename T>
	struct TIsTFunction<const volatile T> : TIsTFunction<T> {};

	template <typename T>
	inline constexpr bool TIsTFunction_v = TIsTFunction<T>::value;

	template <typename T>
	struct TIsTUniqueFunction : std::false_type {};

	template <typename T>
	struct TIsTUniqueFunction<TUniqueFunction<T>> : std::true_type {};

	template <typename T>
	struct TIsTUniqueFunction<const T> : TIsTUniqueFunction<T> {};

	template <typename T>
	struct TIsTUniqueFunction<volatile T> : TIsTUniqueFunction<T> {};

	template <typename T>
	struct TIsTUniqueFunction<const volatile T> : TIsTUniqueFunction<T> {};

	template <typename T>
	inline constexpr bool TIsTUniqueFunction_v = TIsTUniqueFunction<T>::value;

	// ========================================================================
	// Configuration
	// ========================================================================

	// Define NUM_TFUNCTION_INLINE_BYTES to enable inline storage.
	// Common values: 32, 48, 64 bytes (enough for most lambdas with 1-3 captures)
#if !defined(NUM_TFUNCTION_INLINE_BYTES) || NUM_TFUNCTION_INLINE_BYTES == 0
	#define TFUNCTION_USES_INLINE_STORAGE 0
#else
	#define TFUNCTION_USES_INLINE_STORAGE 1
	#define TFUNCTION_INLINE_SIZE         NUM_TFUNCTION_INLINE_BYTES
	#define TFUNCTION_INLINE_ALIGNMENT    16
#endif

	// ========================================================================
	// Private Implementation
	// ========================================================================

	// ========================================================================
	// Private Implementation
	// ========================================================================

	namespace Private::Function
	{
		// Forward declarations
		template <typename T, bool bUnique, bool bOnHeap>
		struct TFunction_OwnedObject;

		struct FFunctionStorage;

		template <bool bUnique>
		struct TFunctionStorage;

		// Interface for owned callable objects
		struct IFunction_OwnedObject
		{
			virtual void* CloneToEmptyStorage(FFunctionStorage* Storage) const = 0;
			virtual void* GetAddress() = 0;
			virtual void Destroy() = 0;
			virtual ~IFunction_OwnedObject() = default;
		};

		// Concrete owned object wrapper
		template <typename T, bool bUnique, bool bOnHeap>
		struct TFunction_OwnedObject : public IFunction_OwnedObject
		{
			T Obj;

			template <typename ArgType>
			explicit TFunction_OwnedObject(ArgType&& Arg)
				: Obj(Forward<ArgType>(Arg))
			{
			}

			void* GetAddress() override
			{
				return &Obj;
			}

			void* CloneToEmptyStorage(FFunctionStorage* UntypedStorage) const override;

			void Destroy() override
			{
				if constexpr (bOnHeap)
				{
					void* This = this;
					this->~TFunction_OwnedObject();
					FMemory::Free(This);
				}
				else
				{
					this->~TFunction_OwnedObject();
				}
			}

			~TFunction_OwnedObject() override = default;
		};

		// @brief Check if a callable is bound (not null)
		template <typename T>
		OLO_FINLINE bool IsBound(const T& Func)
		{
			if constexpr (std::is_pointer_v<T> || std::is_member_pointer_v<T>)
			{
				return !!Func;
			}
			else if constexpr (TIsTFunction_v<T> || TIsTUniqueFunction_v<T>)
			{
				return !!Func;
			}
			else
			{
				return true;  // Assume lambdas/functors are always bound
			}
		}

		// @brief Function storage - manages heap or inline allocation
		struct FFunctionStorage
		{
			static constexpr bool bCanBeNull = true;

			FFunctionStorage()
				: HeapAllocation(nullptr)
			{
			}

			FFunctionStorage(FFunctionStorage&& Other)
				: HeapAllocation(Other.HeapAllocation)
			{
				Other.HeapAllocation = nullptr;
#if TFUNCTION_USES_INLINE_STORAGE
				std::memcpy(&InlineAllocation, &Other.InlineAllocation, sizeof(InlineAllocation));
#endif
			}

			FFunctionStorage(const FFunctionStorage& Other) = delete;
			FFunctionStorage& operator=(FFunctionStorage&& Other) = delete;
			FFunctionStorage& operator=(const FFunctionStorage& Other) = delete;

			void BindCopy(const FFunctionStorage& Other)
			{
				Other.GetBoundObject()->CloneToEmptyStorage(this);
			}

			IFunction_OwnedObject* GetBoundObject() const
			{
				IFunction_OwnedObject* Result = static_cast<IFunction_OwnedObject*>(HeapAllocation);
#if TFUNCTION_USES_INLINE_STORAGE
				if (!Result)
				{
					Result = reinterpret_cast<IFunction_OwnedObject*>(const_cast<u8*>(InlineAllocation));
				}
#endif
				return Result;
			}

			void* GetPtr() const
			{
#if TFUNCTION_USES_INLINE_STORAGE
				IFunction_OwnedObject* Owned = static_cast<IFunction_OwnedObject*>(HeapAllocation);
				if (!Owned)
				{
					Owned = reinterpret_cast<IFunction_OwnedObject*>(const_cast<u8*>(InlineAllocation));
				}
				return Owned->GetAddress();
#else
				return static_cast<IFunction_OwnedObject*>(HeapAllocation)->GetAddress();
#endif
			}

			void Unbind()
			{
				IFunction_OwnedObject* Owned = GetBoundObject();
				Owned->Destroy();
			}

#if TFUNCTION_USES_INLINE_STORAGE
			alignas(TFUNCTION_INLINE_ALIGNMENT) u8 InlineAllocation[TFUNCTION_INLINE_SIZE];
#endif
			void* HeapAllocation;
		};

		// @brief Typed function storage with bind support
		template <bool bUnique>
		struct TFunctionStorage : FFunctionStorage
		{
			TFunctionStorage() = default;

			TFunctionStorage(FFunctionStorage&& Other)
				: FFunctionStorage(MoveTemp(Other))
			{
			}

			template <typename FunctorType>
			std::decay_t<FunctorType>* Bind(FunctorType&& InFunc)
			{
				using DecayedFunctorType = std::decay_t<FunctorType>;

				if (!IsBound(InFunc))
				{
					return nullptr;
				}

#if TFUNCTION_USES_INLINE_STORAGE
				constexpr bool bOnHeap = sizeof(TFunction_OwnedObject<DecayedFunctorType, bUnique, false>) > TFUNCTION_INLINE_SIZE;
#else
				constexpr bool bOnHeap = true;
#endif

				using OwnedType = TFunction_OwnedObject<DecayedFunctorType, bUnique, bOnHeap>;

				void* NewAlloc;
#if TFUNCTION_USES_INLINE_STORAGE
				if constexpr (!bOnHeap)
				{
					NewAlloc = &InlineAllocation;
				}
				else
#endif
				{
					NewAlloc = FMemory::Malloc(sizeof(OwnedType), alignof(OwnedType));
					HeapAllocation = NewAlloc;
				}

				OwnedType* NewOwned = ::new (NewAlloc) OwnedType(Forward<FunctorType>(InFunc));
				return &NewOwned->Obj;
			}
		};

		// Implement CloneToEmptyStorage
		template <typename T, bool bUnique, bool bOnHeap>
		void* TFunction_OwnedObject<T, bUnique, bOnHeap>::CloneToEmptyStorage(FFunctionStorage* UntypedStorage) const
		{
			if constexpr (bUnique)
			{
				// TUniqueFunction cannot be copied
				OLO_CORE_ASSERT(false, "Cannot clone TUniqueFunction");
				return nullptr;
			}
			else
			{
				TFunctionStorage<false>& Storage = *static_cast<TFunctionStorage<false>*>(UntypedStorage);

				void* NewAlloc;
#if TFUNCTION_USES_INLINE_STORAGE
				if constexpr (!bOnHeap)
				{
					NewAlloc = &Storage.InlineAllocation;
				}
				else
#endif
				{
					NewAlloc = FMemory::Malloc(sizeof(TFunction_OwnedObject), alignof(TFunction_OwnedObject));
					Storage.HeapAllocation = NewAlloc;
				}

				auto* NewOwned = ::new (NewAlloc) TFunction_OwnedObject(this->Obj);
				return &NewOwned->Obj;
			}
		}

		// @brief Generic caller for invoking the stored callable
		template <typename Functor, typename Ret, typename... ParamTypes>
		struct TFunctionRefCaller
		{
			static Ret Call(void* Obj, ParamTypes&... Params)
			{
				if constexpr (std::is_void_v<Ret>)
				{
					OloEngine::Invoke(*static_cast<Functor*>(Obj), Forward<ParamTypes>(Params)...);
				}
				else
				{
					return OloEngine::Invoke(*static_cast<Functor*>(Obj), Forward<ParamTypes>(Params)...);
				}
			}
		};

		// @brief Base class for all function types
		template <typename StorageType, typename FuncType>
		struct TFunctionRefBase;

		template <typename StorageType, typename Ret, typename... ParamTypes>
		struct TFunctionRefBase<StorageType, Ret(ParamTypes...)>
		{
			template <typename OtherStorageType, typename OtherFuncType>
			friend struct TFunctionRefBase;

			TFunctionRefBase() = default;

			TFunctionRefBase(TFunctionRefBase&& Other)
				: Callable(Other.Callable)
				, Storage(MoveTemp(Other.Storage))
			{
				static_assert(StorageType::bCanBeNull, "Unable to move non-nullable storage");
				if (Callable)
				{
					Other.Callable = nullptr;
				}
			}

			template <typename OtherStorage>
			TFunctionRefBase(TFunctionRefBase<OtherStorage, Ret(ParamTypes...)>&& Other)
				: Callable(Other.Callable)
				, Storage(MoveTemp(Other.Storage))
			{
				static_assert(OtherStorage::bCanBeNull, "Unable to move from non-nullable storage");
				static_assert(StorageType::bCanBeNull, "Unable to move into non-nullable storage");

				if (Callable)
				{
					Other.Callable = nullptr;
				}
			}

			template <typename OtherStorage>
			TFunctionRefBase(const TFunctionRefBase<OtherStorage, Ret(ParamTypes...)>& Other)
				: Callable(Other.Callable)
			{
				if constexpr (OtherStorage::bCanBeNull)
				{
					static_assert(StorageType::bCanBeNull, "Unable to copy from nullable storage into non-nullable storage");
					if (!Callable)
					{
						return;
					}
				}
				Storage.BindCopy(Other.Storage);
			}

			TFunctionRefBase(const TFunctionRefBase& Other)
				: Callable(Other.Callable)
			{
				if constexpr (StorageType::bCanBeNull)
				{
					if (!Callable)
					{
						return;
					}
				}
				Storage.BindCopy(Other.Storage);
			}

			template <typename FunctorType,
					  typename = std::enable_if_t<
						  !TIsTFunctionRef_v<std::decay_t<FunctorType>> &&
						  !TIsTFunction_v<std::decay_t<FunctorType>> &&
						  !TIsTUniqueFunction_v<std::decay_t<FunctorType>>
					  >>
			TFunctionRefBase(FunctorType&& InFunc)
			{
				auto* Binding = Storage.Bind(Forward<FunctorType>(InFunc));

				if constexpr (StorageType::bCanBeNull)
				{
					if (!Binding)
					{
						return;
					}
				}

				using DecayedFunctorType = std::remove_pointer_t<decltype(Binding)>;
				Callable = &TFunctionRefCaller<DecayedFunctorType, Ret, ParamTypes...>::Call;
			}

			TFunctionRefBase& operator=(TFunctionRefBase&&) = delete;
			TFunctionRefBase& operator=(const TFunctionRefBase&) = delete;

			Ret operator()(ParamTypes... Params) const
			{
				OLO_CORE_ASSERT(Callable, "Attempting to call an unbound function!");
				return Callable(Storage.GetPtr(), Params...);
			}

			~TFunctionRefBase()
			{
				if constexpr (StorageType::bCanBeNull)
				{
					if (!Callable)
					{
						return;
					}
				}
				Storage.Unbind();
			}

			void Reset()
			{
				if (Callable)
				{
					Storage.Unbind();
					Callable = nullptr;
				}
			}

		protected:
			bool IsSet() const
			{
				return !!Callable;
			}

		private:
			Ret(*Callable)(void*, ParamTypes&...) = nullptr;
			StorageType Storage;
		};

		// @brief Storage policy for TFunctionRef (non-owning)
		struct FFunctionRefStoragePolicy
		{
			static constexpr bool bCanBeNull = false;

			template <typename FunctorType>
			std::remove_reference_t<FunctorType>* Bind(FunctorType&& InFunc)
			{
				OLO_CORE_ASSERT(IsBound(InFunc), "Cannot bind a null/unbound callable to a TFunctionRef");
				Ptr = const_cast<void*>(static_cast<const void*>(&InFunc));
				return &InFunc;
			}

			void BindCopy(const FFunctionRefStoragePolicy& Other)
			{
				Ptr = Other.Ptr;
			}

			void* GetPtr() const
			{
				return Ptr;
			}

			void Unbind() const
			{
				// TFunctionRef doesn't own its binding - do nothing
			}

		private:
			void* Ptr = nullptr;
		};

	} // namespace Private::Function

	// ========================================================================
	// TFunctionRef - Non-owning reference
	// ========================================================================

	// @class TFunctionRef
	// @brief A non-owning reference to a callable object
	//
	// TFunctionRef is lightweight and meant to be passed by value.
	// The callable must outlive the TFunctionRef.
	template <typename Ret, typename... ParamTypes>
	class TFunctionRef<Ret(ParamTypes...)> final
		: public Private::Function::TFunctionRefBase<Private::Function::FFunctionRefStoragePolicy, Ret(ParamTypes...)>
	{
		using Super = Private::Function::TFunctionRefBase<Private::Function::FFunctionRefStoragePolicy, Ret(ParamTypes...)>;

	public:
		template <typename FunctorType,
				  typename = std::enable_if_t<
					  !TIsTFunctionRef_v<std::decay_t<FunctorType>> &&
					  std::is_invocable_r_v<Ret, std::decay_t<FunctorType>, ParamTypes...>
				  >>
		TFunctionRef(FunctorType&& InFunc)
			: Super(Forward<FunctorType>(InFunc))
		{
		}

		// Intrusive TOptional support
		static constexpr bool bHasIntrusiveUnsetOptionalState = true;
		using IntrusiveUnsetOptionalStateType = TFunctionRef;

		explicit TFunctionRef(FIntrusiveUnsetOptionalState) {}
		bool operator==(FIntrusiveUnsetOptionalState) const { return !Super::IsSet(); }

		TFunctionRef(const TFunctionRef&) = default;
		TFunctionRef& operator=(const TFunctionRef&) = delete;
		~TFunctionRef() = default;
	};

	// ========================================================================
	// TFunction - Owning copyable callable with inline storage
	// ========================================================================

	// @class TFunction
	// @brief An owning, copyable wrapper for a callable object
	// 
	// Features inline storage optimization to avoid heap allocation for
	// small functors. Configure inline size with NUM_TFUNCTION_INLINE_BYTES.
	template <typename Ret, typename... ParamTypes>
	class TFunction<Ret(ParamTypes...)> final
		: public Private::Function::TFunctionRefBase<Private::Function::TFunctionStorage<false>, Ret(ParamTypes...)>
	{
		using Super = Private::Function::TFunctionRefBase<Private::Function::TFunctionStorage<false>, Ret(ParamTypes...)>;

	public:
		TFunction(TYPE_OF_NULLPTR = nullptr) {}

		template <typename FunctorType,
				  typename = std::enable_if_t<
					  !TIsTFunction_v<std::decay_t<FunctorType>> &&
					  !TIsTFunctionRef_v<std::decay_t<FunctorType>> &&
					  std::is_invocable_r_v<Ret, std::decay_t<FunctorType>, ParamTypes...>
				  >>
		TFunction(FunctorType&& InFunc)
			: Super(Forward<FunctorType>(InFunc))
		{
			static_assert(!TIsTFunctionRef_v<std::decay_t<FunctorType>>,
				"Cannot construct a TFunction from a TFunctionRef");
		}

		TFunction(TFunction&&) = default;
		TFunction(const TFunction& Other) = default;
		~TFunction() = default;

		TFunction& operator=(TFunction&& Other)
		{
			Swap(*this, Other);
			return *this;
		}

		TFunction& operator=(const TFunction& Other)
		{
			TFunction Temp = Other;
			Swap(*this, Temp);
			return *this;
		}

		TFunction& operator=(TYPE_OF_NULLPTR)
		{
			Super::Reset();
			return *this;
		}

		OLO_FINLINE explicit operator bool() const { return Super::IsSet(); }
		OLO_FINLINE bool IsSet() const { return Super::IsSet(); }
		OLO_FINLINE bool operator==(TYPE_OF_NULLPTR) const { return !*this; }
		OLO_FINLINE bool operator!=(TYPE_OF_NULLPTR) const { return (bool)*this; }

		friend void Swap(TFunction& A, TFunction& B)
		{
			// Use bitwise swap like UE5.7 for maximum performance.
			// TFunction is trivially relocatable (its storage is just bytes + a pointer),
			// so we can safely memcpy the raw bytes without calling constructors/destructors.
			// This avoids 3 move constructions + 3 destructor calls in favor of 3 memcpy operations.
			alignas(TFunction) u8 TempStorage[sizeof(TFunction)];
			FMemory::Memcpy(TempStorage, &A, sizeof(TFunction));
			FMemory::Memcpy(&A, &B, sizeof(TFunction));
			FMemory::Memcpy(&B, TempStorage, sizeof(TFunction));
		}
	};

	// ========================================================================
	// TUniqueFunction - Move-only owning callable with inline storage
	// ========================================================================

	// @class TUniqueFunction
	// @brief A move-only, owning wrapper for a callable object
	// 
	// Unlike TFunction, TUniqueFunction supports non-copyable functors.
	// Features inline storage optimization to avoid heap allocation.
	template <typename Ret, typename... ParamTypes>
	class TUniqueFunction<Ret(ParamTypes...)> final
		: public Private::Function::TFunctionRefBase<Private::Function::TFunctionStorage<true>, Ret(ParamTypes...)>
	{
		using Super = Private::Function::TFunctionRefBase<Private::Function::TFunctionStorage<true>, Ret(ParamTypes...)>;

	public:
		TUniqueFunction(TYPE_OF_NULLPTR = nullptr) {}

		template <typename FunctorType,
				  typename = std::enable_if_t<
					  !TIsTUniqueFunction_v<std::decay_t<FunctorType>> &&
					  !TIsTFunction_v<std::decay_t<FunctorType>> &&
					  std::is_invocable_r_v<Ret, std::decay_t<FunctorType>, ParamTypes...>
				  >>
		TUniqueFunction(FunctorType&& InFunc)
			: Super(Forward<FunctorType>(InFunc))
		{
			static_assert(!TIsTFunctionRef_v<std::decay_t<FunctorType>>,
				"Cannot construct a TUniqueFunction from a TFunctionRef");
		}

		// Construct from TFunction (take ownership)
		TUniqueFunction(TFunction<Ret(ParamTypes...)>&& Other)
			: Super(MoveTemp(*reinterpret_cast<Private::Function::TFunctionRefBase<Private::Function::TFunctionStorage<false>, Ret(ParamTypes...)>*>(&Other)))
		{
		}

		TUniqueFunction& operator=(TUniqueFunction&& Other)
		{
			if (this != &Other)
			{
				// Destroy current contents
				Super::Reset();
				// Move-construct in place from Other
				new (this) TUniqueFunction(MoveTemp(Other));
			}
			return *this;
		}

		TUniqueFunction(TUniqueFunction&&) = default;
		TUniqueFunction(const TUniqueFunction& Other) = delete;
		TUniqueFunction& operator=(const TUniqueFunction& Other) = delete;
		~TUniqueFunction() = default;

		void Reset() { Super::Reset(); }

		OLO_FINLINE explicit operator bool() const { return Super::IsSet(); }
		OLO_FINLINE bool IsSet() const { return Super::IsSet(); }
	};

	// ========================================================================
	// Deduction Guides
	// ========================================================================

	template <typename Ret, typename... Args>
	TFunctionRef(Ret(*)(Args...)) -> TFunctionRef<Ret(Args...)>;

	template <typename Ret, typename... Args>
	TFunction(Ret(*)(Args...)) -> TFunction<Ret(Args...)>;

	template <typename Ret, typename... Args>
	TUniqueFunction(Ret(*)(Args...)) -> TUniqueFunction<Ret(Args...)>;

} // namespace OloEngine
