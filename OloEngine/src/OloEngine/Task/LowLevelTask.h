// LowLevelTask.h - Low-level task primitives (FTask, ETaskState, ETaskPriority)
// Ported from UE5.7 Async/Fundamental/Task.h

#pragma once

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201) // nameless struct/union
#endif

#include "OloEngine/Core/Base.h"
#include "OloEngine/Task/TaskDelegate.h"
#include "OloEngine/Memory/Platform.h"
#include "OloEngine/Misc/EnumClassFlags.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <atomic>

#define LOWLEVEL_TASK_SIZE OLO_PLATFORM_CACHE_LINE_SIZE

// Forward declarations for friend access
namespace OloEngine::Tasks::Private
{
    class FTaskBase;
}

namespace OloEngine::LowLevelTasks
{
    // @enum ETaskPriority
    // @brief Priority levels for tasks
    enum class ETaskPriority : i8
    {
        High,
        Normal,
        Default = Normal,
        ForegroundCount,
        BackgroundHigh = ForegroundCount,
        BackgroundNormal,
        BackgroundLow,
        Count,
        Inherit, // Inherit the TaskPriority from the launching Task or the Default Priority if not launched from a Task.
    };

    // @brief Convert priority enum to string
    inline const char* ToString(ETaskPriority Priority)
    {
        if (Priority < ETaskPriority::High || Priority >= ETaskPriority::Count)
        {
            return nullptr;
        }

        const char* TaskPriorityToStr[] = {
            "High",
            "Normal",
            "BackgroundHigh",
            "BackgroundNormal",
            "BackgroundLow"
        };
        return TaskPriorityToStr[static_cast<i32>(Priority)];
    }

    // @brief Parse priority string to enum
    inline bool ToTaskPriority(const char* PriorityStr, ETaskPriority& OutPriority)
    {
        if (!PriorityStr)
        {
            return false;
        }

        // Simple case-insensitive comparison helper
        auto StrCmpI = [](const char* a, const char* b) -> bool
        {
            if (!a || !b)
                return false;
            while (*a && *b)
            {
                char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
                if (ca != cb)
                    return false;
                ++a;
                ++b;
            }
            return *a == *b;
        };

        if (StrCmpI(PriorityStr, ToString(ETaskPriority::High)))
        {
            OutPriority = ETaskPriority::High;
            return true;
        }

        if (StrCmpI(PriorityStr, ToString(ETaskPriority::Normal)))
        {
            OutPriority = ETaskPriority::Normal;
            return true;
        }

        if (StrCmpI(PriorityStr, ToString(ETaskPriority::BackgroundHigh)))
        {
            OutPriority = ETaskPriority::BackgroundHigh;
            return true;
        }

        if (StrCmpI(PriorityStr, ToString(ETaskPriority::BackgroundNormal)))
        {
            OutPriority = ETaskPriority::BackgroundNormal;
            return true;
        }

        if (StrCmpI(PriorityStr, ToString(ETaskPriority::BackgroundLow)))
        {
            OutPriority = ETaskPriority::BackgroundLow;
            return true;
        }

        return false;
    }

    // @enum ECancellationFlags
    // @brief Flags controlling task cancellation behavior
    enum class ECancellationFlags : i8
    {
        None = 0 << 0,
        TryLaunchOnSuccess = 1 << 0,    // try to launch the continuation immediately if it was not launched yet (requires PrelaunchCancellation to work)
        PrelaunchCancellation = 1 << 1, // allow cancellation before a task has been launched (this also allows the optimization of TryLaunchOnSuccess)
        DefaultFlags = TryLaunchOnSuccess | PrelaunchCancellation,
    };
    ENUM_CLASS_FLAGS(ECancellationFlags)

