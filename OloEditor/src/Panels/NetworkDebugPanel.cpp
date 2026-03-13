#include "NetworkDebugPanel.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"

#include <imgui.h>

namespace OloEngine
{
    void NetworkDebugPanel::OnImGuiRender()
    {
        ImGui::Begin("Network Debug");

        // Connection state
        const char* mode = "None";
        if (NetworkManager::IsServer())
        {
            mode = "Server";
        }
        else if (NetworkManager::IsClient())
        {
            mode = "Client";
        }
        ImGui::Text("Mode: %s", mode);

        if (NetworkManager::IsClient())
        {
            ImGui::Text("Connected: %s", NetworkManager::IsConnected() ? "Yes" : "No");
        }

        ImGui::Separator();

        // Controls
        if (!NetworkManager::IsInitialized())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.0f, 1.0f), "NetworkManager not initialized");
        }
        else
        {
            if (!NetworkManager::IsServer() && !NetworkManager::IsClient())
            {
                static u16 serverPort = 27015;
                ImGui::InputScalar("Port", ImGuiDataType_U16, &serverPort);

                if (ImGui::Button("Start Server"))
                {
                    NetworkManager::StartServer(serverPort);
                }

                ImGui::SameLine();

                static char addressBuf[128] = "127.0.0.1";
                ImGui::InputText("Address", addressBuf, sizeof(addressBuf));

                if (ImGui::Button("Connect"))
                {
                    NetworkManager::Connect(addressBuf, serverPort);
                }
            }
            else
            {
                if (NetworkManager::IsServer())
                {
                    if (ImGui::Button("Stop Server"))
                    {
                        NetworkManager::StopServer();
                    }
                }

                if (NetworkManager::IsClient())
                {
                    if (ImGui::Button("Disconnect"))
                    {
                        NetworkManager::Disconnect();
                    }
                }
            }
        }

        ImGui::End();
    }
}
