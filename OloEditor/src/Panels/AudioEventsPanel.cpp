#include "AudioEventsPanel.h"

#include "OloEngine/Audio/AudioEvents/CommandID.h"
#include "OloEngine/Project/Project.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace OloEngine
{
    void AudioEventsPanel::LoadRegistry(const std::filesystem::path& filepath)
    {
        Audio::AudioCommandRegistry tmp;
        if (tmp.Deserialize(filepath))
        {
            m_Registry = std::move(tmp);
            m_RegistryPath = filepath;
            m_Dirty = false;
        }
        else
        {
            OLO_CORE_ERROR("AudioEventsPanel: Failed to load registry from '{}'", filepath.string());
        }
    }

    void AudioEventsPanel::SaveRegistry()
    {
        if (m_RegistryPath.empty())
        {
            if (const auto project = Project::GetActive())
            {
                m_RegistryPath = Project::GetAssetDirectory() / "audio" / "AudioEvents.yaml";
            }
            else
            {
                return;
            }
        }

        // Ensure parent directory exists
        if (const auto parent = m_RegistryPath.parent_path(); !std::filesystem::exists(parent))
        {
            std::filesystem::create_directories(parent);
        }

        if (!m_Registry.Serialize(m_RegistryPath))
        {
            return;
        }

        // Sync the runtime registry if the scene is running
        if (m_ActiveScene && m_ActiveScene->IsRunning())
        {
            if (auto* reg = m_ActiveScene->GetAudioCommandRegistry())
            {
                reg->Clear();
                reg->Deserialize(m_RegistryPath);
            }
        }

        m_Dirty = false;
    }

    static const char* ActionTypeLabels[] = { "Play", "Stop", "Pause", "Resume" };
    static const char* ActionContextLabels[] = { "GameObject", "Global" };

    void AudioEventsPanel::OnImGuiRender(bool* p_open)
    {
        if (!ImGui::Begin("Audio Events", p_open))
        {
            ImGui::End();
            return;
        }

        // Toolbar
        if (ImGui::Button("Save"))
        {
            SaveRegistry();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload"))
        {
            if (!m_RegistryPath.empty())
            {
                if (m_Dirty)
                {
                    m_ShowReloadConfirm = true;
                    ImGui::OpenPopup("Confirm Reload");
                }
                else
                {
                    LoadRegistry(m_RegistryPath);
                }
            }
        }

        if (ImGui::BeginPopupModal("Confirm Reload", &m_ShowReloadConfirm, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("You have unsaved changes. Reload will discard them.");
            ImGui::Separator();
            if (ImGui::Button("Discard & Reload", ImVec2(160, 0)))
            {
                LoadRegistry(m_RegistryPath);
                m_ShowReloadConfirm = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0)))
            {
                m_ShowReloadConfirm = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (m_Dirty)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "(unsaved changes)");
        }

        ImGui::Separator();

        // New trigger input
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("##NewTriggerName", m_NewTriggerName, sizeof(m_NewTriggerName));
        ImGui::SameLine();
        if (ImGui::Button("Add Trigger"))
        {
            if (m_NewTriggerName[0] != '\0')
            {
                if (auto id = m_Registry.AddTrigger(m_NewTriggerName); id.IsValid())
                {
                    m_SelectedTrigger = id;
                    m_Dirty = true;
                }
                m_NewTriggerName[0] = '\0';
            }
        }

        ImGui::Separator();

        // Two-column layout: trigger list | action editor
        const float listWidth = 220.0f;

        // Left column: trigger list
        ImGui::BeginChild("TriggerList", ImVec2(listWidth, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        for (const auto& [id, cmd] : m_Registry.GetAllTriggers())
        {
            const bool selected = (m_SelectedTrigger.ID == id);
            if (ImGui::Selectable(cmd.DebugName.c_str(), selected))
            {
                m_SelectedTrigger = Audio::CommandID(id);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right column: selected trigger's actions
        ImGui::BeginChild("ActionEditor", ImVec2(0, 0));
        if (auto* cmd = m_Registry.GetTrigger(m_SelectedTrigger))
        {
            ImGui::Text("Trigger: %s", cmd->DebugName.c_str());
            ImGui::Text("CommandID: %u", cmd->ID.ID);

            ImGui::Separator();

            if (ImGui::Button("Delete Trigger"))
            {
                m_Registry.RemoveTrigger(m_SelectedTrigger);
                m_SelectedTrigger = {};
                m_Dirty = true;
                ImGui::EndChild();
                ImGui::End();
                return;
            }

            ImGui::SameLine();
            if (ImGui::Button("Add Action"))
            {
                Audio::TriggerAction newAction;
                m_Registry.AddAction(m_SelectedTrigger, newAction);
                m_Dirty = true;
            }

            ImGui::Separator();

            i32 removeIndex = -1;
            for (sizet i = 0, count = cmd->Actions.size(); i < count; ++i)
            {
                auto& action = cmd->Actions[i];
                ImGui::PushID(static_cast<int>(i));

                ImGui::Text("Action %zu", i);
                ImGui::SameLine(0.0f, 20.0f);
                if (ImGui::SmallButton("X##Remove"))
                {
                    removeIndex = static_cast<i32>(i);
                }

                // Type
                if (auto currentType = static_cast<int>(action.Type);
                    ImGui::Combo("Type", &currentType, ActionTypeLabels, IM_ARRAYSIZE(ActionTypeLabels)))
                {
                    action.Type = static_cast<Audio::ActionType>(currentType);
                    m_Dirty = true;
                }

                // Context
                if (auto currentContext = static_cast<int>(action.Context);
                    ImGui::Combo("Context", &currentContext, ActionContextLabels, IM_ARRAYSIZE(ActionContextLabels)))
                {
                    action.Context = static_cast<Audio::ActionContext>(currentContext);
                    m_Dirty = true;
                }

                // Audio filepath
                char pathBuf[512] = {};
                std::strncpy(pathBuf, action.AudioFilepath.c_str(), sizeof(pathBuf) - 1);
                if (ImGui::InputText("Audio File", pathBuf, sizeof(pathBuf)))
                {
                    action.AudioFilepath = pathBuf;
                    m_Dirty = true;
                }

                // Accept drag-drop from Content Browser
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                    {
                        const auto* path = static_cast<const wchar_t*>(payload->Data);
                        std::filesystem::path audioPath(path);
                        auto ext = audioPath.extension().string();
                        std::ranges::transform(ext, ext.begin(), [](unsigned char c)
                                               { return static_cast<char>(std::tolower(c)); });
                        if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac")
                        {
                            action.AudioFilepath = Project::GetAssetRelativeFileSystemPath(audioPath).string();
                            m_Dirty = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                // Volume / Pitch / Looping (only for Play actions)
                if (action.Type == Audio::ActionType::Play)
                {
                    if (ImGui::DragFloat("Volume", &action.VolumeMultiplier, 0.01f, 0.0f, 2.0f))
                    {
                        m_Dirty = true;
                    }
                    if (ImGui::DragFloat("Pitch", &action.PitchMultiplier, 0.01f, 0.1f, 3.0f))
                    {
                        m_Dirty = true;
                    }
                    if (ImGui::Checkbox("Looping", &action.Looping))
                    {
                        m_Dirty = true;
                    }
                }

                ImGui::PopID();
                ImGui::Separator();
            }

            if (removeIndex >= 0)
            {
                m_Registry.RemoveAction(m_SelectedTrigger, static_cast<u32>(removeIndex));
                m_Dirty = true;
            }
        }
        else
        {
            ImGui::TextDisabled("Select a trigger from the list to edit its actions.");
        }
        ImGui::EndChild();

        ImGui::End();
    }

} // namespace OloEngine
