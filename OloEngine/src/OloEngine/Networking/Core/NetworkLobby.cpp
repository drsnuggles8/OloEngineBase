#include "OloEnginePCH.h"
#include "OloEngine/Networking/Core/NetworkLobby.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Serialization/Archive.h"

#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h> // GetAdaptersAddresses — enumerate per-interface broadcast targets
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
static inline bool SetNonBlocking(SocketType s)
{
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) != kSocketError;
}
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>  // IFF_UP / IFF_LOOPBACK / IFF_BROADCAST flags
#include <ifaddrs.h> // getifaddrs — enumerate per-interface broadcast targets
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
static inline bool SetNonBlocking(SocketType s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) >= 0;
}
#endif

namespace
{
    // Ensures the platform socket subsystem is initialised before the first
    // socket() call and torn down at process exit. On Windows WinSock requires
    // a WSAStartup/WSACleanup pair (both are OS-refcounted, so pairing with
    // GameNetworkingSockets' own init is safe); on POSIX this is a no-op.
    //
    // Implemented as a Meyers singleton behind EnsureSocketSubsystem() so the
    // very first CreateNonBlockingUDPSocket() call brings WinSock up — removing
    // the old "Winsock may or may not be initialised" ambiguity that made LAN
    // discovery silently fail when NetworkManager (and thus GNS) wasn't running.
    class SocketSubsystem
    {
      public:
        SocketSubsystem()
        {
#ifdef _WIN32
            WSADATA data{};
            m_Started = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
        }

        ~SocketSubsystem()
        {
#ifdef _WIN32
            if (m_Started)
            {
                WSACleanup();
            }
#endif
        }

        SocketSubsystem(const SocketSubsystem&) = delete;
        SocketSubsystem& operator=(const SocketSubsystem&) = delete;
        SocketSubsystem(SocketSubsystem&&) = delete;
        SocketSubsystem& operator=(SocketSubsystem&&) = delete;

#ifdef _WIN32
      private:
        bool m_Started = false;
#endif
    };

    static void EnsureSocketSubsystem()
    {
        static SocketSubsystem s_Subsystem;
        (void)s_Subsystem;
    }
} // namespace

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
        EnsureSocketSubsystem();

        SocketType sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kInvalidSocket)
        {
            return kInvalidSocket;
        }

        if (!SetNonBlocking(sock))
        {
            CloseSocketHandle(sock);
            return kInvalidSocket;
        }
        return sock;
    }

    // Enumerate the directed-broadcast address of every up, non-loopback IPv4
    // interface, always including the limited broadcast (255.255.255.255) as a
    // fallback. Sending the discovery probe to each directed broadcast reaches
    // hosts on every locally attached subnet — limited broadcast alone is
    // dropped by many stacks/routers and only ever hits the default interface.
    // Returns network-order in_addr values, de-duplicated.
    static std::vector<in_addr> CollectBroadcastTargets()
    {
        std::vector<in_addr> targets;

        auto pushUnique = [&targets](u32 netOrderAddr)
        {
            for (const in_addr& existing : targets)
            {
                if (existing.s_addr == netOrderAddr)
                {
                    return;
                }
            }
            in_addr addr{};
            addr.s_addr = netOrderAddr;
            targets.push_back(addr);
        };

#ifdef _WIN32
        ULONG bufLen = 15000; // Recommended starting size (MSDN GetAdaptersAddresses)
        std::vector<u8> buffer(bufLen);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

        ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufLen);
        if (result == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(bufLen);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufLen);
        }

        if (result == NO_ERROR)
        {
            for (const IP_ADAPTER_ADDRESSES* adapter = adapters; adapter; adapter = adapter->Next)
            {
                if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                {
                    continue;
                }

                for (const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast;
                     unicast = unicast->Next)
                {
                    const sockaddr* sa = unicast->Address.lpSockaddr;
                    if (!sa || sa->sa_family != AF_INET || unicast->OnLinkPrefixLength > 32)
                    {
                        continue;
                    }

                    const u32 hostAddr = ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr);
                    pushUnique(htonl(NetworkLobby::DirectedBroadcast(hostAddr, static_cast<u8>(unicast->OnLinkPrefixLength))));
                }
            }
        }
