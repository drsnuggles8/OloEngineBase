#pragma once

#include "OloEngine/Core/Log.h"

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
