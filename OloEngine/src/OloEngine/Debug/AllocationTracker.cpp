#include "OloEnginePCH.h"
#include "AllocationTracker.h"

#if OLO_ENABLE_ALLOCATION_TRACKING
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <chrono>
#include <algorithm>
#include <thread>
#include <sstream>
#include <iomanip>

// Stack trace support - available when <stacktrace> header is present (C++23)
#if __has_include(<stacktrace>) && __cplusplus >= 202302L
#include <stacktrace>
#define OLO_HAS_STACKTRACE 1
#else
#define OLO_HAS_STACKTRACE 0
#endif

namespace OloEngine
{

    /**
     * @brief Extended allocation tracker with live set tracking and leak inspection
     *
     * This provides additional debugging capabilities beyond simple counting:
     * - Track actual object pointers for leak inspection
     * - Delta tracking for testing allocation neutrality
     * - Thread-safe operations with minimal performance impact
     *
     * Note: This has higher overhead than the basic tracker due to hash table operations
     * and mutex contention. Use judiciously for heavyweight objects or leak hunting.
     */
    template<class Tag>
    class AllocationTrackerExtended
    {
      public:
        struct ObjectInfo
        {
            void const* address;
            std::chrono::steady_clock::time_point created_at;
            std::thread::id creation_thread;
            sizet creation_order; // Global creation order for debugging

#if OLO_HAS_STACKTRACE
            std::stacktrace creation_stack;
#endif

            // Constructor for creating ObjectInfo
            ObjectInfo(void const* addr)
                : address(addr), created_at(std::chrono::steady_clock::now()), creation_thread(std::this_thread::get_id()), creation_order(GetNextCreationOrder())
#if OLO_HAS_STACKTRACE
                  ,
                  creation_stack(std::stacktrace::current(1, 32)) // Skip 1 frame, max 32 frames
#endif
            {
            }

            /**
             * @brief Get age of this object in seconds
             */
            double GetAgeSeconds() const
            {
                auto now = std::chrono::steady_clock::now();
                auto duration = now - created_at;
                return std::chrono::duration<double>(duration).count();
            }

            /**
             * @brief Get formatted creation info string
             */
            std::string GetCreationInfo() const
            {
                std::ostringstream oss;
                oss << "Created: " << std::fixed << std::setprecision(3) << GetAgeSeconds() << "s ago";
                oss << " | Thread: " << creation_thread;
                oss << " | Order: " << creation_order;
                return oss.str();
            }

#if OLO_HAS_STACKTRACE
            /**
             * @brief Get formatted stack trace string
             */
            std::string GetStackTrace() const
            {
                std::ostringstream oss;
                oss << "Creation stack trace:\n";

                for (const auto& frame : creation_stack)
                {
                    oss << "  " << frame << "\n";
                }

                return oss.str();
            }
#endif

          private:
            static sizet GetNextCreationOrder()
            {
                static std::atomic<sizet> s_creation_counter{ 0 };
                return s_creation_counter.fetch_add(1, std::memory_order_relaxed);
            }
        };

        /**
         * @brief Track object creation
         */
        static void TrackCreation(void const* obj)
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            GetLiveObjects().emplace(obj, ObjectInfo{ obj });
        }

