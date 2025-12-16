// InheritedContext.h - Task context inheritance for memory tagging and tracing
// Ported from UE5.7 Async/InheritedContext.h

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/TaskTag.h"

#include <optional>
#include <utility>

/**
 * @file InheritedContext.h
 * @brief Extends inherited context to cover async execution
 * 
 * UE5.7 uses InheritedContext to capture and restore:
 * - LLM (Low-Level Memory) tracker tags for memory attribution
 * - Memory trace tags for profiler memory tracking
 * - Metadata trace IDs for call stack attribution
 * 
 * OloEngine uses Tracy for profiling which handles most of this automatically.
 * This implementation provides:
 * - Task tags (thread type identification)
 * - Stub structure for future LLM integration
 * - Stub structure for memory/metadata trace integration
 */

// ============================================================================
// Configuration - Enable/disable inherited context features
// ============================================================================

// LLM (Low-Level Memory) tracker - currently stubbed for future implementation
#ifndef OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
    #define OLO_ENABLE_LOW_LEVEL_MEM_TRACKER 0
#endif

// Memory trace for profiler attribution
#ifndef OLO_MEMORY_TAGS_TRACE_ENABLED
    #if OLO_PROFILE && TRACY_ENABLE
        #define OLO_MEMORY_TAGS_TRACE_ENABLED 1
    #else
        #define OLO_MEMORY_TAGS_TRACE_ENABLED 0
    #endif
#endif

// Metadata trace for call stack capture
#ifndef OLO_TRACE_METADATA_ENABLED
    #if OLO_PROFILE && TRACY_ENABLE
        #define OLO_TRACE_METADATA_ENABLED 1
    #else
        #define OLO_TRACE_METADATA_ENABLED 0
    #endif
#endif

namespace OloEngine
{
    // ============================================================================
    // LLM Tag Capture (stubbed for future implementation)
    // ============================================================================

#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
    /**
     * @enum ELLMTagSet
     * @brief Tag sets for LLM tracking (matches UE5.7's ELLMTagSet)
     */
    enum class ELLMTagSet : u32
    {
        None = 0,
        Assets,
        AssetClasses,
        Max
    };

    /**
     * @struct FLLMActiveTagsCapture
     * @brief Structure representing captured LLM Tags
     * 
     * UE5.7 captures pointers to active tag data. OloEngine uses a simplified
     * version that stores tag indices for future integration.
     */
    struct FLLMActiveTagsCapture
    {
        // Tag data for each tag set (simplified - stores tag indices)
        i32 LLMTags[static_cast<u32>(ELLMTagSet::Max)] = {};

        void CaptureActiveTagData()
        {
            // TODO: When LLM is implemented, capture active tag data
            for (u32 TagSetIndex = 0; TagSetIndex < static_cast<u32>(ELLMTagSet::Max); ++TagSetIndex)
            {
                LLMTags[TagSetIndex] = -1; // Invalid tag
            }
        }

        static FLLMActiveTagsCapture Current()
        {
            FLLMActiveTagsCapture Capture;
            Capture.CaptureActiveTagData();
            return Capture;
        }
    };

    /**
     * @struct FLLMActiveTagsScope
     * @brief RAII scope for restoring LLM tags
     */
    struct FLLMActiveTagsScope
    {
        explicit FLLMActiveTagsScope([[maybe_unused]] const FLLMActiveTagsCapture& InActiveTagsCapture)
        {
            // TODO: When LLM is implemented, restore captured tags
        }

        ~FLLMActiveTagsScope()
        {
            // TODO: Restore previous tags
        }
    };
#endif // OLO_ENABLE_LOW_LEVEL_MEM_TRACKER

    // ============================================================================
    // Memory Trace Scope (stubbed for future implementation)
    // ============================================================================

#if OLO_MEMORY_TAGS_TRACE_ENABLED
    /**
     * @brief Get the active memory trace tag
     * @return Current memory tag ID
     */
    inline i32 MemoryTrace_GetActiveTag()
    {
        // TODO: Integrate with Tracy memory tracking
        return 0;
    }

    /**
     * @struct FMemScope
     * @brief RAII scope for memory trace tags
     */
    struct FMemScope
    {
        explicit FMemScope([[maybe_unused]] i32 InMemTag)
            : m_PreviousTag(MemoryTrace_GetActiveTag())
            , m_bActive(true)
        {
            // TODO: Set active tag when memory tracing is implemented
            (void)m_PreviousTag;
        }

        ~FMemScope()
        {
            if (m_bActive)
            {
                // TODO: Restore previous tag
            }
        }

    private:
        i32 m_PreviousTag = 0;
        bool m_bActive = false;
    };
#endif // OLO_MEMORY_TAGS_TRACE_ENABLED