#else
        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) == 0)
        {
            for (const struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next)
            {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                {
                    continue;
                }
                if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK) ||
                    !(ifa->ifa_flags & IFF_BROADCAST) || !ifa->ifa_broadaddr)
                {
                    continue;
                }

                pushUnique(reinterpret_cast<const sockaddr_in*>(ifa->ifa_broadaddr)->sin_addr.s_addr);
            }
            freeifaddrs(ifaddr);
        }
#endif

        // Limited broadcast fallback — guarantees at least one target even if
        // interface enumeration failed or yielded nothing.
        pushUnique(htonl(INADDR_BROADCAST));
        return targets;
    }

    // ── Lifecycle ────────────────────────────────────────────────────

    u32 NetworkLobby::DirectedBroadcast(u32 hostOrderAddr, u8 prefixLen)
    {
        // Clamp defensively — a prefix > 32 is meaningless; treat it as /32
        // (the host itself). Shifting a 32-bit value by 32 is UB, so prefix 0
        // is special-cased to an all-ones host mask (limited broadcast).
        const u8 prefix = prefixLen > 32 ? 32 : prefixLen;
        const u32 mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
        return hostOrderAddr | ~mask;
    }

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
        OLO_PROFILE_FUNCTION();

        // Close any existing discovery socket to prevent leaks
        if (m_DiscoverySocket != UINT64_MAX)
        {
            CloseSocketHandle(static_cast<SocketType>(m_DiscoverySocket));
            m_DiscoverySocket = UINT64_MAX;
        }

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
        OLO_PROFILE_FUNCTION();

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
        u32 playerCount = m_PlayerCountProvider ? m_PlayerCountProvider() : 0u;
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

        int const sent = sendto(sock, reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0,
                                reinterpret_cast<sockaddr*>(&senderAddr), addrLen);
        if (sent == kSocketError)
        {
            OLO_CORE_WARN("[NetworkLobby] PollDiscovery: sendto failed (error {})", GetLastSocketError());
        }
    }

    void NetworkLobby::SetPlayerCountProvider(std::function<u32()> provider)
    {
        m_PlayerCountProvider = std::move(provider);
    }

    void NetworkLobby::FindLobbies(std::function<void(const std::vector<LobbyInfo>&)> callback) const
    {
        OLO_PROFILE_FUNCTION();

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
        if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == kSocketError)
        {
            OLO_CORE_WARN("[NetworkLobby] FindLobbies: bind failed (error {})", GetLastSocketError());
            CloseSocketHandle(sock);
            callback({});
            return;
        }

        // Send the probe to the directed broadcast of every local subnet (plus
        // limited broadcast as a fallback) so hosts on any attached interface
        // can answer — not just the default route.
        u8 probe[5];
        std::memcpy(probe, &kDiscoveryMagic, sizeof(u32));
        probe[4] = kProbeRequest;

        for (const in_addr& target : CollectBroadcastTargets())
        {
            sockaddr_in broadcastAddr{};
            broadcastAddr.sin_family = AF_INET;
            broadcastAddr.sin_addr = target;
            broadcastAddr.sin_port = htons(kDiscoveryPort);

            sendto(sock, reinterpret_cast<const char*>(probe), sizeof(probe), 0,
                   reinterpret_cast<sockaddr*>(&broadcastAddr), sizeof(broadcastAddr));
        }

        // Collect responses with a deadline-based timeout using select()
        std::vector<LobbyInfo> results;

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDiscoveryTimeoutMs);

        for (;;)
        {
            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
            {
                break;
            }

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);

            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<long>(remaining.count());

            if (int ready = select(static_cast<int>(sock + 1), &readSet, nullptr, nullptr, &tv); ready <= 0)
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
            i64 const remainingBytes = reader.TotalSize() - reader.Tell();
            if (u8 const actualNameLen = static_cast<u8>(std::min<i64>(nameLen, remainingBytes)); actualNameLen > 0)
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
