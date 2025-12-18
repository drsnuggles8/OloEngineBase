// LowLevelMemTrackerDefines.h - Compile-time settings for LLM
// Ported from UE5.7 HAL/LowLevelMemTrackerDefines.h

#pragma once

/**
 * @file LowLevelMemTrackerDefines.h
 * @brief Configuration defines for the Low-Level Memory Tracker
 * 
 * This header configures compile-time settings for LLM based on build configuration.
 * LLM provides detailed memory tracking at the allocation level, useful for debugging
 * memory issues and profiling memory usage.
 */

// ============================================================================
// Build Configuration
// ============================================================================

#ifndef OLO_DIST
    #define OLO_BUILD_IS_DIST 0
#else
    #define OLO_BUILD_IS_DIST 1
#endif

#ifndef OLO_BUILD_TEST
    #define OLO_BUILD_TEST 0
#endif

#ifndef ALLOW_LOW_LEVEL_MEM_TRACKER_IN_TEST
    #define ALLOW_LOW_LEVEL_MEM_TRACKER_IN_TEST 1
#endif

// ============================================================================
// Platform Support
// ============================================================================

#ifndef PLATFORM_SUPPORTS_LLM
    #define PLATFORM_SUPPORTS_LLM 1
#endif

// ============================================================================
// LLM Enable Configuration
// ============================================================================

// LLM is enabled in non-shipping builds by default
#ifndef LLM_ENABLED_IN_CONFIG
    #define LLM_ENABLED_IN_CONFIG ( \
        !OLO_BUILD_IS_DIST && \
        (!OLO_BUILD_TEST || ALLOW_LOW_LEVEL_MEM_TRACKER_IN_TEST) && \
        PLATFORM_SUPPORTS_LLM \
    )
#endif

// Final enable flag combining config and platform support
#define ENABLE_LOW_LEVEL_MEM_TRACKER (LLM_ENABLED_IN_CONFIG && PLATFORM_SUPPORTS_LLM)

// ============================================================================
// LLM Feature Flags
// ============================================================================

#if ENABLE_LOW_LEVEL_MEM_TRACKER

// LLM_ALLOW_ASSETS_TAGS: Set to 1 to enable run-time toggling of AssetTags reporting
// Enabling causes extra CPU time to track costs even when AssetTags are toggled off.
#ifndef LLM_ALLOW_ASSETS_TAGS
    #define LLM_ALLOW_ASSETS_TAGS 0
#endif

// LLM_ALLOW_STATS: Set to 1 to allow stats to be used as tags
#ifndef LLM_ALLOW_STATS
    #define LLM_ALLOW_STATS 0
#endif

// Enable stat tags if: (1) Stats or (2) Asset tags are allowed
#define LLM_ENABLED_STAT_TAGS (LLM_ALLOW_STATS || LLM_ALLOW_ASSETS_TAGS)

#else // !ENABLE_LOW_LEVEL_MEM_TRACKER

// Define macros to 0 when LLM is disabled to prevent undefined symbol errors
#define LLM_ALLOW_ASSETS_TAGS 0
#define LLM_ALLOW_STATS 0
#define LLM_ENABLED_STAT_TAGS 0

#endif // ENABLE_LOW_LEVEL_MEM_TRACKER
