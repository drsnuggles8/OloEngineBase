#include "OloEnginePCH.h"
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
    void NetworkDebugPanel::OnImGuiRender(bool* p_open) const
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
        else
        {
            // No additional handling required.
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

        // Replication loop — the state that tells you at a glance whether the
        // server-authoritative loop is actually RUNNING, which is precisely what
        // this subsystem had no way to show before (issue #636).
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Replication Loop", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const bool sceneRegistered = NetworkManager::GetActiveScene() != nullptr;
            if (!sceneRegistered)
            {
                // Without a registered scene the whole loop early-outs. Say so
                // loudly: a silent no-op here is exactly how this stayed dead.
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                                   "No active scene registered — replication is idle.");
                ImGui::TextDisabled("Enter Play mode (the editor registers the runtime scene).");
            }

            const auto& serverDriver = NetworkManager::GetServerDriver();
            ImGui::Text("Snapshot rate:     %u Hz", NetworkManager::GetSnapshotRate());
            ImGui::Text("Replication tick:  %u", NetworkManager::GetCurrentTick());
            ImGui::Text("Interest scoping:  %s", serverDriver.IsInterestScopingEnabled() ? "on" : "off");

            if (isServer)
            {
                const auto tracked = serverDriver.GetTrackedClients();
                ImGui::Text("Tracked clients:   %zu", tracked.size());
                for (u32 const clientID : tracked)
                {
                    ImGui::BulletText("client %u -> pawn %llu", clientID,
                                      static_cast<unsigned long long>(serverDriver.GetPlayerEntity(clientID)));
                }
            }

            if (isClient)
            {
                const auto& clientDriver = NetworkManager::GetClientDriver();
                ImGui::Text("Local client ID:   %u", clientDriver.GetLocalClientID());
                ImGui::Text("Last server tick:  %u", clientDriver.GetLastReceivedServerTick());
                ImGui::Text("Input tick:        %u", clientDriver.GetCurrentInputTick());
                ImGui::Text("Spawned entities:  %zu", clientDriver.GetLocallySpawnedEntities().size());
            }
        }

        // Statistics
        if (auto stats = NetworkManager::GetStats(); stats)
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

            // Snapshot the connections FIRST, then query each ping. Asking for a
            // ping from inside ForEachConnection deadlocks: the iteration holds the
            // server's non-recursive mutex and GetClientPingMs takes it again. That
            // is what this panel used to do, so opening it against a live server
            // hung the editor.
            std::vector<PeerEntry> peerSnapshot;
            for (const auto& info : server->GetConnectionSnapshot())
            {
                peerSnapshot.push_back({ info.ClientID, info.State, server->GetClientPingMs(info.Handle) });
            }

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
                else
                {
                    // No additional handling required.
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