    // @enum ETaskFlags
    // @brief Flags controlling task behavior
    enum class ETaskFlags : i8
    {
        AllowNothing = 0 << 0,
        AllowBusyWaiting = 1 << 0,
        AllowCancellation = 1 << 1,
        AllowEverything = AllowBusyWaiting | AllowCancellation,
        DefaultFlags = AllowEverything,
    };
    ENUM_CLASS_FLAGS(ETaskFlags)

    enum class ETaskState : i8
    {
        ReadyState = 0,
        CanceledFlag = 1 << 0,
        ScheduledFlag = 1 << 1,
        RunningFlag = 1 << 2,
        ExpeditingFlag = 1 << 3,
        ExpeditedFlag = 1 << 4,
        CompletedFlag = 1 << 5,
        Count = (1 << 6) - 1,

        Ready = ReadyState,                                        // means the Task is ready to be launched
        CanceledAndReady = Ready | CanceledFlag,                   // means the task was canceled and is ready to be launched (it still is required to be launched)
        Scheduled = Ready | ScheduledFlag,                         // means the task is launched and therefore queued for execution by a worker
        Canceled = CanceledAndReady | ScheduledFlag,               // means the task was canceled and launched and therefore queued for execution by a worker (which already might be executing its continuation)
        Running = Scheduled | RunningFlag,                         // means the task is executing its runnable and continuation by a worker
        CanceledAndRunning = Canceled | RunningFlag,               // means the task is executing its continuation but the runnable was cancelled
        Expediting = Running | ExpeditingFlag,                     // means the task is expediting and the scheduler has released its reference to the expediting thread before that was finished
        Expedited = Expediting | ExpeditedFlag,                    // means the task was expedited
        Completed = Running | CompletedFlag,                       // means the task is completed with execution
        ExpeditedAndCompleted = Expedited | CompletedFlag,         // means the task is completed with execution and the runnable was expedited
        CanceledAndCompleted = CanceledAndRunning | CompletedFlag, // means the task is completed with execution of its continuation but the runnable was cancelled
    };
    ENUM_CLASS_FLAGS(ETaskState)

    // @class TDeleter
    // @brief Generic implementation of a Deleter for cleanup after a Task finished
    //
    // This can be done by capturing a TDeleter like so:
    // [Deleter(LowLevelTasks::TDeleter<Type, &Type::DeleteFunction>(value))](){ }
    template<typename Type, void (Type::*DeleteFunction)()>
    class TDeleter
    {
        Type* m_Value;

      public:
        OLO_FINLINE TDeleter(Type* InValue) : m_Value(InValue)
        {
        }

        OLO_FINLINE TDeleter(const TDeleter&) = delete;
        OLO_FINLINE TDeleter(TDeleter&& Other) : m_Value(Other.m_Value)
        {
            Other.m_Value = nullptr;
        }

        OLO_FINLINE Type* operator->() const
        {
            return m_Value;
        }

        OLO_FINLINE ~TDeleter()
        {
            if (m_Value)
            {
                (m_Value->*DeleteFunction)();
            }
        }
    };

    // Forward declaration
    class FTask;

    namespace Tasks_Impl
    {
        // @class FTaskBase
        // @brief Base class hiding implementation details from FScheduler
        class FTaskBase
        {
            class FPackedDataAtomic;

            friend class ::OloEngine::LowLevelTasks::FTask;

            // Non-copyable and non-movable
            FTaskBase(const FTaskBase&) = delete;
            FTaskBase& operator=(const FTaskBase&) = delete;
            FTaskBase(FTaskBase&&) = delete;
            FTaskBase& operator=(FTaskBase&&) = delete;

            union FPackedData
            {
                uptr PackedData;
                struct
                {
                    uptr State : 6;
                    uptr DebugName : 53;
                    uptr Priority : 3;
                    uptr Flags : 2;
                };

              private:
                friend class FTaskBase::FPackedDataAtomic;
                FPackedData(uptr InPackedData) : PackedData(InPackedData)
                {
                }

