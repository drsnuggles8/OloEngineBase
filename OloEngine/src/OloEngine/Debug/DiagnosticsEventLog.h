#pragma once

// A small, thread-safe ring buffer of recent "what just happened?" engine events —
// scene loads, play/stop, entity spawn/destroy, asset reloads, and script errors —
// giving an agent a unified diagnostics timeline. It mirrors the script-error ring
// buffer (Scripting/ScriptError.h) and the engine log ring buffer (Core/Log.h):
// real engine seams push structured records from the game thread; the read-only MCP
// diagnostics server (#306) reads them from its handler thread, so every
// access is mutex-guarded.
//
// "Expose, don't embed": this records structured events only — it performs no
// analysis. The MCP `olo_events_tail` tool serializes the records; any reasoning is
// left to the agent reading them.
//
// Since #1131 this ring is ALSO the automation event bus. Three things were added
// and the rest is unchanged:
//
//   * five categories for the editor's own lifecycle (scene saved / dirtied, asset
//     imported, compile finished, automation command completed), each carrying a
//     small structured `Data` object built from a CLOSED key set — see
//     DiagnosticEventDataKeys and the rule above it;
//   * a condition variable, so a subscriber can BLOCK for the next matching event
//     (WaitWithCursor) instead of polling a tail;
//   * a reported gap: a query whose cursor has fallen behind the ring learns how
//     many records it lost (DiagnosticEventQueryResult::Dropped) instead of
//     receiving a silently shortened history.
//
// THE SUBSCRIPTION MODEL IS A CURSOR, NOT A QUEUE. Every consumer — a poller, a
// long-poll wait, the MCP push stream, a CLI follow loop — holds an event id and
// asks for what came after it. There is no per-subscriber buffer anywhere, so a
// subscriber that stops reading costs this process nothing: the ring is the one
// fixed window (kCapacity), the oldest records are evicted regardless of who has
// read them, and a consumer that resumes past the window is told the count it
// missed. That is the whole backpressure answer #1131 asked for, and it is why a
// separate "event bus" object was not built over this one.
//
// Header-only with an inline singleton: OloEngine is a static library, so the single
// function-local static is shared across every translation unit in the final binary
// (engine seams write it, the editor's MCP layer reads it).

