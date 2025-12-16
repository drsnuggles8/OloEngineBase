// LowLevelMemTracker.cpp - Low-Level Memory Tracker implementation
// Ported from UE5.7 HAL/LowLevelMemTracker.cpp

#include "OloEngine/HAL/LowLevelMemTracker.h"
#include "OloEngine/Core/Log.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace OloEngine
{

// ============================================================================
// Tag Name Lookup
// ============================================================================

const char* LLMGetTagNameANSI(ELLMTag Tag)
{
    switch (Tag)
    {
#define LLM_ENUM(Enum, Str, Parent) case ELLMTag::Enum: return Str;
        LLM_ENUM_GENERIC_TAGS(LLM_ENUM)
#undef LLM_ENUM
    default:
        return "Unknown";
    }
}

#if ENABLE_LOW_LEVEL_MEM_TRACKER

// ============================================================================
// Thread-Local Storage
// ============================================================================

static thread_local LLMPrivate::FLLMThreadState* s_ThreadState = nullptr;

// ============================================================================
// Allocation Tracking Map
// ============================================================================

namespace LLMPrivate
{

/**
 * @class FAllocationMap
 * @brief Thread-safe map tracking allocation sizes by pointer
 */
class FAllocationMap
{
public:
    struct FAllocationInfo
    {
        u64 Size = 0;
        ELLMTag Tag = ELLMTag::Untagged;
    };

    void Add(const void* Ptr, u64 Size, ELLMTag Tag)
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        m_Map[Ptr] = { Size, Tag };
    }

    bool Remove(const void* Ptr, FAllocationInfo& OutInfo)
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        auto It = m_Map.find(Ptr);
        if (It != m_Map.end())
        {
            OutInfo = It->second;
            m_Map.erase(It);
            return true;
        }
        return false;
    }

    u64 GetTotalSize() const
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        u64 Total = 0;
        for (const auto& Pair : m_Map)
        {
            Total += Pair.second.Size;
        }
        return Total;
    }

private:
    mutable std::mutex m_Mutex;
    std::unordered_map<const void*, FAllocationInfo> m_Map;
};

static FAllocationMap s_AllocationMap;

// ============================================================================
// FLLMThreadState Implementation
// ============================================================================

FLLMThreadState::FLLMThreadState()
{
    // Initialize stack with Untagged
    m_TagStack[0] = ELLMTag::Untagged;
    m_TagStackDepth = 1;
}

FLLMThreadState::~FLLMThreadState() = default;

void FLLMThreadState::PushTag(ELLMTag Tag)
{
    if (m_TagStackDepth < MaxTagStackDepth)
    {
        m_TagStack[m_TagStackDepth++] = Tag;
    }
}

void FLLMThreadState::PopTag()
{
    if (m_TagStackDepth > 1)
    {
        --m_TagStackDepth;
    }
}

ELLMTag FLLMThreadState::GetCurrentTag() const
{
    return m_TagStack[m_TagStackDepth - 1];
}

} // namespace LLMPrivate

// ============================================================================
// FLowLevelMemTracker Implementation
// ============================================================================

FLowLevelMemTracker& FLowLevelMemTracker::Get()
{
    static FLowLevelMemTracker Instance;
    return Instance;
}

bool FLowLevelMemTracker::IsEnabled()
{
    return Get().m_bEnabled.load(std::memory_order_acquire) && 
           Get().m_bInitialized.load(std::memory_order_acquire);
}

FLowLevelMemTracker::FLowLevelMemTracker()
{
    Initialize();
}

FLowLevelMemTracker::~FLowLevelMemTracker()
{
    Shutdown();
}

void FLowLevelMemTracker::Initialize()
{
    if (m_bInitialized.load(std::memory_order_acquire))
    {
        return;
    }

    InitializeTagData();
    
    m_bInitialized.store(true, std::memory_order_release);
    m_bEnabled.store(true, std::memory_order_release);

    OLO_CORE_INFO("LLM: Low-Level Memory Tracker initialized");
}

void FLowLevelMemTracker::Shutdown()
{
    if (!m_bInitialized.load(std::memory_order_acquire))
    {
        return;
    }

    m_bEnabled.store(false, std::memory_order_release);
    m_bInitialized.store(false, std::memory_order_release);

    OLO_CORE_INFO("LLM: Low-Level Memory Tracker shutdown");
}

