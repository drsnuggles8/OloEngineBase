// LowLevelMemTracker.h - Low-Level Memory Tracker for OloEngine
// Ported from UE5.7 HAL/LowLevelMemTracker.h

#pragma once

/**
 * @file LowLevelMemTracker.h
 * @brief Low-Level Memory Tracker (LLM) for tracking all memory allocations
 * 
 * LLM provides detailed per-allocation memory tracking with hierarchical tags.
 * It operates at the lowest level of memory allocation (before malloc wrappers)
 * to ensure accurate tracking of all memory usage.
 * 
 * Key Features:
 * - Per-allocation tagging with hierarchical categories
 * - Thread-local scope tracking with automatic inheritance
 * - Multiple trackers (Platform, Default) for different allocation sources
 * - Peak memory tracking per tag
 * - CSV export for detailed analysis
 * 
 * Usage:
 * @code
 *     // Simple scope tagging
 *     LLM_SCOPE(ELLMTag::Textures);
 *     void* ptr = Allocate(1024);  // Allocation tracked under Textures
 *     
 *     // Nested scopes
 *     LLM_SCOPE(ELLMTag::Audio);
 *     {
 *         LLM_SCOPE(ELLMTag::AudioMixer);
 *         // Allocations here tracked under AudioMixer (child of Audio)
 *     }
 * @endcode
 */

