#pragma once

#include "OloEngine/Core/Base.h"
#include <atomic>

namespace OloEngine {

// Enable allocation tracking only in debug builds by default
// Can be overridden by defining OLO_FORCE_ALLOCATION_TRACKING
#if defined(OLO_DEBUG) || defined(OLO_FORCE_ALLOCATION_TRACKING)
    #define OLO_ENABLE_ALLOCATION_TRACKING 1
#else
    #define OLO_ENABLE_ALLOCATION_TRACKING 0
#endif

#if OLO_ENABLE_ALLOCATION_TRACKING

/**
 * @brief Lightweight allocation tracker for object lifetime debugging
 * 
 * This tracker uses the CRTP (Curiously Recurring Template Pattern) to track
 * object creation and destruction counts per type. Each class gets its own
 * independent counter, allowing precise leak detection at the class level.
 * 
 * Usage:
 *   class MyClass : public AllocationTracker<MyClass> {
 *       // Your class implementation
 *   };
 * 
 *   // In tests or debug code:
 *   auto initial = MyClass::GetLiveCount();
 *   {
 *       MyClass obj1, obj2;
 *       assert(MyClass::GetLiveCount() == initial + 2);
 *   }
 *   assert(MyClass::GetLiveCount() == initial);  // No leaks!
 * 
 * Based on the allocation tracker pattern from:
 * https://solidean.com/blog/2025/minimal-allocation-tracker-cpp/
 * 
 * Performance considerations:
 * - Uses relaxed memory ordering for optimal performance
 * - Zero overhead when disabled (complete elimination in release builds)
 * - Thread-safe atomic operations
 * - Empty base class optimization ensures no memory overhead
 */
template <class Tag>
class AllocationTracker
{
public:
    /**
     * @brief Default constructor - increments live object count
     */
    AllocationTracker() noexcept
    {
        // Use relaxed ordering for best performance
        // We only care about the final count, not ordering between threads
        s_LiveCount.fetch_add(1, std::memory_order_relaxed);
    }
    
    /**
     * @brief Copy constructor - increments live object count
     * Each copy is a new object that needs tracking
     */
    AllocationTracker(AllocationTracker const&) noexcept
    {
        s_LiveCount.fetch_add(1, std::memory_order_relaxed);
    }
    
    /**
     * @brief Move constructor - increments live object count
     * Even though data is moved, we still have a new object instance
     */
    AllocationTracker(AllocationTracker&&) noexcept
    {
        s_LiveCount.fetch_add(1, std::memory_order_relaxed);
    }
    
    /**
     * @brief Copy assignment operator
     * Defaulted because assignment doesn't create/destroy objects
     */
    AllocationTracker& operator=(AllocationTracker const&) noexcept = default;
    
    /**
     * @brief Move assignment operator  
     * Defaulted because assignment doesn't create/destroy objects
     */
    AllocationTracker& operator=(AllocationTracker&&) noexcept = default;
    
    /**
     * @brief Destructor - decrements live object count
     */
    ~AllocationTracker() noexcept
    {
        // Use relaxed ordering for best performance
        s_LiveCount.fetch_sub(1, std::memory_order_relaxed);
    }
    
