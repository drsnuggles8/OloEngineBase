// TaskTag.cpp - Named thread identification and tagging system
// Ported from UE5.7 ETaskTag and FTaskTagScope

#include "OloEngine/Core/TaskTag.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/PlatformTLS.h"
#include "OloEngine/Task/Scheduler.h"

namespace OLO
{
    // Use OloEngine namespace for FPlatformTLS
    using OloEngine::FPlatformTLS;

    //////////////////////////////////////////////////////////////////////////
    // Global thread ID definitions

    u32 GGameThreadId = 0;
    u32 GRenderThreadId = 0;
    u32 GSlateLoadingThreadId = 0;
    u32 GRHIThreadId = 0;
    bool GIsGameThreadIdInitialized = false;
    bool GIsRunningRHIInSeparateThread = false;
    bool GIsRunningRHIInDedicatedThread = false;

    //////////////////////////////////////////////////////////////////////////
    // FTaskTagScope implementation

    thread_local ETaskTag FTaskTagScope::s_ActiveTaskTag = ETaskTag::EStaticInit;
    std::atomic<i32> FTaskTagScope::s_ActiveNamedThreads{ 0 };

    // Store the thread ID of the thread that ran static initialization
    static u32 s_StaticInitThreadId = FPlatformTLS::GetCurrentThreadId();

    i32 FTaskTagScope::GetStaticThreadId()
    {
        return static_cast<i32>(s_StaticInitThreadId);
    }

    ETaskTag FTaskTagScope::SwapTag(ETaskTag NewTag)
    {
        ETaskTag ReturnValue = s_ActiveTaskTag;
        s_ActiveTaskTag = NewTag;
        return ReturnValue;
    }

    void FTaskTagScope::SetTagNone()
    {
        s_ActiveTaskTag = ETaskTag::ENone;
    }

    void FTaskTagScope::SetTagStaticInit()
    {
        s_ActiveTaskTag = ETaskTag::EStaticInit;
    }

    FTaskTagScope::FTaskTagScope(ETaskTag InTag)
        : Tag(InTag)
    {
        OLO_CORE_ASSERT(Tag != ETaskTag::ENone, "None cannot be used as a Tag");
        OLO_CORE_ASSERT(Tag != ETaskTag::EParallelThread, "Parallel cannot be used on its own");

        TagOnlyIfNone = true; // Now defaults to always optional

        if (s_ActiveTaskTag == ETaskTag::EStaticInit)
        {
            // Allow overwrite only from EStaticInit -> EGameThread
            TagOnlyIfNone = Tag != ETaskTag::EGameThread;

            OLO_CORE_ASSERT(Tag == ETaskTag::EGameThread, "The Gamethread can only be tagged on the initial thread of the application");
        }

        // Check for uniqueness of named thread tags (non-parallel)
        if (!EnumHasAllFlags(Tag, ETaskTag::EParallelThread))
        {
            ETaskTag NamedThreadBits = (Tag & ETaskTag::ENamedThreadBits);
            static_assert(sizeof(ETaskTag) == sizeof(i32), "EnumSize must match interlockedOr");
            ETaskTag OldTag = ETaskTag(s_ActiveNamedThreads.fetch_or(static_cast<i32>(NamedThreadBits)));
            bool IsOK = (OldTag & NamedThreadBits) == ETaskTag::ENone;
            if (!IsOK)
            {
                s_ActiveNamedThreads.store(static_cast<i32>(ETaskTag::ENone));
            }
            OLO_CORE_ASSERT(IsOK, "Only Scopes tagged with ETaskTag::EParallelThread can be tagged multiple times...");
        }

        ParentTag = s_ActiveTaskTag;
        if (!TagOnlyIfNone || s_ActiveTaskTag == ETaskTag::ENone || s_ActiveTaskTag == ETaskTag::EWorkerThread)
        {
            s_ActiveTaskTag = Tag;
        }
        else if (TagOnlyIfNone && s_ActiveTaskTag != Tag)
        {
            // Validation for parallel tags
            if (EnumHasAllFlags(Tag, ETaskTag::EParallelRenderingThread))
            {
                OLO_CORE_ASSERT(IsInRenderingThread(), "ETaskTag::EParallelRenderingThread can only be retagged if in a parallel for on the RenderingThread...");
            }

            if (EnumHasAllFlags(Tag, ETaskTag::EParallelGameThread))
            {
                OLO_CORE_ASSERT(IsInGameThread(), "ETaskTag::EParallelGameThread can only be retagged if in a parallel for on the GameThread...");
            }
        }
    }