#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <mutex>
#include <span>
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
        // ---- automation event bus (#1131) ----------------------------------
        SceneSave,        // the authored scene was written to disk (editor menu or olo_scene_save*).
        SceneDirty,       // the scene's unsaved-changes state flipped; Data.dirty says which way.
        AssetImport,      // an asset was registered (file-watch auto-import or olo_asset_import).
        CompileFinished,  // a build target, script assembly or shader (re)compile ended; Data.ok says how.
        CommandCompleted, // a mutating automation command ran (see AutomationEvents.h for the rule).
    };

    // Every category's stable snake_case token, indexed by the enum value. THE ONE
    // spelling: CategoryToString and CategoryFromString read this table, the tool
    // schemas' enums are generated from it, and the exhaustiveness tests compare
    // the enum against it — so a category cannot be parseable, listable and
    // documented in different sets.
    inline constexpr std::array<std::string_view, 12> kDiagnosticEventCategoryTokens{
        "scene_load",
        "play",
        "stop",
        "entity_spawn",
        "entity_destroy",
        "asset_reload",
        "script_error",
        "scene_save",
        "scene_dirty",
        "asset_import",
        "compile_finished",
        "command_completed",
    };

    // Keep in sync with DiagnosticEventCategory; used to validate the enum/string maps.
    inline constexpr std::size_t kDiagnosticEventCategoryCount = kDiagnosticEventCategoryTokens.size();

    // THE PAYLOAD RULE (#1131 design constraint: "an event subscription is an
    // authority-bearing capability, not a free read: events can leak state a read
    // tool would have gated").
    //
    // An event carries the IDENTITY of what happened — a command name, a scene
    // name, a project-relative path, an entity id, an outcome — and never the
    // CONTENT a read command would return for it: no arguments, no results, no
    // serialized scene, no file bytes, no absolute paths. The content is exactly
    // what a gated read protects; keep it out of the event and a subscription can
    // honestly be offered at the authority of any read-only command (a bearer
    // token, and nothing more — which is also what lets a read-only frontend like
    // oloctl follow the stream).
    //
    // Enforced mechanically, not by review: the keys a category may carry are a
    // closed set below, Record() refuses anything else, and every string value is
    // truncated at kMaxDataValueChars so a payload physically cannot hold a
    // document. Adding a key is a deliberate edit to this table, in a diff.
    [[nodiscard]] inline std::span<const std::string_view> DiagnosticEventDataKeys(DiagnosticEventCategory category)
    {
        using namespace std::string_view_literals;
        static constexpr std::string_view kSceneSave[] = { "scene"sv, "path"sv, "changed"sv };
        static constexpr std::string_view kSceneDirty[] = { "scene"sv, "dirty"sv };
        static constexpr std::string_view kAssetImport[] = { "asset"sv, "type"sv, "handle"sv, "source"sv };
        static constexpr std::string_view kCompileFinished[] = { "kind"sv, "target"sv, "ok"sv,
                                                                 "errors"sv, "warnings"sv, "seconds"sv };
        static constexpr std::string_view kCommandCompleted[] = { "command"sv, "ok"sv, "durationMs"sv, "projectWrite"sv,
                                                                  "toolset"sv };
        switch (category)
        {
            case DiagnosticEventCategory::SceneSave:
                return kSceneSave;
            case DiagnosticEventCategory::SceneDirty:
                return kSceneDirty;
            case DiagnosticEventCategory::AssetImport:
                return kAssetImport;
            case DiagnosticEventCategory::CompileFinished:
                return kCompileFinished;
            case DiagnosticEventCategory::CommandCompleted:
                return kCommandCompleted;
            case DiagnosticEventCategory::SceneLoad:
            case DiagnosticEventCategory::Play:
            case DiagnosticEventCategory::Stop:
            case DiagnosticEventCategory::EntitySpawn:
            case DiagnosticEventCategory::EntityDestroy:
            case DiagnosticEventCategory::AssetReload:
            case DiagnosticEventCategory::ScriptError:
                break;
        }
        return {};
    }

    // Builds the structured `Data` object of one event as compact JSON text, without
    // pulling a JSON library into this engine-core header. Values are strings,
    // booleans and numbers only — no nesting, by design (see the payload rule).
    // Duplicate keys keep the last value. String values longer than
    // kMaxDataValueChars are cut (on a UTF-8 character boundary, so the text stays
    // valid) and end in "..." so the truncation is visible.
    class DiagnosticEventData
    {
      public:
        static constexpr std::size_t kMaxDataValueChars = 256;

        DiagnosticEventData& Set(std::string_view key, std::string_view value)
        {
            std::string text;
            if (value.size() > kMaxDataValueChars)
            {
                // Back off from the cut point over UTF-8 continuation bytes so a
                // multi-byte character is dropped whole rather than split.
                std::size_t cut = kMaxDataValueChars;
                while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0u) == 0x80u)
                    --cut;
                text = std::string(value.substr(0, cut)) + "...";
            }
            else
            {
                text = std::string(value);
            }
            return Put(key, "\"" + Escape(text) + "\"");
        }
        DiagnosticEventData& Set(std::string_view key, const char* value)
        {
            return Set(key, std::string_view(value));
        }
        DiagnosticEventData& Set(std::string_view key, const std::string& value)
        {
            return Set(key, std::string_view(value));
        }
        DiagnosticEventData& Set(std::string_view key, bool value)
        {
            return Put(key, value ? "true" : "false");
        }
        DiagnosticEventData& Set(std::string_view key, u64 value)
        {
            return Put(key, std::to_string(value));
        }
        DiagnosticEventData& Set(std::string_view key, i64 value)
        {
            return Put(key, std::to_string(value));
        }
        DiagnosticEventData& Set(std::string_view key, int value)
        {
            return Put(key, std::to_string(value));
        }
        DiagnosticEventData& Set(std::string_view key, u32 value)
        {
            return Put(key, std::to_string(value));
        }
        DiagnosticEventData& Set(std::string_view key, f64 value)
        {
            // JSON has no NaN/Inf; a non-finite measurement is reported as null
            // rather than as a token no parser accepts.
            if (!std::isfinite(value))
                return Put(key, "null");
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.6g", value);
            return Put(key, buffer);
        }

        [[nodiscard]] bool Empty() const
        {
            return m_Fields.empty();
        }

        // The keys set so far, in insertion order. Record() checks them against the
        // category's closed set.
        [[nodiscard]] std::vector<std::string_view> Keys() const
        {
            std::vector<std::string_view> keys;
            keys.reserve(m_Fields.size());
            for (const auto& [key, value] : m_Fields)
                keys.push_back(key);
            return keys;
        }

        // Compact JSON object text, or "" when nothing was set.
        [[nodiscard]] std::string Build() const
        {
            if (m_Fields.empty())
                return {};
            std::string out = "{";
            bool first = true;
            for (const auto& [key, encoded] : m_Fields)
            {
                if (!first)
                    out += ',';
                first = false;
                out += '"';
                out += key;
                out += "\":";
                out += encoded;
            }
            out += '}';
            return out;
        }

        // Minimal JSON string escaping: quotes, backslashes and control characters.
        // Non-ASCII UTF-8 passes through untouched, which JSON permits.
        [[nodiscard]] static std::string Escape(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (const char c : text)
            {
                const auto byte = static_cast<unsigned char>(c);
                switch (c)
                {
                    case '"':
                        out += "\\\"";
                        break;
                    case '\\':
                        out += "\\\\";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        if (byte < 0x20)
                        {
                            char buffer[8];
                            std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(byte));
                            out += buffer;
                        }
                        else
                        {
                            out += c;
                        }
                        break;
                }
            }
            return out;
        }

      private:
        DiagnosticEventData& Put(std::string_view key, std::string encoded)
        {
            for (auto& [existing, value] : m_Fields)
            {
                if (existing == key)
                {
                    value = std::move(encoded);
                    return *this;
                }
            }
            m_Fields.emplace_back(std::string(key), std::move(encoded));
            return *this;
        }

        std::vector<std::pair<std::string, std::string>> m_Fields;
    };

    struct DiagnosticEvent
    {
        u64 Id = 0;          // monotonic, 1-based; assigned on Record. Doubles as the ordering key.
        f64 Timestamp = 0.0; // Unix epoch seconds, stamped on Record.
        DiagnosticEventCategory Category = DiagnosticEventCategory::SceneLoad;
        std::string Message; // short human-readable summary (always present).
        u64 Entity = 0;      // optional entity UUID value; 0 = not applicable.
        std::string Context; // optional structured secondary field: scene name / asset path / script name.
        // Optional structured payload as compact JSON object text (see
        // DiagnosticEventData), empty for the categories that carry none. Serialized
        // under `data` by the MCP carriers; a consumer that only knows the six older
        // fields still reads every record.
        std::string Data;

        // Stable snake_case token used in MCP output and accepted by the category filter.
        [[nodiscard]] static const char* CategoryToString(DiagnosticEventCategory category)
        {
            const auto index = static_cast<std::size_t>(category);
            // The tokens are string literals, so data() is NUL-terminated.
            return index < kDiagnosticEventCategoryTokens.size() ? kDiagnosticEventCategoryTokens[index].data()
                                                                 : "unknown";
        }

        // Parse a category token (as emitted by CategoryToString). Returns false on a
        // value that does not name a category, so callers can reject bad filters.
        [[nodiscard]] static bool CategoryFromString(std::string_view name, DiagnosticEventCategory& out)
        {
            for (std::size_t index = 0; index < kDiagnosticEventCategoryTokens.size(); ++index)
            {
                if (kDiagnosticEventCategoryTokens[index] == name)
                {
                    out = static_cast<DiagnosticEventCategory>(index);
                    return true;
                }
            }
            return false;
        }

        // Every category token, in enum order — the single list the filters' error
        // messages and the tools' schemas are generated from.
        [[nodiscard]] static std::span<const std::string_view> AllCategoryTokens()
        {
            return kDiagnosticEventCategoryTokens;
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
        // How many records with an Id above SinceId were EVICTED before this query
        // could return them — the gap a cursor that fell behind the ring is told
        // about, counted before the category filter (an evicted record's category is
        // unknown). Always 0 for a query with no cursor (SinceId == 0), which asks
        // for the newest N by construction. A consumer that sees a non-zero value
        // has lost history and should say so rather than continue as if complete.
        u64 Dropped = 0;
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
        // Returns the assigned Id, or 0 when suppressed (see SuppressScope and
        // SuppressCategoryScope) — the suppression checks happen before the lock,
        // so bulk operations pay nothing. Wakes every WaitWithCursor blocked on
        // the ring.
        u64 Record(DiagnosticEventCategory category, std::string message, u64 entity = 0, std::string context = {})
        {
            return Record(category, std::move(message), entity, std::move(context), DiagnosticEventData{});
        }

        // As above, with a structured payload. The payload's keys MUST all be in
        // DiagnosticEventDataKeys(category); a key outside the closed set is a
        // programmer error and the record is refused loudly (assert) rather than
        // written with the key silently dropped — the payload rule is only worth
        // anything if it cannot be bypassed by a typo.
        u64 Record(DiagnosticEventCategory category, std::string message, u64 entity, std::string context,
                   const DiagnosticEventData& data)
        {
            if (m_SuppressDepth.load(std::memory_order_relaxed) > 0)
                return 0;
            if (t_CategorySuppressDepth[static_cast<std::size_t>(category)] > 0)
                return 0;

            const std::span<const std::string_view> allowed = DiagnosticEventDataKeys(category);
            for (const std::string_view key : data.Keys())
            {
                const bool permitted = std::find(allowed.begin(), allowed.end(), key) != allowed.end();
                OLO_CORE_ASSERT(permitted,
                                "[DiagnosticsEventLog] Event data key '{}' is not in the closed key set of "
                                "category '{}' (see DiagnosticEventDataKeys). Record refused.",
                                key, DiagnosticEvent::CategoryToString(category));
                if (!permitted)
                    return 0;
            }

            DiagnosticEvent event;
            event.Timestamp = std::chrono::duration<f64>(std::chrono::system_clock::now().time_since_epoch()).count();
            event.Category = category;
            event.Message = std::move(message);
            event.Entity = entity;
            event.Context = std::move(context);
            event.Data = data.Build();

            u64 assignedId = 0;
            {
                std::lock_guard lock(m_Mutex);
                event.Id = m_NextId++;
                assignedId = event.Id;
                m_Events.push_back(std::move(event));
                while (m_Events.size() > kCapacity)
                    m_Events.pop_front();
            }
            // Outside the lock: a waiter that wakes and immediately re-locks must not
            // contend with the thread that woke it.
            m_Changed.notify_all();
            return assignedId;
        }

        // A `compile_finished` record with EVERY declared key filled, whichever of
        // the three compile paths (a build target, the script assembly, a shader
        // library) reports it. Lives here rather than in the editor's publisher
        // set because two of the three sites are engine code; the editor's
        // Events::PublishCompileFinished forwards to it, so the record has one
        // shape. `kind` is "build", "script" or "shader"; counts and seconds are
        // 0 when a path does not measure them.
        u64 RecordCompileFinished(std::string_view kind, std::string_view target, bool ok, u32 errors, u32 warnings,
                                  f64 seconds)
        {
            DiagnosticEventData data;
            data.Set("kind", kind)
                .Set("target", target)
                .Set("ok", ok)
                .Set("errors", errors)
                .Set("warnings", warnings)
                .Set("seconds", seconds);
            std::string message = std::string(kind) + " compile of '" + std::string(target) + "' " +
                                  (ok ? "succeeded" : "FAILED");
            if (errors != 0 || warnings != 0)
                message += " (" + std::to_string(errors) + " errors, " + std::to_string(warnings) + " warnings)";
            return Record(DiagnosticEventCategory::CompileFinished, std::move(message), 0, std::string(target), data);
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
            return QueryWithCursorLocked(query);
        }

        // The long-poll subscription primitive (#1131). Blocks the calling thread
        // until at least one event matches `query`, `timeout` elapses, or
        // `cancelled()` returns true, and returns the same consistent snapshot
        // QueryWithCursor would — so a caller that times out still gets the cursor to
        // resume from, and one that matches gets every match recorded so far, not
        // just the one that woke it. The ring is re-scanned only when something was
        // recorded since the last scan; a wake that only re-checks the deadline or
        // the cancellation token costs nothing.
        //
        // `cancelled` is polled at most every kWaitSlice while blocked, and is called
        // WITH the ring's mutex held: it must be cheap and must not touch this log
        // (an atomic flag read is the intended shape — a call scope's cancellation
        // token). Never call from the game thread: the thread that records is the
        // one this would wait on.
        template<typename Cancelled>
        [[nodiscard]] DiagnosticEventQueryResult WaitWithCursor(const DiagnosticEventQuery& query,
                                                                std::chrono::milliseconds timeout,
                                                                Cancelled&& cancelled) const
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            std::unique_lock lock(m_Mutex);
            DiagnosticEventQueryResult result = QueryWithCursorLocked(query);
            u64 scannedNextId = m_NextId;
            for (;;)
            {
                if (!result.Events.empty())
                    return result;
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline || cancelled())
                    return result;
                m_Changed.wait_until(lock, std::min(deadline, now + kWaitSlice));
                if (m_NextId != scannedNextId)
                {
                    result = QueryWithCursorLocked(query);
                    scannedNextId = m_NextId;
                }
            }
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
            {
                std::lock_guard lock(m_Mutex);
                m_Events.clear();
                m_NextId = 1;
            }
            m_Changed.notify_all();
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

        // RAII suppression of ONE category, on the calling thread only. A
        // transaction (#1127) runs its steps through the registry on the main
        // thread inside one marshaled job; each step would otherwise publish its
        // own `command_completed` — up to 64 records that the batch's own
        // completion already summarises, and enough to evict what a waiter is
        // blocked on. Thread-local so a command completing on a handler thread
        // meanwhile is unaffected. Nests by depth.
        class SuppressCategoryScope
        {
          public:
            explicit SuppressCategoryScope(DiagnosticEventCategory category)
                : m_Index(static_cast<std::size_t>(category))
            {
                ++t_CategorySuppressDepth[m_Index];
            }
            ~SuppressCategoryScope()
            {
                --t_CategorySuppressDepth[m_Index];
            }
            SuppressCategoryScope(const SuppressCategoryScope&) = delete;
            SuppressCategoryScope& operator=(const SuppressCategoryScope&) = delete;
            SuppressCategoryScope(SuppressCategoryScope&&) = delete;
            SuppressCategoryScope& operator=(SuppressCategoryScope&&) = delete;

          private:
            std::size_t m_Index;
        };

        // The ring's fixed window. Public so a consumer can state it ("the newest
        // 512") instead of guessing.
        static constexpr std::size_t kCapacity = 512;

      private:
        DiagnosticsEventLog() = default;

        // How often a blocked WaitWithCursor re-checks its cancellation token.
        static constexpr std::chrono::milliseconds kWaitSlice{ 250 };

        // Shared filter for Query / QueryWithCursor. Caller must hold m_Mutex (m_Mutex
        // is not recursive). Oldest-first; SinceId lower bound (a binary search, since
        // ids are monotonic in the deque), then category filter, then the
        // newest-MaxCount cap.
        [[nodiscard]] std::vector<DiagnosticEvent> QueryLocked(const DiagnosticEventQuery& query) const
        {
            auto begin = m_Events.begin();
            if (query.SinceId != 0)
            {
                begin = std::upper_bound(m_Events.begin(), m_Events.end(), query.SinceId,
                                         [](u64 id, const DiagnosticEvent& event)
                                         { return id < event.Id; });
            }

            std::vector<DiagnosticEvent> matched;
            for (auto it = begin; it != m_Events.end(); ++it)
            {
                if (!query.Categories.empty() &&
                    std::find(query.Categories.begin(), query.Categories.end(), it->Category) == query.Categories.end())
                    continue;
                matched.push_back(*it);
            }

            if (query.MaxCount != 0 && matched.size() > query.MaxCount)
                matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(query.MaxCount));
            return matched;
        }

        [[nodiscard]] DiagnosticEventQueryResult QueryWithCursorLocked(const DiagnosticEventQuery& query) const
        {
            DiagnosticEventQueryResult result{ QueryLocked(query), m_NextId - 1, 0 };
            if (query.SinceId != 0)
            {
                // The first id still retained. An empty ring means no gap: only
                // Clear() empties it, and Clear() restarts the ids.
                const u64 oldestRetained = m_Events.empty() ? m_NextId : m_Events.front().Id;
                if (oldestRetained > query.SinceId + 1)
                    result.Dropped = oldestRetained - query.SinceId - 1;
            }
            return result;
        }

        mutable std::mutex m_Mutex;
        mutable std::condition_variable m_Changed;
        std::deque<DiagnosticEvent> m_Events;
        u64 m_NextId = 1;
        std::atomic<u32> m_SuppressDepth{ 0 };
        // Per-thread, per-category suppression depth (SuppressCategoryScope).
        inline static thread_local std::array<u32, kDiagnosticEventCategoryCount> t_CategorySuppressDepth{};
    };
} // namespace OloEngine
