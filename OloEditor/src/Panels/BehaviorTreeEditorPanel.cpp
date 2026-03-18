#include "BehaviorTreeEditorPanel.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/AIRegistry.h"

#include <imgui.h>

namespace OloEngine
{
    BehaviorTreeEditorPanel::BehaviorTreeEditorPanel(const Ref<Scene>& context)
        : m_Context(context)
    {
    }

    void BehaviorTreeEditorPanel::SetContext(const Ref<Scene>& context)
    {
        m_Context = context;
    }

    void BehaviorTreeEditorPanel::OnImGuiRender()
    {
        ImGui::Begin("Behavior Tree Editor");

        if (!m_Context)
        {
            ImGui::Text("No scene loaded");
            ImGui::End();
            return;
        }

        // List all entities with BehaviorTreeComponent
        auto view = m_Context->GetAllEntitiesWith<BehaviorTreeComponent, TagComponent>();
        if (view.size_hint() == 0)
        {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No entities with Behavior Tree components");
            ImGui::End();
            return;
        }

        ImGui::Text("Entities with Behavior Trees:");
        ImGui::Separator();

        for (auto entityId : view)
        {
            auto const& tag = view.get<TagComponent>(entityId);
            auto& btc = view.get<BehaviorTreeComponent>(entityId);

            bool open = ImGui::TreeNode(reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<u32>(entityId))),
                                        "%s", tag.Tag.c_str());
            if (open)
            {
                if (btc.BehaviorTreeAssetHandle != 0)
                {
                    ImGui::Text("Asset Handle: %llu", static_cast<unsigned long long>(static_cast<u64>(btc.BehaviorTreeAssetHandle)));
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No asset assigned");
                }

                ImGui::Text("Status: %s", btc.IsRunning ? "Running" : "Stopped");

                // Blackboard viewer
                if (ImGui::TreeNode("Blackboard"))
                {
                    auto const& data = btc.Blackboard.GetAll();
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

        // Registered node types
        ImGui::Separator();
        if (ImGui::TreeNode("Registered Node Types"))
        {
            auto const& types = BTNodeRegistry::GetRegisteredTypes();
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