    FTaskTagScope::~FTaskTagScope()
    {
        OLO_CORE_ASSERT(TagOnlyIfNone || s_ActiveTaskTag == Tag, "ActiveTaskTag corrupted");

        if (!TagOnlyIfNone || ParentTag == ETaskTag::ENone || ParentTag == ETaskTag::EWorkerThread)
        {
            s_ActiveTaskTag = ParentTag;
        }

        // Clear uniqueness tracking for non-parallel tags
        if (!EnumHasAllFlags(Tag, ETaskTag::EParallelThread))
        {
            ETaskTag NamedThreadBits = (Tag & ETaskTag::ENamedThreadBits);
            static_assert(sizeof(ETaskTag) == sizeof(i32), "EnumSize must match interlockedAnd");
            [[maybe_unused]] ETaskTag OldTag = ETaskTag(s_ActiveNamedThreads.fetch_and(static_cast<i32>(~static_cast<i32>(NamedThreadBits))));
            OLO_CORE_ASSERT((OldTag & NamedThreadBits) == NamedThreadBits, "Currently active Threads got corrupted...");
        }

        // Prolong the scope of the GT for static variable destructors
        if (Tag == ETaskTag::EGameThread && s_ActiveTaskTag == ETaskTag::EStaticInit)
        {
            s_ActiveTaskTag = ETaskTag::EGameThread;
        }
    }

    bool FTaskTagScope::IsCurrentTag(ETaskTag InTag)
    {
        return s_ActiveTaskTag == InTag;
    }

    ETaskTag FTaskTagScope::GetCurrentTag()
    {
        return s_ActiveTaskTag;
    }

    bool FTaskTagScope::IsRunningDuringStaticInit()
    {
        return s_ActiveTaskTag == ETaskTag::EStaticInit && static_cast<i32>(FPlatformTLS::GetCurrentThreadId()) == GetStaticThreadId();
    }

    //////////////////////////////////////////////////////////////////////////
    // Thread query functions

    bool IsInGameThread()
    {
        if (GIsGameThreadIdInitialized)
        {
            return FTaskTagScope::IsCurrentTag(ETaskTag::EGameThread) || FTaskTagScope::IsRunningDuringStaticInit();
        }
        return true;
    }

    bool IsInParallelGameThread()
    {
        return FTaskTagScope::IsCurrentTag(ETaskTag::EParallelGameThread);
    }

    bool IsInActualRenderingThread()
    {
        return FTaskTagScope::IsCurrentTag(ETaskTag::ERenderingThread);
    }

    bool IsInRenderingThread()
    {
        if (GRenderThreadId == 0)
        {
            // No separate render thread - game thread handles rendering
            return FTaskTagScope::IsCurrentTag(ETaskTag::EGameThread) || FTaskTagScope::IsCurrentTag(ETaskTag::ERenderingThread) || FTaskTagScope::IsRunningDuringStaticInit();
        }
        else
        {
            return FTaskTagScope::IsCurrentTag(ETaskTag::ERenderingThread);
        }
    }

    bool IsInAnyRenderingThread()
    {
        return FTaskTagScope::IsCurrentTag(ETaskTag::EParallelRenderingThread) || FTaskTagScope::IsCurrentTag(ETaskTag::ERenderingThread) || FTaskTagScope::IsCurrentTag(ETaskTag::EParallelRhiThread) || FTaskTagScope::IsCurrentTag(ETaskTag::ERhiThread);
    }

    bool IsInParallelRenderingThread()
    {
        if (GRenderThreadId == 0)
        {
            // No separate render thread
            return FTaskTagScope::IsCurrentTag(ETaskTag::ERenderingThread) || FTaskTagScope::IsCurrentTag(ETaskTag::EGameThread) || FTaskTagScope::IsCurrentTag(ETaskTag::EParallelRenderingThread);
        }
        else
        {
            return FTaskTagScope::IsCurrentTag(ETaskTag::EParallelRenderingThread) || FTaskTagScope::IsCurrentTag(ETaskTag::ERenderingThread) || FTaskTagScope::IsCurrentTag(ETaskTag::EParallelRhiThread) || FTaskTagScope::IsCurrentTag(ETaskTag::ERhiThread);
        }
    }

    bool IsRHIThreadRunning()
    {
        return GIsRunningRHIInDedicatedThread;
    }

    bool IsInRHIThread()
    {
        return FTaskTagScope::IsCurrentTag(ETaskTag::ERhiThread);
    }

    bool IsInParallelRHIThread()
    {
        return (FTaskTagScope::GetCurrentTag() & ETaskTag::ERhiThread) == ETaskTag::ERhiThread;
    }

    bool IsInSlateThread()
    {
        return FTaskTagScope::IsCurrentTag(ETaskTag::ESlateThread);
    }

    bool IsInWorkerThread()
    {
        return OloEngine::LowLevelTasks::FScheduler::Get().IsWorkerThread();
    }

    bool IsInActualLoadingThread()
    {
        return FTaskTagScope::IsCurrentTag(ETaskTag::EAsyncLoadingThread) && !IsInGameThread();
    }

    bool IsInParallelLoadingThread()
    {
        return FTaskTagScope::IsCurrentTag(ETaskTag::EParallelLoadingThread);
    }

} // namespace OLO
