// Copyright Epic Games, Inc. All Rights Reserved.
// Ported to OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Templates/Invoke.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Experimental/ConcurrentLinearAllocator.h"
#include "OloEngine/Misc/Launder.h"

#include <type_traits>

namespace OloEngine::LowLevelTasks
{
    namespace Private
    {
        /**
         * @brief DeclVal implementation for IsInvocable checks
         */
        template <typename T>
        T&& DeclVal();

        /**
         * @brief Void helper for SFINAE
         */
        template <typename T>
        struct TVoid
        {
            using Type = void;
        };

        /**
         * @brief IsInvocable implementation
         */
        template <typename, typename CallableType, typename... ArgTypes>
        struct TIsInvocableImpl
        {
            enum { Value = false };
        };

        template <typename CallableType, typename... ArgTypes>
        struct TIsInvocableImpl<typename TVoid<decltype(Invoke(DeclVal<CallableType>(), DeclVal<ArgTypes>()...))>::Type, CallableType, ArgTypes...>
        {
            enum { Value = true };
        };
    } // namespace Private

    /**
     * @brief Traits class which tests if an instance of CallableType can be invoked with
     *        a list of the arguments of the types provided.
     */
    template <typename CallableType, typename... ArgTypes>
    struct TIsInvocable : Private::TIsInvocableImpl<void, CallableType, ArgTypes...>
    {
    };

    namespace TaskDelegate_Impl
    {
        template<typename ReturnType>
        OLO_FINLINE ReturnType MakeDummyValue()
        {
            return *(reinterpret_cast<ReturnType*>(uptr(1)));
        }

        template<>
        OLO_FINLINE void MakeDummyValue<void>()
        {
            return;
        }
    }

    /**
     * @class TTaskDelegate
     * @brief Version of TUniqueFunction<ReturnType()> that is less wasteful with its memory
     * 
     * This class might be removed when TUniqueFunction<ReturnType()> is fixed.
     * 
     * @tparam ReturnType(ParamTypes...) Function signature
     * @tparam TotalSize Total size of the delegate including inline storage
     */
    template<typename = void(), u32 = OLO_PLATFORM_CACHE_LINE_SIZE>
    class TTaskDelegate;

    template<u32 TotalSize, typename ReturnType, typename... ParamTypes>
    class alignas(8) TTaskDelegate<ReturnType(ParamTypes...), TotalSize>
    {
        template<typename, u32>
        friend class TTaskDelegate;

        using ThisClass = TTaskDelegate<ReturnType(ParamTypes...), TotalSize>;

        struct TTaskDelegateBase
        {
            virtual void Move(TTaskDelegateBase&, void*, void*, u32)
            {
                OLO_CORE_ASSERT(false, "TTaskDelegateBase::Move called on base - vtable lookup optimization issue");
            }

            virtual ReturnType Call(void*, ParamTypes...) const
            {
                OLO_CORE_ASSERT(false, "TTaskDelegateBase::Call called on base - vtable lookup optimization issue");
                return TaskDelegate_Impl::MakeDummyValue<ReturnType>();
            }

            virtual ReturnType CallAndMove(ThisClass&, void*, u32, ParamTypes...)
            {
                OLO_CORE_ASSERT(false, "TTaskDelegateBase::CallAndMove called on base - vtable lookup optimization issue");
                return TaskDelegate_Impl::MakeDummyValue<ReturnType>();
            }

            virtual void Destroy(void*)
            {
                OLO_CORE_ASSERT(false, "TTaskDelegateBase::Destroy called on base - vtable lookup optimization issue");
            }

            virtual bool IsHeapAllocated() const
            {
                OLO_CORE_ASSERT(false, "TTaskDelegateBase::IsHeapAllocated called on base - vtable lookup optimization issue");
                return false;
            }

            virtual bool IsSet() const
            {
                OLO_CORE_ASSERT(false, "TTaskDelegateBase::IsSet called on base - vtable lookup optimization issue");
                return false;
            }