    /**
     * @brief Get the current number of live objects of this type
     * @return Current count of objects that have been constructed but not yet destroyed
     */
    static sizet GetLiveCount() noexcept
    {
        return s_LiveCount.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Get the peak number of live objects of this type
     * @return Maximum number of objects that were alive simultaneously
     */
    static sizet GetPeakCount() noexcept
    {
        return s_PeakCount.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Get the total number of objects ever created of this type
     * @return Cumulative count of all objects ever constructed
     */
    static sizet GetTotalCreated() noexcept
    {
        return s_TotalCreated.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Reset all counters to zero
     * @warning Only call this when you're certain no objects of this type exist!
     */
    static void ResetCounters() noexcept
    {
        s_LiveCount.store(0, std::memory_order_relaxed);
        s_PeakCount.store(0, std::memory_order_relaxed);
        s_TotalCreated.store(0, std::memory_order_relaxed);
    }
    
    /**
     * @brief Get a formatted string with allocation statistics
     * @return Human-readable string with current, peak, and total counts
     */
    static std::string GetStatsString()
    {
        sizet live = GetLiveCount();
        sizet peak = GetPeakCount();
        sizet total = GetTotalCreated();
        
        // Format: "Live: X | Peak: Y | Total: Z"
        return "Live: " + std::to_string(live) + 
               " | Peak: " + std::to_string(peak) + 
               " | Total: " + std::to_string(total);
    }

private:
    /**
     * @brief Update peak count if current count is higher
     * Called by constructors to track peak usage
     */
    static void UpdatePeakCount(sizet currentCount) noexcept
    {
        sizet currentPeak = s_PeakCount.load(std::memory_order_relaxed);
        while (currentCount > currentPeak && 
               !s_PeakCount.compare_exchange_weak(currentPeak, currentCount, 
                                                  std::memory_order_relaxed)) {
            // Loop until we successfully update or another thread sets a higher value
        }
    }
    
    // Static data members - one set per template instantiation
    static std::atomic<sizet> s_LiveCount;      ///< Current number of live objects
    static std::atomic<sizet> s_PeakCount;      ///< Peak number of simultaneous objects
    static std::atomic<sizet> s_TotalCreated;   ///< Total objects ever created
    
    // Friend class for advanced tracking extensions
    template<class T> friend class AllocationTrackerExtended;
    template<class T> friend class AllocationSnapshot;
};

// Forward declarations for extended features (implementation in .cpp)
template<class Tag> class AllocationTrackerExtended;
template<class Tag> class AllocationSnapshot;

// Static member definitions - must be in header for template classes
template<class Tag>
std::atomic<sizet> AllocationTracker<Tag>::s_LiveCount{0};

template<class Tag>
std::atomic<sizet> AllocationTracker<Tag>::s_PeakCount{0};

template<class Tag>
std::atomic<sizet> AllocationTracker<Tag>::s_TotalCreated{0};

#else // OLO_ENABLE_ALLOCATION_TRACKING == 0

/**
 * @brief Empty allocation tracker for release builds
 * 
 * This version provides the same interface but does absolutely nothing.
 * All methods are constexpr or empty, allowing complete optimization away.
 * The class itself has no data members and will be optimized away entirely.
 */
template <class Tag>
class AllocationTracker
{
public:
    // All constructors/destructors are completely empty and will be optimized away
    constexpr AllocationTracker() noexcept = default;
    constexpr AllocationTracker(AllocationTracker const&) noexcept = default;
    constexpr AllocationTracker(AllocationTracker&&) noexcept = default;
    constexpr AllocationTracker& operator=(AllocationTracker const&) noexcept = default;
    constexpr AllocationTracker& operator=(AllocationTracker&&) noexcept = default;
    ~AllocationTracker() noexcept = default;
    
    /**
     * @brief Always returns 0 in release builds
     */
    static constexpr sizet GetLiveCount() noexcept { return 0; }
    
    /**
     * @brief Always returns 0 in release builds
     */
    static constexpr sizet GetPeakCount() noexcept { return 0; }
    
    /**
     * @brief Always returns 0 in release builds
     */
    static constexpr sizet GetTotalCreated() noexcept { return 0; }
    
    /**
     * @brief Does nothing in release builds
     */
    static constexpr void ResetCounters() noexcept {}
    
    /**
     * @brief Returns empty stats in release builds
     */
    static std::string GetStatsString() { return "Tracking disabled (release build)"; }
};

#endif // OLO_ENABLE_ALLOCATION_TRACKING

} // namespace OloEngine

// Extended tracking functions (only available when tracking is enabled)
#if OLO_ENABLE_ALLOCATION_TRACKING

namespace OloEngine {

/**
 * @brief Generate a leak report for a specific class
 * Usage: auto report = GenerateLeakReport<MyClass>(60.0);
 */
template<typename T>
std::string GenerateLeakReport(double min_age_seconds = 60.0);

/**
 * @brief Create an allocation snapshot for delta tracking
 * Usage: auto snapshot = CreateAllocationSnapshot<MyClass>();
 */
template<typename T>
class AllocationSnapshot;

template<typename T>
AllocationSnapshot<T> CreateAllocationSnapshot();

/**
 * @brief Check if allocation is neutral compared to snapshot
 * Usage: bool neutral = IsAllocationNeutral<MyClass>(snapshot);
 */
template<typename T>
bool IsAllocationNeutral(const AllocationSnapshot<T>& snapshot);

} // namespace OloEngine

#endif // OLO_ENABLE_ALLOCATION_TRACKING

/**
 * @brief Convenience macro for declaring a tracked class
 * 
 * Usage:
 *   class MyClass : OLO_ALLOCATION_TRACKED(MyClass) {
 *       // Your class implementation
 *   };
 * 
 * This macro expands to the appropriate inheritance in debug builds,
 * and to nothing in release builds for zero overhead.
 */
#if OLO_ENABLE_ALLOCATION_TRACKING
    #define OLO_ALLOCATION_TRACKED(ClassName) \
        public ::OloEngine::AllocationTracker<ClassName>
#else
    #define OLO_ALLOCATION_TRACKED(ClassName) \
        /* Empty in release builds */
#endif

/**
 * @brief Convenience macro for combining RefCounted with AllocationTracker
 * 
 * Usage:
 *   class MyRefCountedClass : OLO_TRACKED_REFCOUNTED(MyRefCountedClass) {
 *       // Your class implementation
 *   };
 */
#if OLO_ENABLE_ALLOCATION_TRACKING
    #define OLO_TRACKED_REFCOUNTED(ClassName) \
        public ::OloEngine::RefCounted, public ::OloEngine::AllocationTracker<ClassName>
#else
    #define OLO_TRACKED_REFCOUNTED(ClassName) \
        public ::OloEngine::RefCounted
#endif

/**
 * @brief Convenience macro for asserting no leaks in tests
 * 
 * Usage:
 *   void MyTest() {
 *       auto snapshot = OLO_ALLOCATION_SNAPSHOT(MyClass);
 *       // ... test code that creates/destroys MyClass objects ...
 *       OLO_ASSERT_NO_LEAKS(MyClass, snapshot);
 *   }
 */
#if OLO_ENABLE_ALLOCATION_TRACKING
    #define OLO_ALLOCATION_SNAPSHOT(ClassName) \
        ClassName::GetLiveCount()
        
    #define OLO_ASSERT_NO_LEAKS(ClassName, snapshot) \
        OLO_CORE_ASSERT(ClassName::GetLiveCount() == (snapshot), \
                        "Memory leak detected in {0}: {1} objects leaked", \
                        #ClassName, ClassName::GetLiveCount() - (snapshot))
#else
    #define OLO_ALLOCATION_SNAPSHOT(ClassName) 0
    #define OLO_ASSERT_NO_LEAKS(ClassName, snapshot) ((void)0)
#endif