        /**
         * @brief Track object destruction
         */
        static void TrackDestruction(void const* obj)
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            GetLiveObjects().erase(obj);
        }

        /**
         * @brief Get all currently live objects
         * @return Vector of pointers to live objects (safe to iterate)
         */
        static std::vector<void const*> GetLiveObjectPointers()
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            std::vector<void const*> result;
            result.reserve(GetLiveObjects().size());

            for (auto const& [ptr, info] : GetLiveObjects())
            {
                result.push_back(ptr);
            }

            return result;
        }

        /**
         * @brief Get detailed information about live objects
         */
        static std::vector<ObjectInfo> GetLiveObjectInfo()
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            std::vector<ObjectInfo> result;
            result.reserve(GetLiveObjects().size());

            for (auto const& [ptr, info] : GetLiveObjects())
            {
                result.push_back(info);
            }

            return result;
        }

        /**
         * @brief Get objects that have been alive for longer than specified duration
         * @param min_age Minimum age in seconds
         * @return Vector of object info for old objects (potential leaks)
         */
        static std::vector<ObjectInfo> GetOldObjects(double min_age_seconds = 60.0)
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            std::vector<ObjectInfo> result;

            for (auto const& [ptr, info] : GetLiveObjects())
            {
                if (info.GetAgeSeconds() >= min_age_seconds)
                {
                    result.push_back(info);
                }
            }

            // Sort by age (oldest first)
            std::sort(result.begin(), result.end(),
                      [](const ObjectInfo& a, const ObjectInfo& b)
                      {
                          return a.created_at < b.created_at;
                      });

            return result;
        }

        /**
         * @brief Get objects created by a specific thread
         */
        static std::vector<ObjectInfo> GetObjectsByThread(std::thread::id thread_id)
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            std::vector<ObjectInfo> result;

            for (auto const& [ptr, info] : GetLiveObjects())
            {
                if (info.creation_thread == thread_id)
                {
                    result.push_back(info);
                }
            }

            return result;
        }

        /**
         * @brief Get objects created in a specific time range
         */
        static std::vector<ObjectInfo> GetObjectsInTimeRange(
            std::chrono::steady_clock::time_point start,
            std::chrono::steady_clock::time_point end)
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            std::vector<ObjectInfo> result;

            for (auto const& [ptr, info] : GetLiveObjects())
            {
                if (info.created_at >= start && info.created_at <= end)
                {
                    result.push_back(info);
                }
            }

            return result;
        }

        /**
         * @brief Generate a detailed leak report
         */
        static std::string GenerateLeakReport(double min_age_seconds = 60.0)
        {
            auto old_objects = GetOldObjects(min_age_seconds);
            if (old_objects.empty())
            {
                return "No leaks detected (no objects older than " +
                       std::to_string(min_age_seconds) + " seconds)";
            }

            std::ostringstream report;
            report << "=== LEAK REPORT for " << typeid(Tag).name() << " ===\n";
            report << "Found " << old_objects.size() << " potential leaks:\n\n";

            for (sizet i = 0; i < old_objects.size(); ++i)
            {
                const auto& obj = old_objects[i];
                report << "Leak #" << (i + 1) << ":\n";
                report << "  Address: " << obj.address << "\n";
                report << "  " << obj.GetCreationInfo() << "\n";

#if OLO_HAS_STACKTRACE
                report << "  " << obj.GetStackTrace() << "\n";
#else
                report << "  Stack trace: Not available (requires C++23 and MSVC 2022 17.4+)\n\n";
#endif
            }

            return report.str();
        }

#if OLO_HAS_STACKTRACE
        /**
         * @brief Print stack traces for all live objects
         */
        static void PrintAllStackTraces()
        {
            std::lock_guard<std::mutex> lock(GetMutex());

            if (GetLiveObjects().empty())
            {
                OLO_CORE_INFO("No live objects to show stack traces for");
                return;
            }

            OLO_CORE_INFO("Stack traces for all live {} objects:", typeid(Tag).name());

            sizet count = 1;
            for (auto const& [ptr, info] : GetLiveObjects())
            {
                OLO_CORE_INFO("Object #{} at {}:", count++, ptr);
                OLO_CORE_INFO("{}", info.GetCreationInfo());
                OLO_CORE_INFO("{}", info.GetStackTrace());
            }
        }
