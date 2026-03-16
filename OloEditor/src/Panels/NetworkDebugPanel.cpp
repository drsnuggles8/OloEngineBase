#include "NetworkDebugPanel.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Core/NetworkSession.h"
#include "OloEngine/Networking/Core/NetworkLobby.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"
#include "OloEngine/Networking/Transport/NetworkClient.h"
#include "OloEngine/Debug/Profiler.h"

#include <imgui.h>

namespace OloEngine
{
    void NetworkDebugPanel::OnImGuiRender(bool* p_open)
    {
        OLO_PROFILE_FUNCTION();

        if (!ImGui::Begin("Network Debug", p_open))
        {
            ImGui::End();
            return;
        }

        // Snapshot NetworkManager state once per frame to avoid repeated static calls
        bool const isServer = NetworkManager::IsServer();
        bool const isClient = NetworkManager::IsClient();
        bool const isConnected = NetworkManager::IsConnected();
        bool const isInitialized = NetworkManager::IsInitialized();

        // Connection state
        const char* mode = "None";
        if (isServer)
        {
            mode = "Server";
        }
        else if (isClient)
        {
            mode = "Client";
        }
        ImGui::Text("Mode: %s", mode);

        if (isClient)
        {
            ImGui::Text("Connected: %s", isConnected ? "Yes" : "No");
        }

        ImGui::Separator();

        // Controls
        if (!isInitialized)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.0f, 1.0f), "NetworkManager not initialized");
        }
        else
        {
            if (!isServer && !isClient)
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
                if (isServer)
                {
                    if (ImGui::Button("Stop Server"))
                    {
                        NetworkManager::StopServer();
                    }
                }

                if (isClient)
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

        // Connected peers (server only) — snapshot data to avoid holding raw pointer
        if (auto const* server = NetworkManager::GetServer())
        {
            struct PeerEntry
            {
                u32 ClientID = 0;
                EConnectionState State = EConnectionState::None;
                i32 PingMs = -1;
            };
            std::vector<PeerEntry> peerSnapshot;
            server->ForEachConnection([&peerSnapshot, server](HSteamNetConnection handle, const NetworkConnection& conn)
                                      {
                PeerEntry entry;
                entry.ClientID = conn.GetClientID();
                entry.State = conn.GetState();
                entry.PingMs = server->GetClientPingMs(handle);
                peerSnapshot.push_back(entry); });

            ImGui::Separator();
            if (ImGui::CollapsingHeader("Connected Peers", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (peerSnapshot.empty())
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

                    for (auto const& peer : peerSnapshot)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", peer.ClientID);

                        ImGui::TableNextColumn();
                        const char* stateStr = "Unknown";
                        switch (peer.State)
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
                        if (peer.PingMs >= 0)
                        {
                            ImGui::Text("%d ms", peer.PingMs);
                        }
                        else
                        {
                            ImGui::TextDisabled("N/A");
                        }
                    }
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
