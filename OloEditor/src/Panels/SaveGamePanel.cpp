#include "SaveGamePanel.h"

#include "OloEngine/SaveGame/ThumbnailCapture.h"

#include <imgui.h>

#include <cstring>
#include <ctime>

namespace OloEngine
{
    void SaveGamePanel::OnImGuiRender()
    {
        ImGui::Begin("Save Game");

        // Status message
        if (m_StatusTimer > 0.0f)
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "%s", m_StatusMessage.c_str());
            m_StatusTimer -= ImGui::GetIO().DeltaTime;
            ImGui::Separator();
        }

        if (ImGui::CollapsingHeader("Save", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawSaveSection();
        }

        if (ImGui::CollapsingHeader("Load", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawLoadSection();
        }

        if (ImGui::CollapsingHeader("Settings"))
        {
            DrawSettingsSection();
        }

        ImGui::End();
    }

    void SaveGamePanel::DrawSaveSection()
    {
        ImGui::InputText("Save Name", m_NewSaveName, kMaxSaveNameLength);
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu/%zu)", std::strlen(m_NewSaveName), kMaxSaveNameLength - 1);
        ImGui::Checkbox("Include Thumbnail", &m_IncludeThumbnail);

        if (ImGui::Button("Save Game", ImVec2(-1, 0)))
        {
            std::string saveName(m_NewSaveName);
            bool nameValid = !saveName.empty() && saveName.find_first_not_of(' ') != std::string::npos && saveName.find("..") == std::string::npos && saveName.find('/') == std::string::npos && saveName.find('\\') == std::string::npos;

            if (!nameValid)
            {
                m_StatusMessage = "Invalid save name!";
                m_StatusTimer = 3.0f;
            }
            else if (m_Scene)
            {
                std::vector<u8> thumbnail;
                if (m_IncludeThumbnail && m_Framebuffer)
                {
                    thumbnail = ThumbnailCapture::CaptureViewport(m_Framebuffer);
                }

                auto result = SaveGameManager::Save(*m_Scene, m_NewSaveName, m_NewSaveName, thumbnail);
                if (result == SaveLoadResult::Success)
                {
                    m_StatusMessage = "Saved: " + std::string(m_NewSaveName);
                    m_StatusTimer = 3.0f;
                    m_NeedsRefresh = true;
                }
                else
                {
                    m_StatusMessage = "Save failed!";
                    m_StatusTimer = 3.0f;
                }
            }
        }

        ImGui::Spacing();

        if (ImGui::Button("Quick Save (F5)", ImVec2(-1, 0)))
        {
            TriggerQuickSave();
        }
    }

    void SaveGamePanel::DrawLoadSection()
    {
        if (m_NeedsRefresh)
        {
            RefreshSaveList();
            m_NeedsRefresh = false;
        }

        if (ImGui::Button("Refresh"))
        {
            m_NeedsRefresh = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Quick Load (F9)"))
        {
            TriggerQuickLoad();
        }

        ImGui::Separator();

        if (m_SaveFiles.empty())
        {
            ImGui::TextDisabled("No save files found");
            return;
        }

        for (sizet i = 0; i < m_SaveFiles.size(); ++i)
        {
            const auto& save = m_SaveFiles[i];
            ImGui::PushID(static_cast<int>(i));

            char timeStr[64];
            FormatTimestamp(save.Metadata.TimestampUTC, timeStr, sizeof(timeStr));

            // Save entry
            std::string label = save.Metadata.DisplayName + " (" + timeStr + ")";
            if (ImGui::TreeNode(label.c_str()))
            {
                ImGui::Text("Scene: %s", save.Metadata.SceneName.c_str());
                ImGui::Text("Entities: %u", save.Metadata.EntityCount);
                ImGui::Text("Size: %.1f KB", static_cast<f32>(save.FileSizeBytes) / 1024.0f);

                const char* slotTypeStr = "Manual";
                switch (save.Metadata.SlotType)
                {
                    case SaveSlotType::QuickSave:
                        slotTypeStr = "Quick Save";
                        break;
                    case SaveSlotType::AutoSave:
                        slotTypeStr = "Auto Save";
                        break;
                    default:
                        break;
                }
                ImGui::Text("Type: %s", slotTypeStr);

                if (save.HasThumbnail)
                {
                    ImGui::Text("[Thumbnail available]");
                }

                // Load button
                if (ImGui::Button("Load"))
                {
                    if (m_Scene)
                    {
                        // Extract slot name from file path
                        std::string slotName = save.FilePath.stem().string();
                        auto result = SaveGameManager::Load(*m_Scene, slotName);
                        if (result == SaveLoadResult::Success)
                        {
                            m_StatusMessage = "Loaded: " + save.Metadata.DisplayName;
                            m_StatusTimer = 3.0f;
                        }
                        else
                        {
                            m_StatusMessage = "Load failed!";
                            m_StatusTimer = 3.0f;
                        }
                    }
                }

                ImGui::SameLine();

                // Delete button
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
                if (ImGui::Button("Delete"))
                {
                    m_PendingDeleteSlot = save.FilePath.stem().string();
                    ImGui::OpenPopup("Confirm Delete");
                }
                ImGui::PopStyleColor();

                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        // Modal rendered outside TreeNode so it stays open even if the node is collapsed
        if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Delete save \"%s\"?", m_PendingDeleteSlot.c_str());
            ImGui::Separator();
            if (ImGui::Button("Confirm", ImVec2(120, 0)))
            {
                if (SaveGameManager::DeleteSave(m_PendingDeleteSlot))
                {
                    m_StatusMessage = "Deleted: " + m_PendingDeleteSlot;
                    m_StatusTimer = 3.0f;
                }
                else
                {
                    m_StatusMessage = "Delete failed: " + m_PendingDeleteSlot;
                    m_StatusTimer = 3.0f;
                }
                m_NeedsRefresh = true;
                m_PendingDeleteSlot.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                m_PendingDeleteSlot.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void SaveGamePanel::DrawSettingsSection()
    {
        f32 interval = SaveGameManager::GetAutoSaveInterval();
        if (ImGui::SliderFloat("Auto-Save Interval (s)", &interval, 0.0f, 600.0f, "%.0f"))
        {
            SaveGameManager::SetAutoSaveInterval(interval);
        }

        if (interval <= 0.0f)
        {
            ImGui::TextDisabled("Auto-save disabled");
        }
        else
        {
            ImGui::Text("Auto-save every %.0f seconds", interval);
        }
    }

    void SaveGamePanel::TriggerQuickSave()
    {
        if (m_Scene)
        {
            std::vector<u8> thumbnail;
            if (m_IncludeThumbnail && m_Framebuffer)
            {
                thumbnail = ThumbnailCapture::CaptureViewport(m_Framebuffer);
            }

            auto result = SaveGameManager::QuickSave(*m_Scene, thumbnail);
            if (result == SaveLoadResult::Success)
            {
                m_StatusMessage = "Quick Save complete";
                m_StatusTimer = 2.0f;
                m_NeedsRefresh = true;
            }
            else
            {
                m_StatusMessage = "Quick Save failed!";
                m_StatusTimer = 3.0f;
            }
        }
    }

    void SaveGamePanel::TriggerQuickLoad()
    {
        if (m_Scene)
        {
            auto result = SaveGameManager::QuickLoad(*m_Scene);
            if (result == SaveLoadResult::Success)
            {
                m_StatusMessage = "Quick Load complete";
                m_StatusTimer = 2.0f;
            }
            else
            {
                m_StatusMessage = "Quick Load failed!";
                m_StatusTimer = 3.0f;
            }
        }
    }

    void SaveGamePanel::RefreshSaveList()
    {
        m_SaveFiles = SaveGameManager::EnumerateSaves();
    }

    void SaveGamePanel::FormatTimestamp(i64 timestamp, char* buffer, sizet bufferSize) const
    {
        auto time = static_cast<std::time_t>(timestamp);
        struct tm tmBuf;
#ifdef _WIN32
        localtime_s(&tmBuf, &time);
#else
        localtime_r(&time, &tmBuf);
#endif
        std::strftime(buffer, bufferSize, "%Y-%m-%d %H:%M", &tmBuf);
    }

} // namespace OloEngine