    // ============================================================================
    // Metadata Trace Scope (stubbed for future implementation)
    // ============================================================================

#if OLO_TRACE_METADATA_ENABLED
    /**
     * @brief Capture current trace metadata (call stack ID)
     * @return Metadata ID representing current call stack
     */
    inline u32 TraceMetadata_SaveStack()
    {
        // TODO: Integrate with Tracy call stack capture
        return 0;
    }

    /**
     * @struct FMetadataRestoreScope
     * @brief RAII scope for restoring trace metadata
     */
    struct FMetadataRestoreScope
    {
        explicit FMetadataRestoreScope([[maybe_unused]] u32 InMetadataId)
            : m_MetadataId(InMetadataId)
        {
            // TODO: Restore captured call stack when tracing is implemented
        }

        ~FMetadataRestoreScope()
        {
            // TODO: Cleanup
        }

    private:
        u32 m_MetadataId = 0;
    };
#endif // OLO_TRACE_METADATA_ENABLED

    // ============================================================================
    // FInheritedContextScope - RAII scope for restoring inherited context
    // ============================================================================

    /**
     * @class FInheritedContextScope
     * @brief Restores an inherited context for the current scope
     * 
     * An instance must be obtained by calling FInheritedContextBase::RestoreInheritedContext()
     * This is an RAII scope that automatically restores the captured context.
     * 
     * In OloEngine, this captures and restores:
     * - Task tags (thread type identification)
     * - LLM tags (when OLO_ENABLE_LOW_LEVEL_MEM_TRACKER is enabled)
     * - Memory trace tags (when OLO_MEMORY_TAGS_TRACE_ENABLED is enabled)
     * - Metadata trace IDs (when OLO_TRACE_METADATA_ENABLED is enabled)
     */
    class FInheritedContextScope
    {
    public:
        // Non-copyable
        FInheritedContextScope(const FInheritedContextScope&) = delete;
        FInheritedContextScope& operator=(const FInheritedContextScope&) = delete;

        // Movable - transfers ownership of context restoration
        FInheritedContextScope(FInheritedContextScope&& Other) noexcept
            : m_CapturedTag(Other.m_CapturedTag)
            , m_PreviousTag(Other.m_PreviousTag)
            , m_bOwnsContext(Other.m_bOwnsContext)
#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
            , m_LLMScopes(std::move(Other.m_LLMScopes))
#endif
#if OLO_MEMORY_TAGS_TRACE_ENABLED
            , m_MemScope(std::move(Other.m_MemScope))
#endif
#if OLO_TRACE_METADATA_ENABLED
            , m_MetaScope(std::move(Other.m_MetaScope))
#endif
        {
            Other.m_bOwnsContext = false; // Transfer ownership
        }

        FInheritedContextScope& operator=(FInheritedContextScope&& Other) noexcept
        {
            if (this != &Other)
            {
                // Restore our context before taking Other's
                if (m_bOwnsContext)
                {
                    OLO::FTaskTagScope::SwapTag(m_PreviousTag);
                }
                
                m_CapturedTag = Other.m_CapturedTag;
                m_PreviousTag = Other.m_PreviousTag;
                m_bOwnsContext = Other.m_bOwnsContext;
                Other.m_bOwnsContext = false;

#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
                m_LLMScopes = std::move(Other.m_LLMScopes);
#endif
#if OLO_MEMORY_TAGS_TRACE_ENABLED
                m_MemScope = std::move(Other.m_MemScope);
#endif
#if OLO_TRACE_METADATA_ENABLED
                m_MetaScope = std::move(Other.m_MetaScope);
#endif
            }
            return *this;
        }

        ~FInheritedContextScope()
        {
            // Restore the previous context when this scope ends
            if (m_bOwnsContext)
            {
                OLO::FTaskTagScope::SwapTag(m_PreviousTag);
            }
            // LLMScopes, MemScope, MetaScope destructors handle their own cleanup
        }

    private:
        friend class FInheritedContextBase;

        // Full constructor with all context types (used by FInheritedContextBase)
        FInheritedContextScope(
            OLO::ETaskTag InCapturedTag,
            bool bHasCapturedContext
#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
            , const FLLMActiveTagsCapture& InLLMTags
#endif
#if OLO_MEMORY_TAGS_TRACE_ENABLED
            , i32 InMemTag
#endif
#if OLO_TRACE_METADATA_ENABLED
            , u32 InMetadataId
#endif
        )
            : m_CapturedTag(InCapturedTag)
            , m_PreviousTag(OLO::ETaskTag::ENone)
            , m_bOwnsContext(bHasCapturedContext)
#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
            , m_LLMScopes(InLLMTags)
#endif
#if OLO_MEMORY_TAGS_TRACE_ENABLED
            , m_MemScope(InMemTag)
#endif
#if OLO_TRACE_METADATA_ENABLED
            , m_MetaScope(InMetadataId)
#endif
        {
            if (m_bOwnsContext)
            {
                // Apply the captured context and save the current one for restoration
                m_PreviousTag = OLO::FTaskTagScope::SwapTag(m_CapturedTag);
            }
        }