                constexpr FPackedData()
                    : State(static_cast<uptr>(ETaskState::CompletedFlag)), DebugName(0ull), Priority(static_cast<uptr>(ETaskPriority::Count)), Flags(static_cast<uptr>(ETaskFlags::DefaultFlags))
                {
                    static_assert(sizeof(uptr) == 8, "32-bit platforms are not supported");
                    static_assert(static_cast<uptr>(ETaskPriority::Count) <= (1ull << 3), "Not enough bits to store ETaskPriority");
                    static_assert(static_cast<uptr>(ETaskState::Count) <= (1ull << 6), "Not enough bits to store ETaskState");
                    static_assert(static_cast<uptr>(ETaskFlags::AllowEverything) < (1ull << 2), "Not enough bits to store ETaskFlags");
                }

              public:
                FPackedData(const char* InDebugName, ETaskPriority InPriority, ETaskState InState, ETaskFlags InFlags)
                    : State(static_cast<uptr>(InState)), DebugName(reinterpret_cast<uptr>(InDebugName)), Priority(static_cast<uptr>(InPriority)), Flags(static_cast<uptr>(InFlags))
                {
                    OLO_CORE_ASSERT(reinterpret_cast<uptr>(InDebugName) < (1ull << 53), "Debug name pointer too large");
                    OLO_CORE_ASSERT(static_cast<uptr>(InPriority) < (1ull << 3), "Priority value out of range");
                    OLO_CORE_ASSERT(static_cast<uptr>(InState) < (1ull << 6), "State value out of range");
                    OLO_CORE_ASSERT(static_cast<uptr>(InFlags) < (1ull << 2), "Flags value out of range");
                    static_assert(sizeof(FPackedData) == sizeof(uptr), "Packed data needs to be pointer size");
                }

                FPackedData(const FPackedData& Other, ETaskState State)
                    : FPackedData(Other.GetDebugName(), Other.GetPriority(), State, Other.GetFlags())
                {
                }

                OLO_FINLINE const char* GetDebugName() const
                {
                    return reinterpret_cast<const char*>(DebugName);
                }

                OLO_FINLINE ETaskPriority GetPriority() const
                {
                    return static_cast<ETaskPriority>(Priority);
                }

                OLO_FINLINE ETaskState GetState() const
                {
                    return static_cast<ETaskState>(State);
                }

                OLO_FINLINE ETaskFlags GetFlags() const
                {
                    return static_cast<ETaskFlags>(Flags);
                }
            };

            class FPackedDataAtomic
            {
                std::atomic<uptr> PackedData{ FPackedData().PackedData };

              public:
                ETaskState fetch_or(ETaskState State, std::memory_order Order)
                {
                    return static_cast<ETaskState>(FPackedData(PackedData.fetch_or(static_cast<uptr>(State), Order)).State);
                }

                bool compare_exchange_strong(FPackedData& Expected, FPackedData Desired, std::memory_order Success, std::memory_order Failure)
                {
                    return PackedData.compare_exchange_strong(Expected.PackedData, Desired.PackedData, Success, Failure);
                }

                bool compare_exchange_strong(FPackedData& Expected, FPackedData Desired, std::memory_order Order)
                {
                    return PackedData.compare_exchange_strong(Expected.PackedData, Desired.PackedData, Order);
                }

                FPackedData load(std::memory_order Order) const
                {
                    return PackedData.load(Order);
                }

                void store(const FPackedData& Expected, std::memory_order Order)
                {
                    PackedData.store(Expected.PackedData, Order);
                }
            };

          private:
            using FTaskDelegate = TTaskDelegate<FTask*(bool), LOWLEVEL_TASK_SIZE - sizeof(FPackedData) - sizeof(void*)>;
            FTaskDelegate Runnable;
            mutable void* UserData = nullptr;
            FPackedDataAtomic PackedData;

          private:
            FTaskBase() = default;
        };
    } // namespace Tasks_Impl

