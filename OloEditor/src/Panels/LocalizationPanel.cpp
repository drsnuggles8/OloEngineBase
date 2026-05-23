#include "LocalizationPanel.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Localization/LocalizationLint.h"
#include "OloEngine/Localization/LocalizationManager.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <set>

namespace OloEngine
{
    void LocalizationPanel::SetDirectory(const std::filesystem::path& dir)
    {
        m_PendingScanDir = dir;
        m_NeedsScan = true;
    }

    void LocalizationPanel::RefreshKeyList()
    {
        // Union the key sets across every loaded locale so the table
        // surfaces every key the game uses — including ones that only exist
        // in the source locale (the missing-translation signal we want to
        // highlight in the active column).
        std::set<std::string> dedup;
        for (const auto& loc : LocalizationManager::GetAvailableLocales())
        {
            for (auto& k : LocalizationManager::GetAllKeys(loc.Code))
                dedup.insert(std::move(k));
        }
        m_KeyCache.assign(dedup.begin(), dedup.end());
    }

    void LocalizationPanel::RefreshLintCache()
    {
        m_LintCache.clear();
        for (const auto& issue : LocalizationLint::RunParameterDriftLint())
        {
            m_LintCache.push_back("[" + issue.TargetLocale + "] " + issue.Key + " — " + issue.Description);
        }
        m_MissingKeysCache = LocalizationManager::GetMissingKeysSnapshot();
    }

    void LocalizationPanel::OnImGuiRender(bool* p_open)
    {
        if (m_NeedsScan && !m_PendingScanDir.empty())
        {
            LocalizationManager::Initialize(m_PendingScanDir);
            m_NeedsScan = false;
            m_StatusMessage = std::string("Loaded ") + std::to_string(LocalizationManager::GetAvailableLocales().size()) + " locale(s) from " + m_PendingScanDir.string();
            m_StatusTimer = 4.0f;
        }

        if (m_StatusTimer > 0.0f)
            m_StatusTimer -= ImGui::GetIO().DeltaTime;

        if (!ImGui::Begin("Localization", p_open))
        {
            ImGui::End();
            return;
        }

        RenderToolbar();
        ImGui::Separator();
        RenderLocaleSwitcher();
        ImGui::Separator();

        // Refresh the cached key list / lint findings whenever the active
        // locale or any loaded table changed.
        if (const u64 gen = LocalizationManager::GetGeneration(); gen != m_KeyCacheGeneration)
        {
            RefreshKeyList();
            RefreshLintCache();
            m_KeyCacheGeneration = gen;
            // Edits authored against an older table version are stale —
            // drop them so the next render picks up fresh values.
            m_EditBuffers.clear();
            m_HasUnsavedEdits = false;
        }
        // Runtime misses don't bump the generation, so refresh the
        // missing-key snapshot every frame — it's a cheap sorted copy
        // from the manager's internal set under a shared lock.
        m_MissingKeysCache = LocalizationManager::GetMissingKeysSnapshot();

        RenderKeyTable();
        ImGui::Separator();
        RenderReportsSection();

        if (m_StatusTimer > 0.0f && !m_StatusMessage.empty())
        {
            ImGui::Separator();
            ImGui::TextWrapped("%s", m_StatusMessage.c_str());
        }

        ImGui::End();
    }