            virtual u32 DelegateSize() const
            {
                OLO_CORE_ASSERT(false, "TTaskDelegateBase::DelegateSize called on base - vtable lookup optimization issue");
                return 0;
            }
        };

        struct TTaskDelegateDummy final : TTaskDelegateBase
        {
            void Move(TTaskDelegateBase&, void*, void*, u32) override
            {
            }

            ReturnType Call(void*, ParamTypes...) const override
            {
                OLO_CORE_ASSERT(false, "Trying to Call a dummy TaskDelegate");
                return TaskDelegate_Impl::MakeDummyValue<ReturnType>();
            }

            ReturnType CallAndMove(ThisClass&, void*, u32, ParamTypes...) override
            {
                OLO_CORE_ASSERT(false, "Trying to Call a dummy TaskDelegate");
                return TaskDelegate_Impl::MakeDummyValue<ReturnType>();
            }

            void Destroy(void*) override
            {
            }

            bool IsHeapAllocated() const override
            {
                return false;
            }

            bool IsSet() const override
            {
                return false;
            }

            u32 DelegateSize() const override
            {
                return 0;
            }
        };

        template<typename TCallableType, bool HeapAllocated>
        struct TTaskDelegateImpl;

        template<typename TCallableType>
        struct TTaskDelegateImpl<TCallableType, false> final : TTaskDelegateBase
        {
            template<typename CallableT>
            OLO_FINLINE TTaskDelegateImpl(CallableT&& Callable, void* InlineData)
            {
                static_assert(TIsInvocable<TCallableType, ParamTypes...>::Value, "TCallableType is not invocable");
                static_assert(std::is_same_v<ReturnType, decltype(Callable(Private::DeclVal<ParamTypes>()...))>, "TCallableType return type does not match");
                static_assert(sizeof(TTaskDelegateImpl<TCallableType, false>) == sizeof(TTaskDelegateBase), "Size must match the Baseclass");
                new (InlineData) TCallableType(Forward<CallableT>(Callable));
            }

            OLO_FINLINE void Move(TTaskDelegateBase& DstWrapper, void* DstData, void* SrcData, u32 DestInlineSize) override
            {
                TCallableType* SrcPtr = reinterpret_cast<TCallableType*>(SrcData);
                if ((sizeof(TCallableType) <= DestInlineSize) && (uptr(DstData) % alignof(TCallableType)) == 0)
                {
                    new (&DstWrapper) TTaskDelegateImpl<TCallableType, false>(MoveTemp(*SrcPtr), DstData);
                }
                else
                {
                    new (&DstWrapper) TTaskDelegateImpl<TCallableType, true>(MoveTemp(*SrcPtr), DstData);
                }
                new (this) TTaskDelegateDummy();
            }

            OLO_FINLINE ReturnType Call(void* InlineData, ParamTypes... Params) const override
            {
                TCallableType* LocalPtr = reinterpret_cast<TCallableType*>(InlineData);
                return Invoke(*LocalPtr, Params...);
            }

            ReturnType CallAndMove(ThisClass& Destination, void* InlineData, u32 DestInlineSize, ParamTypes... Params) override
            {
                struct FScopeExit
                {
                    TTaskDelegateImpl* Self;
                    ThisClass& Dest;
                    void* InlineData;
                    u32 DestInlineSize;
                    ~FScopeExit()
                    {
                        Self->Move(Dest.m_CallableWrapper, Dest.m_InlineStorage, InlineData, DestInlineSize);
                    }
                } ScopeExit{this, Destination, InlineData, DestInlineSize};

                return Call(InlineData, Params...);
            }

            void Destroy(void* InlineData) override
            {
                TCallableType* LocalPtr = reinterpret_cast<TCallableType*>(InlineData);
                LocalPtr->~TCallableType();
            }

            bool IsHeapAllocated() const override
            {
                return false;
            }

            bool IsSet() const override
            {
                return true;
            }

            u32 DelegateSize() const override
            {
                return sizeof(TCallableType);
            }
        };