    // @class FTask
    // @brief Minimal low level task interface
    class FTask final : private Tasks_Impl::FTaskBase
    {
        friend class FScheduler;
        friend class ::OloEngine::Tasks::Private::FTaskBase;

        // Non-copyable and non-movable
        FTask(const FTask&) = delete;
        FTask& operator=(const FTask&) = delete;
        FTask(FTask&&) = delete;
        FTask& operator=(FTask&&) = delete;

        // Thread-local storage for the currently executing task
        // Defined inline to ensure ODR compliance (C++17 inline variable)
        static inline thread_local FTask* s_ActiveTask = nullptr;

      public:
        // @brief Check if the task is completed and this taskhandle can be recycled
        OLO_FINLINE bool IsCompleted(std::memory_order MemoryOrder = std::memory_order_seq_cst) const
        {
            ETaskState State = PackedData.load(MemoryOrder).GetState();
            return EnumHasAnyFlags(State, ETaskState::CompletedFlag);
        }

        // @brief Check if the task was canceled but might still need to be launched
        OLO_FINLINE bool WasCanceled() const
        {
            ETaskState State = PackedData.load(std::memory_order_relaxed).GetState();
            return EnumHasAnyFlags(State, ETaskState::CanceledFlag);
        }

        // @brief Check if the task was expedited or that it already completed
        OLO_FINLINE bool WasExpedited() const
        {
            ETaskState State = PackedData.load(std::memory_order_acquire).GetState();
            return EnumHasAnyFlags(State, ETaskState::ExpeditedFlag | ETaskState::CompletedFlag);
        }

      private:
        // Scheduler internal interface to speed things up
        OLO_FINLINE bool WasCanceledOrIsExpediting() const
        {
            ETaskState State = PackedData.load(std::memory_order_relaxed).GetState();
            return EnumHasAnyFlags(State, ETaskState::CanceledFlag | ETaskState::RunningFlag);
        }

      public:
        // @brief Check if the task is ready to be launched but might already been canceled
        OLO_FINLINE bool IsReady() const
        {
            ETaskState State = PackedData.load(std::memory_order_relaxed).GetState();
            return !EnumHasAnyFlags(State, ~ETaskState::CanceledFlag);
        }

        // @brief Get the currently active task if any
        static const FTask* GetActiveTask()
        {
            return s_ActiveTask;
        }

        // @brief Try to cancel the task if it has not been launched yet
        OLO_FINLINE bool TryCancel(ECancellationFlags CancellationFlags = ECancellationFlags::DefaultFlags);

        // @brief Try to revive a canceled task (reverting the cancellation as if it never happened)
        //
        // If it had been canceled and the scheduler has not run it yet it succeeds.
        OLO_FINLINE bool TryRevive();

        // @brief Try to expedite the task
        //
        // If succeeded it will run immediately but it will not set the completed state until
        // the scheduler has executed it, because the scheduler still holds a reference.
        // To check for completion in the context of expediting use WasExpedited.
        // The TaskHandle cannot be reused until IsCompleted returns true.
        //
        // @param Continuation Optional Continuation that needs to be executed or scheduled by the caller
        //                     (can only be non null if the operation returned true)
        OLO_FINLINE bool TryExpedite();
        OLO_FINLINE bool TryExpedite(FTask*& Continuation);

        // @brief Try to execute the task if it has not been launched yet the task will execute immediately
        //
        // @param Continuation Optional Continuation that needs to be executed or scheduled by the caller
        //                     (can only be non null if the operation returned true)
        OLO_FINLINE bool TryExecute();
        OLO_FINLINE bool TryExecute(FTask*& Continuation);

        template<typename TRunnable>
        OLO_FINLINE void Init(const char* InDebugName, ETaskPriority InPriority, TRunnable&& InRunnable, ETaskFlags Flags = ETaskFlags::DefaultFlags);

        template<typename TRunnable>
        OLO_FINLINE void Init(const char* InDebugName, TRunnable&& InRunnable, ETaskFlags Flags = ETaskFlags::DefaultFlags);

