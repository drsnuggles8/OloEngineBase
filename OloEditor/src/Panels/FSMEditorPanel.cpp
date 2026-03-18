#include "FSMEditorPanel.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/AIRegistry.h"

#include <imgui.h>

namespace OloEngine
{
    FSMEditorPanel::FSMEditorPanel(const Ref<Scene>& context)
        : m_Context(context)
    {
    }

    void FSMEditorPanel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
    }

    void FSMEditorPanel::OnImGuiRender()
    {
        ImGui::Begin("State Machine Editor");

        if (!m_Context)
        {
            ImGui::Text("No scene loaded");
            ImGui::End();
            return;
        }

        // List all entities with StateMachineComponent
        auto view = m_Context->GetAllEntitiesWith<StateMachineComponent, TagComponent>();
        if (view.size_hint() == 0)
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No entities with State Machine components");
            ImGui::End();
            return;
        }

        ImGui::Text("Entities with State Machines:");
        ImGui::Separator();

        for (auto entityId : view)
        {
            auto const& tag = view.get<TagComponent>(entityId);
            auto& smc = view.get<StateMachineComponent>(entityId);

            bool open = ImGui::TreeNode(reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<u32>(entityId))),
                                        "%s", tag.Tag.c_str());
            if (open)
            {
                if (smc.StateMachineAssetHandle != 0)
                {
                    ImGui::Text("Asset Handle: %llu", static_cast<unsigned long long>(static_cast<u64>(smc.StateMachineAssetHandle)));
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No asset assigned");
                }

                if (smc.RuntimeFSM && smc.RuntimeFSM->IsStarted())
                {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                                       "Current State: %s", smc.RuntimeFSM->GetCurrentStateID().c_str());
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Not started");
                }

                // Blackboard viewer
                if (ImGui::TreeNode("Blackboard"))
                {
                    auto const& data = smc.Blackboard.GetAll();
                    if (data.empty())
                    {
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(empty)");
                    }
                    for (auto const& [key, value] : data)
                    {
                        std::string valueStr = BlackboardValueToString(value);
                        ImGui::Text("%s: %s", key.c_str(), valueStr.c_str());
                    }
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
        }

        // Registered state types
        ImGui::Separator();
        if (ImGui::TreeNode("Registered State Types"))
        {
            auto const& types = FSMStateRegistry::GetRegisteredTypes();
            for (auto const& [name, factory] : types)
            {
                ImGui::BulletText("%s", name.c_str());
            }
            if (types.empty())
            {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No types registered");
            }
            ImGui::TreePop();
        }

        ImGui::End();
    }
} // namespace OloEngine
