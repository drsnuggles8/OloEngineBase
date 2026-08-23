#pragma once

#include "OloEngine/Core/Log.h"
#include "OloEngine/Math/Math.h"

#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::SettingsChangeLog
{
    // Appends a "name: before -> after" entry to `changes` if the values differ.
    // The free-function template lets callers mix booleans, scalars, and enums in
    // one panel without instantiating per-field helpers.

    inline void AppendChange(std::vector<std::string>& changes, const char* name, const bool before, const bool after)
    {
        if (before == after)
            return;

        std::ostringstream oss;
        oss << name << ": " << (before ? "true" : "false") << " -> " << (after ? "true" : "false");
        changes.emplace_back(oss.str());
    }

    // Floating-point overloads. The question a settings panel asks is "did this
    // value move at all", so a bit-for-bit comparison is the right one — but it
    // must not be spelled `==` (see docs/agent-rules/cpp-coding-quality.md §2).
    // These are exact-match non-template overloads, so every f32/f64 field in
    // every panel routes here rather than to the generic template below.
    inline void AppendChange(std::vector<std::string>& changes, const char* name, const f32 before, const f32 after)
    {
        if (Math::BitwiseEqual(before, after))
            return;

        std::ostringstream oss;
        oss << name << ": " << before << " -> " << after;
        changes.emplace_back(oss.str());
    }

    inline void AppendChange(std::vector<std::string>& changes, const char* name, const f64 before, const f64 after)
    {
        if (Math::BitwiseEqual(before, after))
            return;

        std::ostringstream oss;
        oss << name << ": " << before << " -> " << after;
        changes.emplace_back(oss.str());
    }

    template<typename T>
    void AppendChange(std::vector<std::string>& changes, const char* name, const T& before, const T& after)
    {
        if (before == after)
            return;

        std::ostringstream oss;
        oss << name << ": " << before << " -> " << after;
        changes.emplace_back(oss.str());
    }

    // Joins the accumulated changes into one comma-separated line and emits a
    // single OLO_CORE_INFO. `panelTag` becomes the log prefix (e.g.
    // "RendererSettingsPanel"). No-op when `changes` is empty so callers can
    // unconditionally invoke this at the end of OnImGuiRender.
    inline void EmitLog(const char* panelTag, const std::vector<std::string>& changes)
    {
        if (changes.empty())
            return;

        std::ostringstream joined;
        for (sizet i = 0; i < changes.size(); ++i)
        {
            if (i != 0)
                joined << ", ";
            joined << changes[i];
        }

        OLO_CORE_INFO("{}: {}", panelTag, joined.str());
    }
} // namespace OloEngine::SettingsChangeLog
