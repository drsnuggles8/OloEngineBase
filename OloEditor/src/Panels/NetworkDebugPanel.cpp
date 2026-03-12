#include "NetworkDebugPanel.h"
#include "OloEngine/Core/Log.h"

#include <imgui.h>

namespace OloEngine
{
    void NetworkDebugPanel::OnImGuiRender()
    {
        ImGui::Begin("Network Debug");

        // --- Connection state ---
        const bool initialized = NetworkManager::IsInitialized();
        const bool isServer    = NetworkManager::IsServer();
        const bool isClient    = NetworkManager::IsClient();
        const bool isConnected = NetworkManager::IsConnected();

        ImGui::Text("Status:  %s", initialized ? "Initialized" : "Not Initialized");
        ImGui::Text("Mode:    %s", isServer ? "Server" : (isClient && isConnected ? "Client" : "None"));
        ImGui::Text("Connected: %s", isConnected ? "Yes" : "No");

        ImGui::Separator();

        // --- Server controls ---
        ImGui::Text("Server");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputScalar("Port##Server", ImGuiDataType_U16, &m_ServerPort);

        ImGui::SameLine();
        if (ImGui::Button("Start Server"))
        {
            if (initialized)
            {
                NetworkManager::StartServer(m_ServerPort);
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Stop Server"))
        {
            NetworkManager::StopServer();
        }

        ImGui::Separator();

        // --- Client controls ---
        ImGui::Text("Client");
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Address##Client", m_ConnectAddress, IM_ARRAYSIZE(m_ConnectAddress));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputScalar("Port##Client", ImGuiDataType_U16, &m_ConnectPort);
        ImGui::SameLine();
        if (ImGui::Button("Connect"))
        {
            if (initialized)
            {
                NetworkManager::Connect(m_ConnectAddress, m_ConnectPort);
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Disconnect"))
        {
            NetworkManager::Disconnect();
        }

        ImGui::Separator();

        // --- Statistics placeholder ---
        ImGui::Text("Statistics");
        ImGui::TextDisabled("(detailed stats available after connection)");

        ImGui::End();
    }

} // namespace OloEngine