void FLowLevelMemTracker::InitializeTagData()
{
    // Initialize all tag data
    for (u32 i = 0; i < LLM_TAG_COUNT; ++i)
    {
        m_TagData[i].Tag = static_cast<ELLMTag>(i);
        m_TagData[i].ParentIndex = -1;
        m_TagData[i].CurrentSize.store(0, std::memory_order_relaxed);
        m_TagData[i].PeakSize.store(0, std::memory_order_relaxed);
    }

    // Set names and parent relationships
#define LLM_ENUM(Enum, Str, Parent) \
    m_TagData[static_cast<u8>(ELLMTag::Enum)].Name = Str; \
    if constexpr (static_cast<i32>(Parent) >= 0) { \
        m_TagData[static_cast<u8>(ELLMTag::Enum)].ParentIndex = static_cast<i32>(Parent); \
    }
    LLM_ENUM_GENERIC_TAGS(LLM_ENUM)
#undef LLM_ENUM
}

LLMPrivate::FLLMThreadState& FLowLevelMemTracker::GetThreadState()
{
    if (!s_ThreadState)
    {
        s_ThreadState = new LLMPrivate::FLLMThreadState();
    }
    return *s_ThreadState;
}

void FLowLevelMemTracker::OnLowLevelAlloc(ELLMTracker Tracker, const void* Ptr, u64 Size, 
                                          ELLMTag DefaultTag, ELLMAllocType AllocType)
{
    if (!IsEnabled() || Ptr == nullptr || Size == 0)
    {
        return;
    }

    auto& ThreadState = GetThreadState();
    if (ThreadState.IsPaused())
    {
        return;
    }

    // Get the tag from the current scope, or use default
    ELLMTag Tag = ThreadState.GetCurrentTag();
    if (Tag == ELLMTag::Untagged && DefaultTag != ELLMTag::Untagged)
    {
        Tag = DefaultTag;
    }

    // Track the allocation
    LLMPrivate::s_AllocationMap.Add(Ptr, Size, Tag);

    // Update tag sizes
    u8 TagIndex = static_cast<u8>(Tag);
    i64 NewSize = m_TagData[TagIndex].CurrentSize.fetch_add(static_cast<i64>(Size), std::memory_order_relaxed) + static_cast<i64>(Size);
    
    // Update peak if needed
    i64 CurrentPeak = m_TagData[TagIndex].PeakSize.load(std::memory_order_relaxed);
    while (NewSize > CurrentPeak)
    {
        if (m_TagData[TagIndex].PeakSize.compare_exchange_weak(CurrentPeak, NewSize, std::memory_order_relaxed))
        {
            break;
        }
    }

    // Also update parent tags
    i32 ParentIndex = m_TagData[TagIndex].ParentIndex;
    while (ParentIndex >= 0 && ParentIndex < static_cast<i32>(LLM_TAG_COUNT))
    {
        i64 ParentNewSize = m_TagData[ParentIndex].CurrentSize.fetch_add(static_cast<i64>(Size), std::memory_order_relaxed) + static_cast<i64>(Size);
        
        i64 ParentPeak = m_TagData[ParentIndex].PeakSize.load(std::memory_order_relaxed);
        while (ParentNewSize > ParentPeak)
        {
            if (m_TagData[ParentIndex].PeakSize.compare_exchange_weak(ParentPeak, ParentNewSize, std::memory_order_relaxed))
            {
                break;
            }
        }

        ParentIndex = m_TagData[ParentIndex].ParentIndex;
    }

    // Update total
    u8 TotalIndex = static_cast<u8>(ELLMTag::Total);
    i64 TotalNewSize = m_TagData[TotalIndex].CurrentSize.fetch_add(static_cast<i64>(Size), std::memory_order_relaxed) + static_cast<i64>(Size);
    
    i64 TotalPeak = m_TagData[TotalIndex].PeakSize.load(std::memory_order_relaxed);
    while (TotalNewSize > TotalPeak)
    {
        if (m_TagData[TotalIndex].PeakSize.compare_exchange_weak(TotalPeak, TotalNewSize, std::memory_order_relaxed))
        {
            break;
        }
    }
}

