#pragma once

// VulkanWarnOnceSet — a thread-safe "have I already warned about this name?"
// set for the draw path's warn-once diagnostics. Issue #806, ADR 0011
// amendment (92) rule 8.
//
// The draw path used to keep these as function-static
// std::unordered_set<std::string> objects, which was fine while every draw
// recorded on the render thread. With a parallel region several recording
// threads can miss the same name at once, and a concurrent insert into an
// unordered_set is a data race — so the set and the lock travel together.
// Header-only; the lock is held only for the insert, and a warn-once site
// reaches it at most once per distinct name plus one lookup per repeat.
//
// Usage at a warn-once site:
//
//     static VulkanWarnOnceSet s_WarnedPipelines;
//     if (s_WarnedPipelines.Insert(shader->GetName()))
//     {
//         OLO_CORE_WARN(...);
//     }

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace OloEngine
{
    class VulkanWarnOnceSet
    {
      public:
        // True the first time `name` is seen, false on every later call —
        // the std::set::insert(...).second the call sites already test.
        [[nodiscard]] bool Insert(std::string_view name)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_Names.emplace(name).second;
        }

      private:
        std::mutex m_Mutex;
        std::unordered_set<std::string> m_Names;
    };
} // namespace OloEngine
