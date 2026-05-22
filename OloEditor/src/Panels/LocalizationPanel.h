#pragma once

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine
{
    // Editor-side translation-management panel.
    //
    // Provides:
    //   - Active-locale dropdown
    //   - Side-by-side comparison of the active locale's keys/values against
    //     a configurable "source" locale (default: the first loaded locale,
    //     usually English) — missing translations are highlighted
    //   - Reload-from-disk button for hot-iterating .ololocale files
    //   - Key/prefix filter
    //
    // CSV export and external-translator round-trip are intentionally out of
    // scope for this slice (see Issue #175 follow-up).
    class LocalizationPanel
    {
      public:
        LocalizationPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);

        // Optional: an explicit directory to scan via
        // LocalizationManager::Initialize. Leaving this empty (the default)
        // keeps whatever the engine already loaded; calling SetDirectory()
        // with a path triggers a one-time scan on the next render.
        void SetDirectory(const std::filesystem::path& dir);

      private:
        void RenderToolbar();
        void RenderLocaleSwitcher();
        void RenderKeyTable();
        void RenderReportsSection();
        void RefreshKeyList();
        void RefreshLintCache();

        std::filesystem::path m_PendingScanDir;
        bool m_NeedsScan = false;

        // Source locale used as the reference for missing-translation
        // detection. Empty means "the alphabetically first loaded locale".
        std::string m_SourceLocale;

        // Combined, deduplicated sorted list of all keys across all loaded
        // locales. Cached and rebuilt when the LocalizationManager generation
        // moves or the user changes filters.
        std::vector<std::string> m_KeyCache;
        u64 m_KeyCacheGeneration = 0;

        // Filter / search buffer (ImGui input).
        char m_KeyFilter[128] = "";

        // Only show keys that are present in the source locale but missing
        // in the active locale.
        bool m_ShowMissingOnly = false;

        // Edit-in-place state. Keyed by (localeCode, key); values mirror the
        // ImGui InputText buffers so users can edit cells inline. Committed
        // back into LocalizationManager::SetKey on Enter/blur. Saving to
        // disk is an explicit "Save edits" button — protects against
        // accidental file writes mid-edit.
        std::unordered_map<std::string, std::string> m_EditBuffers; // key = locale "|" stringKey
        bool m_HasUnsavedEdits = false;

        // Cached lint findings (parameter-drift). Refreshed on demand and
        // whenever the manager generation moves.
        std::vector<std::string> m_LintCache;
        u64 m_LintCacheGeneration = 0;

        // Cached missing-key snapshot. Same refresh policy as the lint
        // cache — pulled from LocalizationManager::GetMissingKeysSnapshot
        // so a single ImGui frame draws against a stable view.
        std::vector<std::string> m_MissingKeysCache;

        // Transient status banner.
        std::string m_StatusMessage;
        f32 m_StatusTimer = 0.0f;
    };
} // namespace OloEngine