#include "OloEngine/Core/Base.h"
#include "OloEngine/HAL/LowLevelMemTrackerDefines.h"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace OloEngine
{

// ============================================================================
// LLM Tag Type
// ============================================================================

#define LLM_TAG_TYPE u8

// ============================================================================
// LLM Trackers
// ============================================================================

/**
 * @enum ELLMTracker
 * @brief Identifies which tracker an allocation belongs to
 */
enum class ELLMTracker : u8
{
    Platform,   ///< Platform-level allocations (OS, graphics API)
    Default,    ///< Default engine/game allocations
    Max
};

// ============================================================================
// LLM Tag Sets
// ============================================================================

/**
 * @enum ELLMTagSet
 * @brief Optional tag sets that can be enabled at runtime
 */
enum class ELLMTagSet : u8
{
    None,           ///< Default tag set (always active)
    Assets,         ///< Per-asset tracking
    AssetClasses,   ///< Per-asset-class tracking
    Max
};

// ============================================================================
// LLM Allocation Type
// ============================================================================

/**
 * @enum ELLMAllocType
 * @brief Type of allocation being tracked
 */
enum class ELLMAllocType : u8
{
    None = 0,
    FMalloc,    ///< Standard malloc allocations
    System,     ///< System/OS allocations
    RHI,        ///< Graphics/RHI allocations
    Count
};

// ============================================================================
// Size Parameters
// ============================================================================

namespace LLM
{

/**
 * @enum ESizeParams
 * @brief Flags for size query operations
 */
enum class ESizeParams : u8
{
    Default = 0,
    ReportCurrent = 0,          ///< Report current memory usage
    ReportPeak = 1,             ///< Report peak memory usage
    RelativeToSnapshot = 2      ///< Report relative to last snapshot
};

} // namespace LLM

// ============================================================================
// LLM Tags Enumeration
// ============================================================================

// Macro to generate tag declarations
#define LLM_ENUM_GENERIC_TAGS(macro) \
    macro(Untagged,             "Untagged",         -1) \
    macro(Paused,               "Paused",           -1) \
    macro(Total,                "Total",            -1) \
    macro(Untracked,            "Untracked",        -1) \
    macro(TrackedTotal,         "TrackedTotal",     -1) \
    macro(PlatformTotal,        "PlatformTotal",    -1) \
    macro(EngineMisc,           "EngineMisc",       -1) \
    macro(TaskGraphTasks,       "TaskGraphTasks",   -1) \
    macro(Audio,                "Audio",            -1) \
    macro(AudioMixer,           "AudioMixer",       ELLMTag::Audio) \
    macro(FName,                "FName",            -1) \
    macro(Networking,           "Networking",       -1) \
    macro(Meshes,               "Meshes",           -1) \
    macro(Shaders,              "Shaders",          -1) \
    macro(Textures,             "Textures",         -1) \
    macro(RenderTargets,        "RenderTargets",    -1) \
    macro(Physics,              "Physics",          -1) \
    macro(PhysX,                "PhysX",            ELLMTag::Physics) \
    macro(Jolt,                 "Jolt",             ELLMTag::Physics) \
    macro(UI,                   "UI",               -1) \
    macro(Animation,            "Animation",        -1) \
    macro(StaticMesh,           "StaticMesh",       ELLMTag::Meshes) \
    macro(SkeletalMesh,         "SkeletalMesh",     ELLMTag::Meshes) \
    macro(Materials,            "Materials",        -1) \
    macro(Particles,            "Particles",        -1) \
    macro(GC,                   "GC",               -1) \
    macro(AsyncLoading,         "AsyncLoading",     -1) \
    macro(FileSystem,           "FileSystem",       -1) \
    macro(Scripting,            "Scripting",        -1) \
    macro(ScriptingMono,        "ScriptingMono",    ELLMTag::Scripting) \
    macro(ScriptingLua,         "ScriptingLua",     ELLMTag::Scripting) \
    macro(ECS,                  "ECS",              -1) \
    macro(Scene,                "Scene",            -1) \
    macro(Rendering,            "Rendering",        -1) \
    macro(LinearAllocator,      "LinearAllocator",  -1) \
    macro(MemStack,             "MemStack",         -1) \

/**
 * @enum ELLMTag
 * @brief Enumeration of all built-in LLM tags
 */
enum class ELLMTag : LLM_TAG_TYPE
{
#define LLM_ENUM(Enum, Str, Parent) Enum,
    LLM_ENUM_GENERIC_TAGS(LLM_ENUM)
#undef LLM_ENUM

    GenericTagCount,

    // Platform tags range
    PlatformTagStart = 100,
    PlatformTagEnd = 149,

    // Project-specific tags range
    ProjectTagStart = 150,
    ProjectTagEnd = 255,
};

static_assert(static_cast<u8>(ELLMTag::GenericTagCount) <= static_cast<u8>(ELLMTag::PlatformTagStart),
    "Too many LLM tags defined!");

constexpr u32 LLM_TAG_COUNT = 256;
constexpr u32 LLM_CUSTOM_TAG_START = static_cast<u32>(ELLMTag::PlatformTagStart);
constexpr u32 LLM_CUSTOM_TAG_END = static_cast<u32>(ELLMTag::ProjectTagEnd);
constexpr u32 LLM_CUSTOM_TAG_COUNT = LLM_CUSTOM_TAG_END + 1 - LLM_CUSTOM_TAG_START;

// ============================================================================
// LLM Tag Info
// ============================================================================

/**
 * @brief Get the display name for a tag
 */
const char* LLMGetTagNameANSI(ELLMTag Tag);

#if ENABLE_LOW_LEVEL_MEM_TRACKER

// ============================================================================
// Forward Declarations
// ============================================================================

class FLowLevelMemTracker;
class FLLMScope;
class FLLMPauseScope;

// ============================================================================
// LLM Tag Data
// ============================================================================

namespace LLMPrivate
{

/**
 * @struct FTagData
 * @brief Internal data structure for a tracked tag
 */
struct FTagData
{
    const char* Name = nullptr;
    ELLMTag Tag = ELLMTag::Untagged;
    i32 ParentIndex = -1;
    std::atomic<i64> CurrentSize{0};
    std::atomic<i64> PeakSize{0};
};

/**
 * @class FLLMThreadState
 * @brief Thread-local LLM state
 */
class FLLMThreadState
{
public:
    FLLMThreadState();
    ~FLLMThreadState();

    void PushTag(ELLMTag Tag);
    void PopTag();
    ELLMTag GetCurrentTag() const;
    bool IsPaused() const { return m_PauseCount > 0; }
    void Pause() { ++m_PauseCount; }
    void Unpause() { if (m_PauseCount > 0) --m_PauseCount; }

private:
    static constexpr u32 MaxTagStackDepth = 256;
    ELLMTag m_TagStack[MaxTagStackDepth];
    u32 m_TagStackDepth = 0;
    u32 m_PauseCount = 0;
};

} // namespace LLMPrivate

// ============================================================================
// FLowLevelMemTracker
// ============================================================================

/**
 * @class FLowLevelMemTracker
 * @brief Main LLM singleton that manages all memory tracking
 */
class FLowLevelMemTracker
{
public:
    /**
     * @brief Get the LLM singleton instance
     */
    static FLowLevelMemTracker& Get();

    /**
     * @brief Check if LLM is currently enabled
     */
    static bool IsEnabled();

    /**
     * @brief Initialize LLM (called automatically on first use)
     */
    void Initialize();

    /**
     * @brief Shutdown LLM and release all tracking data
     */
    void Shutdown();

    /**
     * @brief Called when memory is allocated
     */
    void OnLowLevelAlloc(ELLMTracker Tracker, const void* Ptr, u64 Size, 
                         ELLMTag DefaultTag = ELLMTag::Untagged,
                         ELLMAllocType AllocType = ELLMAllocType::None);

    /**
     * @brief Called when memory is freed
     */
    void OnLowLevelFree(ELLMTracker Tracker, const void* Ptr);

    /**
     * @brief Get the current size for a tag
     */
    i64 GetTagSize(ELLMTag Tag) const;

    /**
     * @brief Get the peak size for a tag
     */
    i64 GetTagPeakSize(ELLMTag Tag) const;

    /**
     * @brief Get all tag sizes as a snapshot
     */
    void GetAllTagSizes(i64* OutSizes, u32 MaxTags) const;

    /**
     * @brief Dump LLM stats to log
     */
    void DumpToLog();

    /**
     * @brief Get the thread-local state for the current thread
     */
    LLMPrivate::FLLMThreadState& GetThreadState();

    /**
     * @brief Check if LLM is fully initialized
     */
    bool IsInitialized() const { return m_bInitialized.load(std::memory_order_acquire); }

private:
    FLowLevelMemTracker();
    ~FLowLevelMemTracker();

    FLowLevelMemTracker(const FLowLevelMemTracker&) = delete;
    FLowLevelMemTracker& operator=(const FLowLevelMemTracker&) = delete;

    void InitializeTagData();

private:
    LLMPrivate::FTagData m_TagData[LLM_TAG_COUNT];
    std::atomic<bool> m_bInitialized{false};
    std::atomic<bool> m_bEnabled{true};
};

// ============================================================================
// FLLMScope - RAII scope for tracking allocations under a tag
// ============================================================================

/**
 * @class FLLMScope
 * @brief RAII scope that tracks allocations under a specific tag
 */
class FLLMScope
{
public:
    FLLMScope(ELLMTag Tag, bool bIsStatTag = false, 
              ELLMTagSet TagSet = ELLMTagSet::None,
              ELLMTracker Tracker = ELLMTracker::Default);
    ~FLLMScope();

    FLLMScope(const FLLMScope&) = delete;
    FLLMScope& operator=(const FLLMScope&) = delete;

private:
    ELLMTag m_Tag;
    bool m_bEnabled;
};

// ============================================================================
// FLLMPauseScope - RAII scope to pause LLM tracking
// ============================================================================

/**
 * @class FLLMPauseScope
 * @brief RAII scope that pauses LLM tracking
 */
class FLLMPauseScope
{
public:
    FLLMPauseScope();
    ~FLLMPauseScope();

    FLLMPauseScope(const FLLMPauseScope&) = delete;
    FLLMPauseScope& operator=(const FLLMPauseScope&) = delete;

private:
    bool m_bEnabled;
};

// ============================================================================
// LLM Macros (Enabled)
// ============================================================================

#define LLM(x) x
#define LLM_IF_ENABLED(x) if (FLowLevelMemTracker::IsEnabled()) { x; }
#define LLM_IS_ENABLED() FLowLevelMemTracker::IsEnabled()
#define LLM_SCOPE_NAME PREPROCESSOR_JOIN(LLMScope, __LINE__)

/**
 * @brief Main LLM scope macro - tracks allocations under the specified tag
 */
#define LLM_SCOPE(Tag) \
    OloEngine::FLLMScope LLM_SCOPE_NAME(Tag, false, OloEngine::ELLMTagSet::None, OloEngine::ELLMTracker::Default)

/**
 * @brief LLM scope with specific tracker
 */
#define LLM_SCOPE_BYTRACKER(Tracker, Tag) \
    OloEngine::FLLMScope LLM_SCOPE_NAME(Tag, false, OloEngine::ELLMTagSet::None, Tracker)

/**
 * @brief Pause LLM tracking in this scope
 */
#define LLM_SCOPED_PAUSE_TRACKING() \
    OloEngine::FLLMPauseScope LLM_SCOPE_NAME

/**
 * @brief Track a specific allocation
 */
#define LLM_PLATFORM_SCOPE(Tag) \
    OloEngine::FLLMScope LLM_SCOPE_NAME(Tag, false, OloEngine::ELLMTagSet::None, OloEngine::ELLMTracker::Platform)

#else // ENABLE_LOW_LEVEL_MEM_TRACKER

// ============================================================================
// LLM Macros (Disabled - No-ops)
// ============================================================================

#define LLM(x)
#define LLM_IF_ENABLED(x)
#define LLM_IS_ENABLED() false
#define LLM_SCOPE(Tag)
#define LLM_SCOPE_BYTRACKER(Tracker, Tag)
#define LLM_SCOPED_PAUSE_TRACKING()
#define LLM_PLATFORM_SCOPE(Tag)

#endif // ENABLE_LOW_LEVEL_MEM_TRACKER

} // namespace OloEngine
