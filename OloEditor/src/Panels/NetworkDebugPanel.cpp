#include "NetworkDebugPanel.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Core/NetworkSession.h"
#include "OloEngine/Networking/Core/NetworkLobby.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Debug/Profiler.h"

#include <imgui.h>
#include <steam/steamnetworkingsockets.h>

namespace OloEngine
{
    void NetworkDebugPanel::OnImGuiRender()
    {
        OLO_PROFILE_FUNCTION();

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

        // Statistics
        auto stats = NetworkManager::GetStats();
        if (stats)
        {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("Messages Sent:     %u", stats->TotalMessagesSent);
                ImGui::Text("Messages Received: %u", stats->TotalMessagesReceived);
                ImGui::Text("Bytes Sent:        %llu", stats->TotalBytesSent);
                ImGui::Text("Bytes Received:    %llu", stats->TotalBytesReceived);
                ImGui::Separator();
                ImGui::Text("Send Rate:    %.1f msg/s  (%.1f KB/s)", stats->MessagesSentPerSec,
                            stats->BytesSentPerSec / 1024.0f);
                ImGui::Text("Recv Rate:    %.1f msg/s  (%.1f KB/s)", stats->MessagesReceivedPerSec,
                            stats->BytesReceivedPerSec / 1024.0f);
            }
        }

        // Connected peers (server only)
        if (auto const* server = NetworkManager::GetServer())
        {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Connected Peers", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (server->GetConnectionCount() == 0)
                {
                    ImGui::TextDisabled("No clients connected");
                }
                else if (
                    ImGui::BeginTable("PeersTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Client ID");
                    ImGui::TableSetupColumn("State");
                    ImGui::TableSetupColumn("Ping");
                    ImGui::TableHeadersRow();

                    server->ForEachConnection([](HSteamNetConnection handle, const NetworkConnection& conn)
                                              {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", conn.GetClientID());

                        ImGui::TableNextColumn();
                        const char* stateStr = "Unknown";
                        switch (conn.GetState())
                        {
                            case EConnectionState::Connecting:
                                stateStr = "Connecting";
                                break;
                            case EConnectionState::Connected:
                                stateStr = "Connected";
                                break;
                            case EConnectionState::ClosedByPeer:
                                stateStr = "Closed";
                                break;
                            default:
                                break;
                        }
                        ImGui::Text("%s", stateStr);

                        ImGui::TableNextColumn();
                        SteamNetConnectionRealTimeStatus_t status;
                        auto* gns = SteamNetworkingSockets();
                        if (gns && gns->GetConnectionRealTimeStatus(handle, &status, 0, nullptr) == k_EResultOK)
                        {
                            ImGui::Text("%d ms", status.m_nPing);
                        }
                        else
                        {
                            ImGui::TextDisabled("N/A");
                        } });
                    ImGui::EndTable();
                }
            }
        }

        // Session info
        if (auto const* session = NetworkManager::GetSession())
        {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Session", ImGuiTreeNodeFlags_DefaultOpen))
            {
                const char* stateStr = "None";
                switch (session->GetState())
                {
                    case ESessionState::Lobby:
                        stateStr = "Lobby";
                        break;
                    case ESessionState::Loading:
                        stateStr = "Loading";
                        break;
                    case ESessionState::InGame:
                        stateStr = "In Game";
                        break;
                    case ESessionState::PostGame:
                        stateStr = "Post Game";
                        break;
                    default:
                        break;
                }
                ImGui::Text("Session: %s", session->GetSessionName().c_str());
                ImGui::Text("State:   %s", stateStr);
                ImGui::Text("Players: %u", session->GetPlayerCount());
                ImGui::Text("All Ready: %s", session->AreAllPlayersReady() ? "Yes" : "No");
            }
        }

        // Lobby info
        if (auto const* lobby = NetworkManager::GetLobby())
        {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Lobby"))
            {
                ImGui::Text("Lobby: %s", lobby->GetLobbyName().c_str());
                ImGui::Text("Hosting: %s", lobby->IsHosting() ? "Yes" : "No");
                ImGui::Text("In Lobby: %s", lobby->IsInLobby() ? "Yes" : "No");
                ImGui::Text("Ready: %s", lobby->IsReady() ? "Yes" : "No");
                ImGui::Text("Port: %u", lobby->GetPort());
                ImGui::Text("Max Players: %u", lobby->GetMaxPlayers());
            }
        }

        ImGui::End();
    }
} // namespace OloEngine