        OLO_FINLINE const char* GetDebugName() const;
        OLO_FINLINE ETaskPriority GetPriority() const;
        OLO_FINLINE bool IsBackgroundTask() const;
        OLO_FINLINE bool AllowBusyWaiting() const;
        OLO_FINLINE bool AllowCancellation() const;

        struct FInitData
        {
            const char* DebugName;
            ETaskPriority Priority;
            ETaskFlags Flags;
        };
        OLO_FINLINE FInitData GetInitData() const;

        void* GetUserData() const
        {
            return UserData;
        }
        void SetUserData(void* NewUserData) const
        {
            UserData = NewUserData;
        }

      public:
        FTask() = default;
        OLO_FINLINE ~FTask();

      private: // Interface of the Scheduler
        OLO_FINLINE static bool PermitBackgroundWork()
        {
            return s_ActiveTask && s_ActiveTask->IsBackgroundTask();
        }

        OLO_FINLINE bool TryPrepareLaunch();

        template<bool bIsExpeditingThread>
        OLO_FINLINE void TryFinish();

        OLO_FINLINE FTask* ExecuteTask();
        OLO_FINLINE void InheritParentData(ETaskPriority& Priority);
    };

    // ******************
    // * IMPLEMENTATION *
    // ******************

    OLO_FINLINE ETaskPriority FTask::GetPriority() const
    {
        return PackedData.load(std::memory_order_relaxed).GetPriority();
    }

    OLO_FINLINE void FTask::InheritParentData(ETaskPriority& Priority)
    {
        const FTask* LocalActiveTask = FTask::GetActiveTask();
        if (LocalActiveTask != nullptr)
        {
            if (Priority == ETaskPriority::Inherit)
            {
                Priority = LocalActiveTask->GetPriority();
            }
            UserData = LocalActiveTask->GetUserData();
        }
        else
        {
            if (Priority == ETaskPriority::Inherit)
            {
                Priority = ETaskPriority::Default;
            }
            UserData = nullptr;
        }
    }

    template<typename TRunnable>
    OLO_FINLINE void FTask::Init(const char* InDebugName, ETaskPriority InPriority, TRunnable&& InRunnable, ETaskFlags Flags)
    {
        OLO_CORE_ASSERT(IsCompleted(), "Task must be completed before reinitializing. State: {}", static_cast<i32>(PackedData.load(std::memory_order_relaxed).GetState()));
        OLO_CORE_ASSERT(!Runnable.IsSet(), "Runnable must not be set");

        // If the Runnable returns an FTask* then enable symmetric switching
        if constexpr (std::is_same_v<FTask*, decltype(Private::DeclVal<TRunnable>()())>)
        {
            Runnable = [LocalRunnable = Forward<TRunnable>(InRunnable)](const bool bNotCanceled) mutable -> FTask*
            {
                if (bNotCanceled)
                {
                    FTask* Task = LocalRunnable();
                    return Task;
                }
                return nullptr;
            };
        }
        else
        {
            Runnable = [LocalRunnable = Forward<TRunnable>(InRunnable)](const bool bNotCanceled) mutable -> FTask*
            {
                if (bNotCanceled)
                {
                    LocalRunnable();
                }
                return nullptr;
            };
        }
        InheritParentData(InPriority);
        PackedData.store(FPackedData(InDebugName, InPriority, ETaskState::Ready, Flags), std::memory_order_release);
    }

    template<typename TRunnable>
    OLO_FINLINE void FTask::Init(const char* InDebugName, TRunnable&& InRunnable, ETaskFlags Flags)
    {
        Init(InDebugName, ETaskPriority::Default, Forward<TRunnable>(InRunnable), Flags);
    }

    OLO_FINLINE FTask::~FTask()
    {
        OLO_CORE_ASSERT(IsCompleted(), "Task must be completed before destruction. State: {}", static_cast<i32>(PackedData.load(std::memory_order_relaxed).GetState()));
    }