        // Task tag state
        OLO::ETaskTag m_CapturedTag = OLO::ETaskTag::ENone;
        OLO::ETaskTag m_PreviousTag = OLO::ETaskTag::ENone;
        bool m_bOwnsContext = false;

        // LLM scope restoration
#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
        std::optional<FLLMActiveTagsScope> m_LLMScopes;
#endif

        // Memory trace scope restoration
#if OLO_MEMORY_TAGS_TRACE_ENABLED
        std::optional<FMemScope> m_MemScope;
#endif

        // Metadata trace scope restoration
#if OLO_TRACE_METADATA_ENABLED
        std::optional<FMetadataRestoreScope> m_MetaScope;
#endif
    };

    // ============================================================================
    // FInheritedContextBase - Base class for capturing/restoring context
    // ============================================================================

    /**
     * @class FInheritedContextBase
     * @brief Base class for capturing and restoring task execution context
     * 
     * This class extends inherited context (memory tags, profiling metadata) to cover
     * async execution. It is intended to be used as a base class for task implementations.
     * 
     * When profiling/tracing is compiled out, this class has minimal overhead.
     * 
     * Usage:
     * @code
     * class FMyTask : public FInheritedContextBase
     * {
     * public:
     *     void Launch()
     *     {
     *         CaptureInheritedContext(); // Capture at launch site
     *     }
     *     
     *     void Execute()
     *     {
     *         FInheritedContextScope Scope = RestoreInheritedContext();
     *         // Context is now restored for this execution
     *         DoWork();
     *     }
     * };
     * @endcode
     * 
     * Key integrations:
     * - Task Tags: Captures which thread type spawned the task
     * - LLM: Captures active Low-Level Memory tracker tags for attribution
     * - Memory Trace: Captures memory tag for profiler
     * - Metadata Trace: Captures call stack ID for attribution
     */
    class FInheritedContextBase
    {
    public:
        FInheritedContextBase() = default;

        /**
         * @brief Capture the current thread's context for later restoration
         * 
         * Must be called in the inherited context, e.g., on launching an async task.
         * This captures memory tags, trace IDs, and other profiling metadata.
         */
        void CaptureInheritedContext()
        {
            // Capture the current thread's task tag
            m_CapturedTaskTag = OLO::FTaskTagScope::GetCurrentTag();
            m_bContextCaptured = true;
            
            // Capture LLM tags if enabled
#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
            m_InheritedLLMTags.CaptureActiveTagData();
#endif

            // Capture memory trace tag if enabled
#if OLO_MEMORY_TAGS_TRACE_ENABLED
            m_InheritedMemTag = MemoryTrace_GetActiveTag();
#endif

            // Capture metadata trace ID if enabled
#if OLO_TRACE_METADATA_ENABLED
            m_InheritedMetadataId = TraceMetadata_SaveStack();
#endif
        }

        /**
         * @brief Restore the captured context for the current scope
         * 
         * Must be called where the inherited context should be restored,
         * e.g., at the start of async task execution.
         * 
         * @return RAII scope that restores the context until it goes out of scope
         */
        [[nodiscard]] FInheritedContextScope RestoreInheritedContext()
        {
            return FInheritedContextScope(
                m_CapturedTaskTag,
                m_bContextCaptured
#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
                , m_InheritedLLMTags
#endif
#if OLO_MEMORY_TAGS_TRACE_ENABLED
                , m_InheritedMemTag
#endif
#if OLO_TRACE_METADATA_ENABLED
                , m_InheritedMetadataId
#endif
            );
        }

        /**
         * @brief Check if context was captured
         * @return true if CaptureInheritedContext() was called
         */
        bool HasCapturedContext() const { return m_bContextCaptured; }

        /**
         * @brief Get the captured task tag
         * @return The task tag that was active when context was captured
         */
        OLO::ETaskTag GetCapturedTaskTag() const { return m_CapturedTaskTag; }

    private:
        // Captured task tag (which thread type created this task)
        OLO::ETaskTag m_CapturedTaskTag = OLO::ETaskTag::ENone;
        
        // Whether context has been captured
        bool m_bContextCaptured = false;

        // LLM (Low-Level Memory) tags for memory attribution
#if OLO_ENABLE_LOW_LEVEL_MEM_TRACKER
        FLLMActiveTagsCapture m_InheritedLLMTags;
#endif

        // Memory trace tag for profiler
#if OLO_MEMORY_TAGS_TRACE_ENABLED
        i32 m_InheritedMemTag = 0;
#endif

        // Metadata trace ID for call stack attribution
#if OLO_TRACE_METADATA_ENABLED
        u32 m_InheritedMetadataId = 0;
#endif
    };

} // namespace OloEngine