    void LocalizationPanel::RenderToolbar()
    {
        if (ImGui::Button("Reload current locale from disk"))
        {
            if (LocalizationManager::ReloadCurrentLocale())
            {
                m_StatusMessage = "Reloaded '" + LocalizationManager::GetCurrentLocale() + "' from disk.";
                m_StatusTimer = 4.0f;
            }
            else
            {
                m_StatusMessage = "Reload failed (no active locale or source file missing).";
                m_StatusTimer = 4.0f;
            }
        }
        ImGui::SameLine();
        // Save edits writes every locale whose buffer was touched back to
        // its source .ololocale file. Greyed out when there's nothing to save.
        // ImGui's disabled stack must be balanced — using the bool-arg
        // overload keeps Begin/End paired even when the click handler flips
        // m_HasUnsavedEdits inside the block.
        ImGui::BeginDisabled(!m_HasUnsavedEdits);
        if (ImGui::Button("Save edits to disk"))
        {
            std::set<std::string> touchedLocales;
            for (const auto& kv : m_EditBuffers)
            {
                const auto sep = kv.first.find('|');
                if (sep != std::string::npos)
                    touchedLocales.insert(kv.first.substr(0, sep));
            }
            // Track which locales saved successfully so we keep failed
            // ones in m_EditBuffers (the user can retry without losing
            // their unsaved edits). m_HasUnsavedEdits only clears when
            // every touched locale wrote OK.
            std::set<std::string> savedLocales;
            for (const auto& loc : touchedLocales)
            {
                if (LocalizationManager::SaveLocaleToFile(loc))
                    savedLocales.insert(loc);
            }
            const u32 savedCount = static_cast<u32>(savedLocales.size());
            const u32 totalCount = static_cast<u32>(touchedLocales.size());
            if (savedCount == totalCount)
            {
                m_EditBuffers.clear();
                m_HasUnsavedEdits = false;
                m_StatusMessage = "Saved " + std::to_string(savedCount) + " locale file(s).";
            }
            else
            {
                // Drop only the buffers for successfully-saved locales; leave
                // the rest in place so the user sees their pending edits and
                // can retry the "Save edits to disk" button.
                for (auto it = m_EditBuffers.begin(); it != m_EditBuffers.end();)
                {
                    const auto sep = it->first.find('|');
                    const std::string locCode = (sep != std::string::npos) ? it->first.substr(0, sep) : std::string{};
                    if (savedLocales.contains(locCode))
                        it = m_EditBuffers.erase(it);
                    else
                        ++it;
                }
                // m_HasUnsavedEdits stays true so the button keeps offering
                // a retry.
                m_StatusMessage = "Saved " + std::to_string(savedCount) + " / " + std::to_string(totalCount) + " locale file(s) — " + std::to_string(totalCount - savedCount) + " failed; retry?";
            }
            m_StatusTimer = 4.0f;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        // Generate / refresh the synthetic pseudo locale from the source locale.
        // Once registered it appears in the active-locale dropdown.
        if (ImGui::Button("Generate pseudo-locale"))
        {
            const auto locales = LocalizationManager::GetAvailableLocales();
            const std::string src = m_SourceLocale.empty() && !locales.empty() ? locales.front().Code : m_SourceLocale;
            if (LocalizationManager::GeneratePseudoLocale(src.empty() ? "en" : src))
            {
                m_StatusMessage = "Pseudo-locale 'pseudo' generated from '" + (src.empty() ? std::string("en") : src) + "'.";
                m_StatusTimer = 4.0f;
            }
            else
            {
                m_StatusMessage = "Pseudo-locale generation failed (source locale not loaded).";
                m_StatusTimer = 4.0f;
            }
        }

        ImGui::SameLine();
        ImGui::InputTextWithHint("##KeyFilter", "Filter keys by substring...", m_KeyFilter, IM_ARRAYSIZE(m_KeyFilter));

        ImGui::SameLine();
        ImGui::Checkbox("Missing only", &m_ShowMissingOnly);
    }

    void LocalizationPanel::RenderLocaleSwitcher()
    {
        const auto locales = LocalizationManager::GetAvailableLocales();
        if (locales.empty())
        {
            ImGui::TextDisabled("No locales loaded. Drop .ololocale files under assets/localization/ and call LocalizationManager::Initialize().");
            return;
        }

        const std::string current = LocalizationManager::GetCurrentLocale();
        if (ImGui::BeginCombo("Active locale", current.empty() ? "<none>" : current.c_str()))
        {
            for (const auto& loc : locales)
            {
                const bool selected = (loc.Code == current);
                std::string label = loc.Name.empty() ? loc.Code : (loc.Name + " (" + loc.Code + ")");
                if (ImGui::Selectable(label.c_str(), selected) && !selected)
                {
                    LocalizationManager::SetCurrentLocale(loc.Code);
                    m_StatusMessage = "Switched to locale '" + loc.Code + "'.";
                    m_StatusTimer = 3.0f;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Source-locale dropdown (the reference column for missing-key detection).
        std::string sourceShown = m_SourceLocale.empty() ? (locales.front().Code + "  (default)") : m_SourceLocale;
        if (ImGui::BeginCombo("Source locale", sourceShown.c_str()))
        {
            if (ImGui::Selectable("(alphabetically first)", m_SourceLocale.empty()))
                m_SourceLocale.clear();
            for (const auto& loc : locales)
            {
                const bool selected = (loc.Code == m_SourceLocale);
                if (ImGui::Selectable(loc.Code.c_str(), selected))
                    m_SourceLocale = loc.Code;
            }
            ImGui::EndCombo();
        }
    }

    void LocalizationPanel::RenderKeyTable()
    {
        const auto locales = LocalizationManager::GetAvailableLocales();
        if (locales.empty() || m_KeyCache.empty())
        {
            ImGui::TextDisabled("No keys to display.");
            return;
        }

        const std::string sourceLocale = m_SourceLocale.empty() ? locales.front().Code : m_SourceLocale;
        const std::string activeLocale = LocalizationManager::GetCurrentLocale();
        const std::string filterStr(m_KeyFilter);

        if (ImGui::BeginTable("LocalizationKeys", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn((std::string("Source (") + sourceLocale + ")").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn((std::string("Active (") + activeLocale + ")").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (const auto& key : m_KeyCache)
            {
                if (!filterStr.empty() && key.find(filterStr) == std::string::npos)
                    continue;

                const bool sourceHas = LocalizationManager::HasKey(key, sourceLocale);
                const bool activeHas = LocalizationManager::HasKey(key, activeLocale);
                const bool missing = sourceHas && !activeHas;
                if (m_ShowMissingOnly && !missing)
                    continue;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(key.c_str());

                ImGui::TableSetColumnIndex(1);
                if (sourceHas)
                    ImGui::TextWrapped("%s", LocalizationManager::Get(key, sourceLocale).c_str());
                else
                    ImGui::TextDisabled("(not in source)");

                ImGui::TableSetColumnIndex(2);
                {
                    // Edit-in-place: an InputText against a per-cell buffer
                    // keyed by "<localeCode>|<key>". Buffer is seeded from the
                    // current value the first time the cell is rendered;
                    // every keystroke writes back through SetKey so the rest
                    // of the panel (and other observers via the generation
                    // counter) see the live state. Save-to-disk is gated
                    // behind the toolbar button — typing alone never writes
                    // a file.
                    const std::string bufKey = activeLocale + "|" + key;
                    auto bufIt = m_EditBuffers.find(bufKey);
                    if (bufIt == m_EditBuffers.end())
                        bufIt = m_EditBuffers.emplace(bufKey, activeHas ? LocalizationManager::Get(key, activeLocale) : std::string{}).first;

                    std::string& buf = bufIt->second;
                    ImGui::PushID(bufKey.c_str());
                    if (!activeHas)
                    {
                        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.30f, 0.15f, 0.15f, 1.0f));
                    }
                    if (ImGui::InputText("##cell", &buf))
                    {
                        if (LocalizationManager::SetKey(activeLocale, key, buf))
                            m_HasUnsavedEdits = true;
                    }
                    if (!activeHas)
                        ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    void LocalizationPanel::RenderReportsSection()
    {
        if (ImGui::CollapsingHeader("Reports (lint + missing keys)"))
        {
            ImGui::TextWrapped("Parameter-drift lint (translations with %s tokens that differ from the source locale):", "{name}");
            if (m_LintCache.empty())
            {
                ImGui::TextDisabled("  (none)");
            }
            else
            {
                ImGui::BeginChild("##lint", ImVec2(0, 120), true);
                for (const auto& line : m_LintCache)
                    ImGui::TextWrapped("%s", line.c_str());
                ImGui::EndChild();
            }

            ImGui::Spacing();
            ImGui::TextWrapped("Missing-key reports (keys that runtime lookups asked for but the active locale doesn't have):");
            if (ImGui::SmallButton("Clear missing-key list"))
            {
                LocalizationManager::ClearMissingKeys();
                m_MissingKeysCache.clear();
            }
            if (m_MissingKeysCache.empty())
            {
                ImGui::TextDisabled("  (none)");
            }
            else
            {
                ImGui::BeginChild("##missing", ImVec2(0, 120), true);
                for (const auto& k : m_MissingKeysCache)
                    ImGui::TextUnformatted(k.c_str());
                ImGui::EndChild();
            }
        }
    }
} // namespace OloEngine