    OLO_FINLINE bool FTask::TryPrepareLaunch()
    {
        return !EnumHasAnyFlags(PackedData.fetch_or(ETaskState::ScheduledFlag, std::memory_order_release), ETaskState::ScheduledFlag);
    }

    OLO_FINLINE bool FTask::TryCancel(ECancellationFlags CancellationFlags)
    {
        bool bPrelaunchCancellation = EnumHasAnyFlags(CancellationFlags, ECancellationFlags::PrelaunchCancellation);
        bool bTryLaunchOnSuccess = EnumHasAllFlags(CancellationFlags, ECancellationFlags::PrelaunchCancellation | ECancellationFlags::TryLaunchOnSuccess);

        FPackedData LocalPackedData = PackedData.load(std::memory_order_relaxed);
        FPackedData ReadyState(LocalPackedData, ETaskState::Ready);
        FPackedData ScheduledState(LocalPackedData, ETaskState::Scheduled);

        // To launch a canceled task it has to go through TryPrepareLaunch which is doing the memory_order_release
        bool WasCanceledResult = EnumHasAnyFlags(LocalPackedData.GetFlags(), ETaskFlags::AllowCancellation) && ((bPrelaunchCancellation && PackedData.compare_exchange_strong(ReadyState, FPackedData(LocalPackedData, ETaskState::CanceledAndReady), std::memory_order_acquire)) || PackedData.compare_exchange_strong(ScheduledState, FPackedData(LocalPackedData, ETaskState::Canceled), std::memory_order_acquire));

        if (bTryLaunchOnSuccess && WasCanceledResult && TryPrepareLaunch())
        {
            OLO_CORE_VERIFY(ExecuteTask() == nullptr);
            return true;
        }
        return WasCanceledResult;
    }

    OLO_FINLINE bool FTask::TryRevive()
    {
        FPackedData LocalPackedData = PackedData.load(std::memory_order_relaxed);
        OLO_CORE_ASSERT(EnumHasAnyFlags(LocalPackedData.GetState(), ETaskState::CanceledFlag), "Cannot revive a non-canceled task");
        if (EnumHasAnyFlags(LocalPackedData.GetState(), ETaskState::RunningFlag))
        {
            return false;
        }

        FPackedData CanceledReadyState(LocalPackedData, ETaskState::CanceledAndReady);
        FPackedData CanceledState(LocalPackedData, ETaskState::Canceled);
        return PackedData.compare_exchange_strong(CanceledReadyState, FPackedData(LocalPackedData, ETaskState::Ready), std::memory_order_release) || PackedData.compare_exchange_strong(CanceledState, FPackedData(LocalPackedData, ETaskState::Scheduled), std::memory_order_release);
    }

    OLO_FINLINE bool FTask::TryExecute(FTask*& OutContinuation)
    {
        if (TryPrepareLaunch())
        {
            OutContinuation = ExecuteTask();
            return true;
        }
        return false;
    }

    OLO_FINLINE bool FTask::TryExecute()
    {
        FTask* Continuation = nullptr;
        bool Result = TryExecute(Continuation);
        OLO_CORE_ASSERT(Continuation == nullptr, "Continuation should be null");
        return Result;
    }

    template<bool bIsExpeditingThread>
    OLO_FINLINE void FTask::TryFinish()
    {
        const ETaskState NextState = bIsExpeditingThread ? ETaskState::ExpeditedFlag | ETaskState::ExpeditingFlag : ETaskState::ExpeditingFlag;
        ETaskState PreviousState = PackedData.fetch_or(NextState, std::memory_order_acq_rel);
        if constexpr (bIsExpeditingThread)
        {
            OLO_CORE_ASSERT(PreviousState == ETaskState::Running || PreviousState == ETaskState::Expediting, "Invalid state for expediting thread");
        }
        if (EnumHasAnyFlags(PreviousState, ETaskState::ExpeditingFlag))
        {
            FTaskDelegate LocalRunnable = MoveTemp(Runnable);
            // Do not access the task again after this call
            // as by definition the task can be considered dead
            PreviousState = PackedData.fetch_or(ETaskState::CompletedFlag, std::memory_order_seq_cst);
            OLO_CORE_ASSERT(PreviousState == ETaskState::Expedited, "Invalid state after expediting");
        }
    }

