#include "OloEngine/Networking/Core/NetworkLobby.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Serialization/Archive.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
using SocketType = SOCKET;
using socklen_t = int;
static constexpr SocketType kInvalidSocket = INVALID_SOCKET;
static constexpr int kSocketError = SOCKET_ERROR;
static inline int GetLastSocketError()
{
    return WSAGetLastError();
}
static inline void CloseSocketHandle(SocketType s)
{
    closesocket(s);
}
static inline void SetNonBlocking(SocketType s)
{
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
using SocketType = int;
static constexpr SocketType kInvalidSocket = -1;
static constexpr int kSocketError = -1;
static inline int GetLastSocketError()
{
    return errno;
}
static inline void CloseSocketHandle(SocketType s)
{
    close(s);
}
static inline void SetNonBlocking(SocketType s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }
}
#endif

namespace OloEngine
{
    // Discovery protocol constants
    static constexpr u32 kDiscoveryMagic = 0x4F4C4F44; // "OLOD"
    static constexpr u8 kProbeRequest = 0x01;
    static constexpr u8 kProbeResponse = 0x02;
    static constexpr u32 kDiscoveryTimeoutMs = 500;

    // ── Helpers ──────────────────────────────────────────────────────

    static SocketType CreateNonBlockingUDPSocket()
    {
        SocketType sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kInvalidSocket)
        {
            return kInvalidSocket;
        }

