#pragma once

// A small, thread-safe ring buffer of recent "what just happened?" engine events —
// scene loads, play/stop, entity spawn/destroy, asset reloads, and script errors —
// giving an agent a unified diagnostics timeline. It mirrors the script-error ring
// buffer (Scripting/ScriptError.h) and the engine log ring buffer (Core/Log.h):
// real engine seams push structured records from the game thread; the read-only MCP
// diagnostics server (#306 item B) reads them from its handler thread, so every
// access is mutex-guarded.
//
// "Expose, don't embed": this records structured events only — it performs no
// analysis. The MCP `olo_events_tail` tool serializes the records; any reasoning is
// left to the agent reading them.
//
// Header-only with an inline singleton: OloEngine is a static library, so the single
// function-local static is shared across every translation unit in the final binary
// (engine seams write it, the editor's MCP layer reads it).

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OloEngine
{
    enum class DiagnosticEventCategory : u8
    {
        SceneLoad = 0,
        Play,
        Stop,
        EntitySpawn,
        EntityDestroy,
        AssetReload,
        ScriptError,
    };

    // Keep in sync with DiagnosticEventCategory; used to validate the enum/string maps.
    inline constexpr std::size_t kDiagnosticEventCategoryCount = 7;

    struct DiagnosticEvent
    {
        u64 Id = 0;          // monotonic, 1-based; assigned on Record. Doubles as the ordering key.
        f64 Timestamp = 0.0; // Unix epoch seconds, stamped on Record.
        DiagnosticEventCategory Category = DiagnosticEventCategory::SceneLoad;
        std::string Message; // short human-readable summary (always present).
        u64 Entity = 0;      // optional entity UUID value; 0 = not applicable.
        std::string Context; // optional structured secondary field: scene name / asset path / script name.

        // Stable snake_case token used in MCP output and accepted by the category filter.
        [[nodiscard]] static const char* CategoryToString(DiagnosticEventCategory category)
        {
            switch (category)
            {
                case DiagnosticEventCategory::SceneLoad:
                    return "scene_load";
                case DiagnosticEventCategory::Play:
                    return "play";
                case DiagnosticEventCategory::Stop:
                    return "stop";
                case DiagnosticEventCategory::EntitySpawn:
                    return "entity_spawn";
                case DiagnosticEventCategory::EntityDestroy:
                    return "entity_destroy";
                case DiagnosticEventCategory::AssetReload:
                    return "asset_reload";
                case DiagnosticEventCategory::ScriptError:
                    return "script_error";
            }
            return "unknown";
        }

        // Parse a category token (as emitted by CategoryToString). Returns false on a
        // value that does not name a category, so callers can reject bad filters.
        [[nodiscard]] static bool CategoryFromString(std::string_view name, DiagnosticEventCategory& out)
        {
            if (name == "scene_load")
                out = DiagnosticEventCategory::SceneLoad;
            else if (name == "play")
                out = DiagnosticEventCategory::Play;
            else if (name == "stop")
                out = DiagnosticEventCategory::Stop;
            else if (name == "entity_spawn")
                out = DiagnosticEventCategory::EntitySpawn;
            else if (name == "entity_destroy")
                out = DiagnosticEventCategory::EntityDestroy;
            else if (name == "asset_reload")
                out = DiagnosticEventCategory::AssetReload;
            else if (name == "script_error")
                out = DiagnosticEventCategory::ScriptError;
            else
                return false;
            return true;
        }
    };

    // Read parameters for DiagnosticsEventLog::Query.
    struct DiagnosticEventQuery
    {
        std::size_t MaxCount = 50;                       // newest-N cap applied after filtering.
        u64 SinceId = 0;                                 // 0 = no lower bound; else only events with Id > SinceId.
        std::vector<DiagnosticEventCategory> Categories; // empty = all categories.
    };

    // A consistent snapshot from DiagnosticsEventLog::QueryWithCursor: the filtered
    // events plus the highest Id present in the buffer at that same instant. Returning
    // both under one lock is what makes incremental polling lossless — read LastId
    // separately and a record landing between the two reads would advance the cursor
    // past an event the query never returned.
    struct DiagnosticEventQueryResult
    {
        std::vector<DiagnosticEvent> Events;
        u64 LastId = 0; // highest Id assigned at snapshot time (0 if nothing recorded).
    };

    class DiagnosticsEventLog
    {
      public:
        static DiagnosticsEventLog& Get()
        {
            static DiagnosticsEventLog s_Instance;
            return s_Instance;
        }

        // Append an event, assigning it a monotonic Id and a wall-clock timestamp.
        // Returns the assigned Id, or 0 when suppressed (see SuppressScope) — the
        // suppression check happens before the lock, so bulk operations pay nothing.
        u64 Record(DiagnosticEventCategory category, std::string message, u64 entity = 0, std::string context = {})
        {
            if (m_SuppressDepth.load(std::memory_order_relaxed) > 0)
                return 0;

            DiagnosticEvent event;
            event.Timestamp = std::chrono::duration<f64>(std::chrono::system_clock::now().time_since_epoch()).count();
            event.Category = category;
            event.Message = std::move(message);
            event.Entity = entity;
            event.Context = std::move(context);

            std::lock_guard lock(m_Mutex);
            event.Id = m_NextId++;
            const u64 assignedId = event.Id;
            m_Events.push_back(std::move(event));
            while (m_Events.size() > kCapacity)
                m_Events.pop_front();
            return assignedId;
        }

        // Filtered read, returned oldest-first (newest event last). Applies, in order:
        // the SinceId lower bound, the category filter, then keeps the newest MaxCount.
        [[nodiscard]] std::vector<DiagnosticEvent> Query(const DiagnosticEventQuery& query) const
        {
            std::lock_guard lock(m_Mutex);
            return QueryLocked(query);
        }

        // Filtered read PLUS the buffer's highest Id, captured under a single lock so the
        // returned cursor is consistent with the returned events. Pollers must use this
        // (not Query + LastId) so an event recorded between the two reads can't advance
        // the cursor past an event that was never returned.
        [[nodiscard]] DiagnosticEventQueryResult QueryWithCursor(const DiagnosticEventQuery& query) const
        {
            std::lock_guard lock(m_Mutex);
            return DiagnosticEventQueryResult{ QueryLocked(query), m_NextId - 1 };
        }

        // Highest Id assigned so far (0 if nothing recorded). Lets a poller learn the
        // latest cursor without reading events.
        [[nodiscard]] u64 LastId() const
        {
            std::lock_guard lock(m_Mutex);
            return m_NextId - 1;
        }

        void Clear()
        {
            std::lock_guard lock(m_Mutex);
            m_Events.clear();
            m_NextId = 1;
        }

        // RAII suppression: Record() is a no-op while any scope is alive. Used to mute
        // the per-entity EntitySpawn flood during bulk operations (whole-scene copy on
        // Play, scene deserialize on load) that would otherwise drown the ring buffer;
        // a single higher-level event (Play / SceneLoad) is recorded instead. The depth
        // counter is atomic so nesting and any incidental cross-thread use are safe.
        class SuppressScope
        {
          public:
            SuppressScope()
            {
                DiagnosticsEventLog::Get().m_SuppressDepth.fetch_add(1, std::memory_order_relaxed);
            }
            ~SuppressScope()
            {
                DiagnosticsEventLog::Get().m_SuppressDepth.fetch_sub(1, std::memory_order_relaxed);
            }
            SuppressScope(const SuppressScope&) = delete;
            SuppressScope& operator=(const SuppressScope&) = delete;
            SuppressScope(SuppressScope&&) = delete;
            SuppressScope& operator=(SuppressScope&&) = delete;
        };

      private:
        DiagnosticsEventLog() = default;

        // Shared filter for Query / QueryWithCursor. Caller must hold m_Mutex (m_Mutex
        // is not recursive). Oldest-first; SinceId lower bound, then category filter,
        // then the newest-MaxCount cap.
        [[nodiscard]] std::vector<DiagnosticEvent> QueryLocked(const DiagnosticEventQuery& query) const
        {
            std::vector<DiagnosticEvent> matched;
            for (const auto& event : m_Events)
            {
                if (query.SinceId != 0 && event.Id <= query.SinceId)
                    continue;
                if (!query.Categories.empty() &&
                    std::find(query.Categories.begin(), query.Categories.end(), event.Category) == query.Categories.end())
                    continue;
                matched.push_back(event);
            }

            if (query.MaxCount != 0 && matched.size() > query.MaxCount)
                matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(query.MaxCount));
            return matched;
        }

        static constexpr std::size_t kCapacity = 512;
        mutable std::mutex m_Mutex;
        std::deque<DiagnosticEvent> m_Events;
        u64 m_NextId = 1;
        std::atomic<u32> m_SuppressDepth{ 0 };
    };
} // namespace OloEngine