    OLO_FINLINE bool FTask::TryExpedite(FTask*& OutContinuation)
    {
        FPackedData LocalPackedData = PackedData.load(std::memory_order_relaxed);
        FPackedData ScheduledState(LocalPackedData, ETaskState::Scheduled);
        if (PackedData.compare_exchange_strong(ScheduledState, FPackedData(LocalPackedData, ETaskState::Running), std::memory_order_acquire))
        {
            OutContinuation = Runnable(true);
            TryFinish<true>();
            return true;
        }
        return false;
    }

    OLO_FINLINE bool FTask::TryExpedite()
    {
        FTask* Continuation = nullptr;
        bool Result = TryExpedite(Continuation);
        OLO_CORE_ASSERT(Continuation == nullptr, "Continuation should be null");
        return Result;
    }

    OLO_FINLINE FTask* FTask::ExecuteTask()
    {
        ETaskState PreviousState = PackedData.fetch_or(ETaskState::RunningFlag, std::memory_order_acquire);
        OLO_CORE_ASSERT(EnumHasAnyFlags(PreviousState, ETaskState::ScheduledFlag), "Task must be scheduled before execution");

        FTask* Continuation = nullptr;
        if (!EnumHasAnyFlags(PreviousState, ETaskState::RunningFlag)) // we are running or canceled
        {
            FTaskDelegate LocalRunnable;
            Continuation = Runnable.CallAndMove(LocalRunnable, !EnumHasAnyFlags(PreviousState, ETaskState::CanceledFlag));
            // Do not access the task again after this call
            // as by definition the task can be considered dead
            PreviousState = PackedData.fetch_or(ETaskState::CompletedFlag, std::memory_order_seq_cst);
            OLO_CORE_ASSERT(PreviousState == ETaskState::Running || PreviousState == ETaskState::CanceledAndRunning, "Invalid state after execution");
        }
        else // we are expedited
        {
            OLO_CORE_ASSERT(PreviousState == ETaskState::Running || PreviousState == ETaskState::Expediting || PreviousState == ETaskState::Expedited, "Invalid state for expedited task");
            TryFinish<false>();
        }

        return Continuation;
    }

    OLO_FINLINE const char* FTask::GetDebugName() const
    {
        return PackedData.load(std::memory_order_relaxed).GetDebugName();
    }

    OLO_FINLINE bool FTask::IsBackgroundTask() const
    {
        return PackedData.load(std::memory_order_relaxed).GetPriority() >= ETaskPriority::ForegroundCount;
    }

    OLO_FINLINE bool FTask::AllowBusyWaiting() const
    {
        return EnumHasAnyFlags(PackedData.load(std::memory_order_relaxed).GetFlags(), ETaskFlags::AllowBusyWaiting);
    }

    OLO_FINLINE bool FTask::AllowCancellation() const
    {
        return EnumHasAnyFlags(PackedData.load(std::memory_order_relaxed).GetFlags(), ETaskFlags::AllowCancellation);
    }

    OLO_FINLINE FTask::FInitData FTask::GetInitData() const
    {
        FPackedData LocalPackedData = PackedData.load(std::memory_order_relaxed);
        return { LocalPackedData.GetDebugName(), LocalPackedData.GetPriority(), LocalPackedData.GetFlags() };
    }

} // namespace OloEngine::LowLevelTasks

#ifdef _MSC_VER
#pragma warning(pop)
#endif