        template<typename TCallableType>
        struct TTaskDelegateImpl<TCallableType, true> final : TTaskDelegateBase
        {
        private:
            OLO_FINLINE TTaskDelegateImpl(void* DstData, void* SrcData)
            {
                FMemory::Memcpy(DstData, SrcData, sizeof(TCallableType*));
            }

        public:
            template<typename CallableT>
            OLO_FINLINE TTaskDelegateImpl(CallableT&& Callable, void* InlineData)
            {
                static_assert(TIsInvocable<TCallableType, ParamTypes...>::Value, "TCallableType is not invocable");
                static_assert(std::is_same_v<ReturnType, decltype(Callable(Private::DeclVal<ParamTypes>()...))>, "TCallableType return type does not match");
                static_assert(sizeof(TTaskDelegateImpl<TCallableType, true>) == sizeof(TTaskDelegateBase), "Size must match the Baseclass");
                static_assert(alignof(TCallableType) <= FLowLevelTasksBlockAllocationTag::MaxAlignment, "Alignment too large for allocator");
                TCallableType** HeapPtr = reinterpret_cast<TCallableType**>(InlineData);
                *HeapPtr = reinterpret_cast<TCallableType*>(TConcurrentLinearAllocator<FLowLevelTasksBlockAllocationTag>::template Malloc<alignof(TCallableType)>(sizeof(TCallableType)));
                new (*HeapPtr) TCallableType(Forward<CallableT>(Callable));
            }

            OLO_FINLINE void Move(TTaskDelegateBase& DstWrapper, void* DstData, void* SrcData, u32 DestInlineSize) override
            {
                new (&DstWrapper) TTaskDelegateImpl<TCallableType, true>(DstData, SrcData);
                new (this) TTaskDelegateDummy();
            }

            OLO_FINLINE ReturnType Call(void* InlineData, ParamTypes... Params) const override
            {
                TCallableType* HeapPtr = reinterpret_cast<TCallableType*>(*reinterpret_cast<void* const*>(InlineData));
                return Invoke(*HeapPtr, Params...);
            }

            ReturnType CallAndMove(ThisClass& Destination, void* InlineData, u32 DestInlineSize, ParamTypes... Params) override
            {
                struct FScopeExit
                {
                    TTaskDelegateImpl* Self;
                    ThisClass& Dest;
                    void* InlineData;
                    u32 DestInlineSize;
                    ~FScopeExit()
                    {
                        Self->Move(Dest.m_CallableWrapper, Dest.m_InlineStorage, InlineData, DestInlineSize);
                    }
                } ScopeExit{this, Destination, InlineData, DestInlineSize};

                return Call(InlineData, Params...);
            }

            void Destroy(void* InlineData) override
            {
                TCallableType* HeapPtr = reinterpret_cast<TCallableType*>(*reinterpret_cast<void**>(InlineData));
                using DestructorType = TCallableType;
                HeapPtr->DestructorType::~TCallableType();
                TConcurrentLinearAllocator<FLowLevelTasksBlockAllocationTag>::Free(HeapPtr);
            }

            bool IsHeapAllocated() const override
            {
                return true;
            }

            bool IsSet() const override
            {
                return true;
            }

            u32 DelegateSize() const override
            {
                return sizeof(TCallableType);
            }
        };

    public:
        TTaskDelegate()
        {
            static_assert(TotalSize % 8 == 0, "TotalSize must be divisible by 8");
            static_assert(TotalSize >= (sizeof(TTaskDelegateBase) + sizeof(void*)), "TotalSize must be large enough to fit a vtable and pointer");
            new (&m_CallableWrapper) TTaskDelegateDummy();
        }

        template<u32 SourceTotalSize>
        TTaskDelegate(const TTaskDelegate<ReturnType(ParamTypes...), SourceTotalSize>&) = delete;