#endif

        /**
         * @brief Get current live object count (thread-safe)
         */
        static sizet GetLiveCount()
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            return GetLiveObjects().size();
        }

        /**
         * @brief Clear all tracking data
         * @warning Only call when certain no tracked objects exist!
         */
        static void Clear()
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            GetLiveObjects().clear();
        }

      private:
        using LiveObjectMap = std::unordered_map<void const*, ObjectInfo>;

        static LiveObjectMap& GetLiveObjects()
        {
            static LiveObjectMap s_live_objects;
            return s_live_objects;
        }

        static std::mutex& GetMutex()
        {
            static std::mutex s_mutex;
            return s_mutex;
        }
    };

    /**
     * @brief Allocation snapshot for delta tracking
     *
     * Allows testing that operations are allocation-neutral even in systems
     * that already have baseline leaks. This is particularly useful for
     * large codebases where achieving zero global leaks is impractical.
     */
    template<class Tag>
    class AllocationSnapshot
    {
      public:
        AllocationSnapshot()
            : m_snapshot_time(std::chrono::steady_clock::now()), m_live_objects(AllocationTrackerExtended<Tag>::GetLiveObjectPointers()), m_initial_count(m_live_objects.size())
        {
        }

        /**
         * @brief Get objects that are live now but weren't in the snapshot
         * @return Vector of pointers to newly created objects
         */
        std::vector<void const*> GetNewObjects() const
        {
            const auto current_objects = AllocationTrackerExtended<Tag>::GetLiveObjectPointers();
            std::vector<void const*> new_objects;

            // Create unordered_set from snapshot for O(1) lookups
            std::unordered_set<void const*> snapshot_set;
            snapshot_set.reserve(m_live_objects.size());
            for (void const* obj : m_live_objects)
            {
                snapshot_set.insert(obj);
            }

            // Find objects in current that aren't in snapshot
            for (void const* obj : current_objects)
            {
                if (snapshot_set.find(obj) == snapshot_set.end())
                {
                    new_objects.push_back(obj);
                }
            }

            return new_objects;
        }

        /**
         * @brief Get objects that were in snapshot but are no longer live
         * @return Vector of pointers to destroyed objects
         */
        std::vector<void const*> GetDestroyedObjects() const
        {
            const auto current_objects = AllocationTrackerExtended<Tag>::GetLiveObjectPointers();
            std::vector<void const*> destroyed_objects;

            // Create unordered_set from current objects for O(1) lookups
            std::unordered_set<void const*> current_set;
            current_set.reserve(current_objects.size());
            for (void const* obj : current_objects)
            {
                current_set.insert(obj);
            }

            // Find objects in snapshot that aren't in current
            for (void const* obj : m_live_objects)
            {
                if (current_set.find(obj) == current_set.end())
                {
                    destroyed_objects.push_back(obj);
                }
            }

            return destroyed_objects;
        }

        /**
         * @brief Check if any new objects have been created since snapshot
         */
        bool HasNewObjects() const
        {
            const auto current_objects = AllocationTrackerExtended<Tag>::GetLiveObjectPointers();

            // Create unordered_set from snapshot for O(1) lookups
            std::unordered_set<void const*> snapshot_set;
            snapshot_set.reserve(m_live_objects.size());
            for (void const* obj : m_live_objects)
            {
                snapshot_set.insert(obj);
            }

            // Check if any current object is not in snapshot (early exit on first match)
            for (void const* obj : current_objects)
            {
                if (snapshot_set.find(obj) == snapshot_set.end())
                {
                    return true; // Found a new object, early exit
                }
            }

            return false; // No new objects found
        }

        /**
         * @brief Get count of new objects since snapshot
         */
        sizet GetNewObjectCount() const
        {
            const auto current_objects = AllocationTrackerExtended<Tag>::GetLiveObjectPointers();
            sizet count = 0;

            // Create unordered_set from snapshot for O(1) lookups
            std::unordered_set<void const*> snapshot_set;
            snapshot_set.reserve(m_live_objects.size());
            for (void const* obj : m_live_objects)
            {
                snapshot_set.insert(obj);
            }

            // Count objects in current that aren't in snapshot (no vector allocation)
            for (void const* obj : current_objects)
            {
                if (snapshot_set.find(obj) == snapshot_set.end())
                {
                    ++count;
                }
            }

            return count;
        }

        /**
         * @brief Get net change in object count since snapshot
         * @return Positive if more objects, negative if fewer objects
         */
        i64 GetNetChange() const
        {
            sizet current_count = AllocationTrackerExtended<Tag>::GetLiveCount();
            return static_cast<i64>(current_count) - static_cast<i64>(m_initial_count);
        }

        /**
         * @brief Check if allocation is neutral (same count as snapshot)
         */
        bool IsAllocationNeutral() const
        {
            return GetNetChange() == 0;
        }

        /**
         * @brief Get detailed delta report
         */
        std::string GetDeltaReport() const
        {
            std::ostringstream report;
            report << "=== ALLOCATION DELTA REPORT ===\n";
            report << "Snapshot taken: " << GetSnapshotAgeSeconds() << " seconds ago\n";
            report << "Initial count: " << m_initial_count << "\n";
            report << "Current count: " << AllocationTrackerExtended<Tag>::GetLiveCount() << "\n";
            report << "Net change: " << GetNetChange() << "\n";

            auto new_objects = GetNewObjects();
            auto destroyed_objects = GetDestroyedObjects();

            report << "New objects: " << new_objects.size() << "\n";
            report << "Destroyed objects: " << destroyed_objects.size() << "\n";

            if (IsAllocationNeutral())
            {
                report << "Status: ALLOCATION NEUTRAL ✅\n";
            }
            else
            {
                report << "Status: ALLOCATION NOT NEUTRAL ❌\n";
            }

            return report.str();
        }

        /**
         * @brief Get age of this snapshot in seconds
         */
        double GetSnapshotAgeSeconds() const
        {
            auto now = std::chrono::steady_clock::now();
            auto duration = now - m_snapshot_time;
            return std::chrono::duration<double>(duration).count();
        }

      private:
        std::chrono::steady_clock::time_point m_snapshot_time;
        std::vector<void const*> m_live_objects;
        sizet m_initial_count;
    };

    // Explicit instantiation declarations for commonly used types
    // This ensures the extended tracker is available for these types
    // Add more instantiations as needed for your specific classes

    // Note: We don't instantiate specific types here to avoid coupling
    // The extended tracker will be instantiated on-demand when used

    // Convenience function implementations
    template<typename T>
    std::string GenerateLeakReport(double min_age_seconds)
    {
        return AllocationTrackerExtended<T>::GenerateLeakReport(min_age_seconds);
    }

    template<typename T>
    AllocationSnapshot<T> CreateAllocationSnapshot()
    {
        return AllocationSnapshot<T>();
    }

    template<typename T>
    bool IsAllocationNeutral(const AllocationSnapshot<T>& snapshot)
    {
        return snapshot.IsAllocationNeutral();
    }

} // namespace OloEngine

#endif // OLO_ENABLE_ALLOCATION_TRACKING