void FLowLevelMemTracker::OnLowLevelFree(ELLMTracker Tracker, const void* Ptr)
{
    if (!IsEnabled() || Ptr == nullptr)
    {
        return;
    }

    LLMPrivate::FAllocationMap::FAllocationInfo Info;
    if (!LLMPrivate::s_AllocationMap.Remove(Ptr, Info))
    {
        return;
    }

    // Update tag sizes
    u8 TagIndex = static_cast<u8>(Info.Tag);
    m_TagData[TagIndex].CurrentSize.fetch_sub(static_cast<i64>(Info.Size), std::memory_order_relaxed);

    // Also update parent tags
    i32 ParentIndex = m_TagData[TagIndex].ParentIndex;
    while (ParentIndex >= 0 && ParentIndex < static_cast<i32>(LLM_TAG_COUNT))
    {
        m_TagData[ParentIndex].CurrentSize.fetch_sub(static_cast<i64>(Info.Size), std::memory_order_relaxed);
        ParentIndex = m_TagData[ParentIndex].ParentIndex;
    }

    // Update total
    u8 TotalIndex = static_cast<u8>(ELLMTag::Total);
    m_TagData[TotalIndex].CurrentSize.fetch_sub(static_cast<i64>(Info.Size), std::memory_order_relaxed);
}

i64 FLowLevelMemTracker::GetTagSize(ELLMTag Tag) const
{
    u8 Index = static_cast<u8>(Tag);
    if (Index < LLM_TAG_COUNT)
    {
        return m_TagData[Index].CurrentSize.load(std::memory_order_relaxed);
    }
    return 0;
}

i64 FLowLevelMemTracker::GetTagPeakSize(ELLMTag Tag) const
{
    u8 Index = static_cast<u8>(Tag);
    if (Index < LLM_TAG_COUNT)
    {
        return m_TagData[Index].PeakSize.load(std::memory_order_relaxed);
    }
    return 0;
}

void FLowLevelMemTracker::GetAllTagSizes(i64* OutSizes, u32 MaxTags) const
{
    u32 Count = (MaxTags < LLM_TAG_COUNT) ? MaxTags : LLM_TAG_COUNT;
    for (u32 i = 0; i < Count; ++i)
    {
        OutSizes[i] = m_TagData[i].CurrentSize.load(std::memory_order_relaxed);
    }
}

void FLowLevelMemTracker::DumpToLog()
{
    OLO_CORE_INFO("=== LLM Memory Report ===");
    OLO_CORE_INFO("Total tracked: {} bytes", GetTagSize(ELLMTag::Total));
    
    for (u32 i = 0; i < static_cast<u32>(ELLMTag::GenericTagCount); ++i)
    {
        i64 Size = m_TagData[i].CurrentSize.load(std::memory_order_relaxed);
        i64 Peak = m_TagData[i].PeakSize.load(std::memory_order_relaxed);
        
        if (Size > 0 || Peak > 0)
        {
            const char* Name = m_TagData[i].Name ? m_TagData[i].Name : "Unknown";
            OLO_CORE_INFO("  {}: {} bytes (peak: {} bytes)", Name, Size, Peak);
        }
    }
    
    OLO_CORE_INFO("=========================");
}

// ============================================================================
// FLLMScope Implementation
// ============================================================================

FLLMScope::FLLMScope(ELLMTag Tag, bool bIsStatTag, ELLMTagSet TagSet, ELLMTracker Tracker)
    : m_Tag(Tag)
    , m_bEnabled(FLowLevelMemTracker::IsEnabled())
{
    if (m_bEnabled)
    {
        FLowLevelMemTracker::Get().GetThreadState().PushTag(Tag);
    }
}

FLLMScope::~FLLMScope()
{
    if (m_bEnabled)
    {
        FLowLevelMemTracker::Get().GetThreadState().PopTag();
    }
}

// ============================================================================
// FLLMPauseScope Implementation
// ============================================================================

FLLMPauseScope::FLLMPauseScope()
    : m_bEnabled(FLowLevelMemTracker::IsEnabled())
{
    if (m_bEnabled)
    {
        FLowLevelMemTracker::Get().GetThreadState().Pause();
    }
}

FLLMPauseScope::~FLLMPauseScope()
{
    if (m_bEnabled)
    {
        FLowLevelMemTracker::Get().GetThreadState().Unpause();
    }
}

#endif // ENABLE_LOW_LEVEL_MEM_TRACKER

} // namespace OloEngine