        template<u32 SourceTotalSize>
        TTaskDelegate(TTaskDelegate<ReturnType(ParamTypes...), SourceTotalSize>&& Other)
        {
            Other.GetWrapper()->Move(m_CallableWrapper, m_InlineStorage, Other.m_InlineStorage, InlineStorageSize);
        }

        template<typename CallableT>
        TTaskDelegate(CallableT&& Callable)
        {
            using TCallableType = std::decay_t<CallableT>;
            if constexpr ((sizeof(TCallableType) <= InlineStorageSize) && ((uptr(InlineStorageSize) % alignof(TCallableType)) == 0))
            {
                new (&m_CallableWrapper) TTaskDelegateImpl<TCallableType, false>(Forward<CallableT>(Callable), m_InlineStorage);
            }
            else
            {
                new (&m_CallableWrapper) TTaskDelegateImpl<TCallableType, true>(Forward<CallableT>(Callable), m_InlineStorage);
            }
        }

        ~TTaskDelegate()
        {
            GetWrapper()->Destroy(m_InlineStorage);
        }

        ReturnType operator()(ParamTypes... Params) const
        {
            return GetWrapper()->Call(m_InlineStorage, Params...);
        }

        template<u32 DestTotalSize>
        ReturnType CallAndMove(TTaskDelegate<ReturnType(ParamTypes...), DestTotalSize>& Destination, ParamTypes... Params)
        {
            OLO_CORE_ASSERT(!Destination.IsSet(), "Destination delegate must not be set");
            return GetWrapper()->CallAndMove(Destination, m_InlineStorage, TTaskDelegate<ReturnType(ParamTypes...), DestTotalSize>::InlineStorageSize, Params...);
        }

        template<u32 SourceTotalSize>
        ThisClass& operator= (const TTaskDelegate<ReturnType(ParamTypes...), SourceTotalSize>&) = delete;

        template<u32 SourceTotalSize>
        ThisClass& operator= (TTaskDelegate<ReturnType(ParamTypes...), SourceTotalSize>&& Other)
        {
            GetWrapper()->Destroy(m_InlineStorage);
            Other.GetWrapper()->Move(m_CallableWrapper, m_InlineStorage, Other.m_InlineStorage, InlineStorageSize);
            return *this;
        }

        template<typename CallableT>
        ThisClass& operator= (CallableT&& Callable)
        {
            using TCallableType = std::decay_t<CallableT>;
            GetWrapper()->Destroy(m_InlineStorage);
            if constexpr ((sizeof(TCallableType) <= InlineStorageSize) && ((uptr(InlineStorageSize) % alignof(TCallableType)) == 0))
            {
                new (&m_CallableWrapper) TTaskDelegateImpl<TCallableType, false>(Forward<CallableT>(Callable), m_InlineStorage);
            }
            else
            {
                new (&m_CallableWrapper) TTaskDelegateImpl<TCallableType, true>(Forward<CallableT>(Callable), m_InlineStorage);
            }
            return *this;
        }

        void Destroy()
        {
            GetWrapper()->Destroy(m_InlineStorage);
            new (&m_CallableWrapper) TTaskDelegateDummy();
        }

        bool IsHeapAllocated() const
        {
            return GetWrapper()->IsHeapAllocated();
        }

        bool IsSet() const
        {
            return GetWrapper()->IsSet();
        }

        u32 DelegateSize() const
        {
            return GetWrapper()->DelegateSize();
        }

    private:
        static constexpr u32 InlineStorageSize = TotalSize - sizeof(TTaskDelegateBase);
        mutable char m_InlineStorage[InlineStorageSize];
        TTaskDelegateBase m_CallableWrapper;

        TTaskDelegateBase* GetWrapper()
        {
            return OLO_LAUNDER(reinterpret_cast<TTaskDelegateBase*>(&m_CallableWrapper));
        }

        const TTaskDelegateBase* GetWrapper() const
        {
            return OLO_LAUNDER(reinterpret_cast<const TTaskDelegateBase*>(&m_CallableWrapper));
        }
    };

} // namespace OloEngine::LowLevelTasks