        SetNonBlocking(sock);
        return sock;
    }

    // ── Lifecycle ────────────────────────────────────────────────────

    NetworkLobby::NetworkLobby() = default;

    NetworkLobby::~NetworkLobby()
    {
        if (m_DiscoverySocket != UINT64_MAX)
        {
            CloseSocketHandle(static_cast<SocketType>(m_DiscoverySocket));
            m_DiscoverySocket = UINT64_MAX;
        }
    }

    void NetworkLobby::CreateLobby(const std::string& name, u16 port, u32 maxPlayers)
    {
        m_LobbyName = name;
        m_Port = port;
        m_MaxPlayers = maxPlayers;
        m_Hosting = true;
        m_InLobby = true;
        m_Ready = false;

        // Open the discovery beacon socket bound to kDiscoveryPort
        SocketType sock = CreateNonBlockingUDPSocket();
        if (sock != kInvalidSocket)
        {
            // Allow address reuse so multiple hosts on the same machine work in dev
            int reuse = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

            sockaddr_in bindAddr{};
            bindAddr.sin_family = AF_INET;
            bindAddr.sin_addr.s_addr = INADDR_ANY;
            bindAddr.sin_port = htons(kDiscoveryPort);

            if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == kSocketError)
            {
                OLO_CORE_WARN("[NetworkLobby] Failed to bind discovery beacon on port {} (error {})", kDiscoveryPort,
                              GetLastSocketError());
                CloseSocketHandle(sock);
            }
            else
            {
                m_DiscoverySocket = static_cast<u64>(sock);
                OLO_CORE_INFO("[NetworkLobby] Discovery beacon active on UDP port {}", kDiscoveryPort);
            }
        }
        else
        {
            OLO_CORE_WARN("[NetworkLobby] Could not create discovery socket — LAN discovery disabled");
        }
    }

    void NetworkLobby::CloseLobby()
    {
        if (m_DiscoverySocket != UINT64_MAX)
        {
            CloseSocketHandle(static_cast<SocketType>(m_DiscoverySocket));
            m_DiscoverySocket = UINT64_MAX;
        }

        m_Hosting = false;
        m_InLobby = false;
        m_Ready = false;
        m_LobbyName.clear();
        m_Port = 0;
        m_MaxPlayers = 0;
    }

    // ── LAN Discovery ────────────────────────────────────────────────

    void NetworkLobby::PollDiscovery()
    {
        if (m_DiscoverySocket == UINT64_MAX || !m_Hosting)
        {
            return;
        }

        auto sock = static_cast<SocketType>(m_DiscoverySocket);

        char recvBuf[8];
        sockaddr_in senderAddr{};
        socklen_t addrLen = sizeof(senderAddr);

        int received =
            recvfrom(sock, recvBuf, sizeof(recvBuf), 0, reinterpret_cast<sockaddr*>(&senderAddr), &addrLen);

        if (received < static_cast<int>(sizeof(u32) + sizeof(u8)))
        {
            return; // No data or too small
        }

        // Validate magic + request type
        u32 magic = 0;
        std::memcpy(&magic, recvBuf, sizeof(u32));
        if (magic != kDiscoveryMagic || recvBuf[4] != kProbeRequest)
        {
            return;
        }

        // Build response: magic(4) + type(1) + gamePort(2) + playerCount(4) + maxPlayers(4) + nameLen(1) + name(N)
        std::vector<u8> response;
        FMemoryWriter writer(response);
        writer.ArIsNetArchive = true;

        u32 responseMagic = kDiscoveryMagic;
        u8 responseType = kProbeResponse;
        u32 playerCount = 0; // Caller should set this via future API if needed
        u8 nameLen = static_cast<u8>(std::min<size_t>(m_LobbyName.size(), 255));

        writer << responseMagic;
        writer << responseType;
        writer << m_Port;
        writer << playerCount;
        writer << m_MaxPlayers;
        writer << nameLen;

        if (nameLen > 0)
        {
            response.insert(response.end(), m_LobbyName.begin(), m_LobbyName.begin() + nameLen);
        }

        sendto(sock, reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0,
               reinterpret_cast<sockaddr*>(&senderAddr), addrLen);
    }

    void NetworkLobby::FindLobbies(std::function<void(const std::vector<LobbyInfo>&)> callback)
    {
        if (!callback)
        {
            return;
        }

        // Create a temporary UDP socket for sending the broadcast probe
        SocketType sock = CreateNonBlockingUDPSocket();
        if (sock == kInvalidSocket)
        {
            OLO_CORE_WARN("[NetworkLobby] FindLobbies: cannot create UDP socket — returning empty");
            callback({});
            return;
        }

        // Enable broadcast
        int bcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

        // Bind to any port so we can receive responses
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = INADDR_ANY;
        bindAddr.sin_port = 0;
        bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr));

        // Send broadcast probe
        sockaddr_in broadcastAddr{};
        broadcastAddr.sin_family = AF_INET;
        broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
        broadcastAddr.sin_port = htons(kDiscoveryPort);

        u8 probe[5];
        std::memcpy(probe, &kDiscoveryMagic, sizeof(u32));
        probe[4] = kProbeRequest;

        sendto(sock, reinterpret_cast<const char*>(probe), sizeof(probe), 0,
               reinterpret_cast<sockaddr*>(&broadcastAddr), sizeof(broadcastAddr));

        // Collect responses with a timeout using select()
        std::vector<LobbyInfo> results;

        fd_set readSet;
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = static_cast<long>(kDiscoveryTimeoutMs * 1000);

        for (;;)
        {
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);

            int ready = select(static_cast<int>(sock + 1), &readSet, nullptr, nullptr, &tv);
            if (ready <= 0)
            {
                break; // Timeout or error — done collecting
            }

            char recvBuf[512];
            sockaddr_in senderAddr{};
            socklen_t addrLen = sizeof(senderAddr);

            int received =
                recvfrom(sock, recvBuf, sizeof(recvBuf), 0, reinterpret_cast<sockaddr*>(&senderAddr), &addrLen);

            // Minimum response: magic(4) + type(1) + port(2) + playerCount(4) + maxPlayers(4) + nameLen(1) = 16
            if (received < 16)
            {
                continue;
            }

            auto const* raw = reinterpret_cast<const u8*>(recvBuf);
            FMemoryReader reader(raw, static_cast<i64>(received));
            reader.ArIsNetArchive = true;

            u32 magic = 0;
            u8 type = 0;
            u16 gamePort = 0;
            u32 playerCount = 0;
            u32 maxPlayers = 0;
            u8 nameLen = 0;

            reader << magic;
            reader << type;
            reader << gamePort;
            reader << playerCount;
            reader << maxPlayers;
            reader << nameLen;

            if (reader.IsError() || magic != kDiscoveryMagic || type != kProbeResponse)
            {
                continue;
            }

            // Read lobby name from remaining bytes
            std::string name;
            i64 const remaining = reader.TotalSize() - reader.Tell();
            u8 const actualNameLen = static_cast<u8>(std::min<i64>(nameLen, remaining));
            if (actualNameLen > 0)
            {
                name.assign(reinterpret_cast<const char*>(raw + reader.Tell()), actualNameLen);
            }

            // Convert sender IP to string
            char addrStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, addrStr, sizeof(addrStr));

            LobbyInfo info;
            info.Name = std::move(name);
            info.HostAddress = addrStr;
            info.HostPort = gamePort;
            info.PlayerCount = playerCount;
            info.MaxPlayers = maxPlayers;
            results.push_back(std::move(info));

            // Reduce remaining timeout for next iteration (rough: halve it)
            tv.tv_usec /= 2;
            if (tv.tv_usec < 10000)
            {
                break; // Less than 10 ms left — stop
            }
        }

        CloseSocketHandle(sock);
        callback(results);
    }

    // ── Join / Leave ─────────────────────────────────────────────────

    bool NetworkLobby::JoinLobby(const LobbyInfo& lobby)
    {
        if (m_InLobby)
        {
            return false;
        }
        m_LobbyName = lobby.Name;
        m_Port = lobby.HostPort;
        m_MaxPlayers = lobby.MaxPlayers;
        m_InLobby = true;
        m_Hosting = false;
        m_Ready = false;
        return true;
    }

    void NetworkLobby::LeaveLobby()
    {
        m_InLobby = false;
        m_Hosting = false;
        m_Ready = false;
        m_LobbyName.clear();
        m_Port = 0;
        m_MaxPlayers = 0;
    }

    void NetworkLobby::SetReady(bool ready)
    {
        m_Ready = ready;
    }

    // ── Query ────────────────────────────────────────────────────────

    bool NetworkLobby::IsHosting() const
    {
        return m_Hosting;
    }

    bool NetworkLobby::IsInLobby() const
    {
        return m_InLobby;
    }

    bool NetworkLobby::IsReady() const
    {
        return m_Ready;
    }

    const std::string& NetworkLobby::GetLobbyName() const
    {
        return m_LobbyName;
    }

    u32 NetworkLobby::GetMaxPlayers() const
    {
        return m_MaxPlayers;
    }

    u16 NetworkLobby::GetPort() const
    {
        return m_Port;
    }
} // namespace OloEngine
