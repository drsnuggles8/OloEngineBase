# OloServer Deployment Guide

## Building the Server

### Prerequisites

- CMake 3.27+
- A C++23-compatible compiler (MSVC 19.38+ or GCC 13+ or Clang 17+)
- All engine dependencies (fetched automatically via CPM/FetchContent)

### Build Commands

**Debug** (with assertions and debug symbols):

```bash
cmake --build build --target OloServer --config Debug --parallel
```

**Release** (optimised, with debug info):

```bash
cmake --build build --target OloServer --config Release --parallel
```

**Dist** (fully optimised, no debug overhead):

```bash
cmake --build build --target OloServer --config Dist --parallel
```

The resulting binary is placed at `bin/<Config>/OloServer/OloServer.exe`.

---

## Asset Requirements

The server executable **does not** need renderer assets (textures, shaders, HDRI maps). It only requires:

| Asset Type | Purpose |
|---|---|
| `.oloscene` files | Scene definitions (entities, components, scripts) |
| `mono/` directory | Mono runtime + C# script assemblies (if using C# scripting) |
| Lua scripts | If Lua scripting is used |
| `server.yaml` | Server configuration file |

**Recommended**: Create a minimal server asset package that excludes:
- `assets/cache/shader/` — SPIR-V shader cache (renderer only)
- `assets/textures/` — All textures
- `assets/models/` — 3D model files (meshes, materials)
- `assets/hdri/` — Environment maps
- `assets/sounds/` — Audio files
- `Resources/` — Editor-only resources (icons, fonts)

A simple deployment structure:

```text
server/
├── OloServer.exe
├── server.yaml
├── mono/
│   ├── lib/
│   └── etc/
├── Scenes/
│   └── MainWorld.oloscene
└── Scripts/
    └── *.lua
```

---

## Running the Server

### Basic Usage

```bash
# From the directory containing the assets:
./OloServer --port 7777 --scene Scenes/MainWorld.oloscene
```

**Important**: Run the server from the directory that contains the asset folders, or specify the working directory structure so that asset paths resolve correctly.

### Command-Line Arguments

| Flag | Default | Description |
|---|---|---|
| `--port <n>` | 7777 | Network port to listen on |
| `--max-players <n>` | 64 | Maximum simultaneous connections |
| `--tick-rate <n>` | 60 | Server simulation tick rate (Hz) |
| `--scene <path>` | *(none)* | Path to the scene file to load |
| `--config <file>` | *(none)* | Path to a YAML configuration file |
| `--log-level <level>` | Info | *(reserved — not yet implemented)* Logging verbosity |

### Configuration File (server.yaml)

```yaml
port: 7777
maxPlayers: 64
tickRate: 60
scene: "Scenes/MainWorld.oloscene"
password: ""
logLevel: Info
autoSaveInterval: 300
```

| Key | Type | Default | Description |
|---|---|---|---|
| `port` | integer | 7777 | Network port to listen on (1–65535) |
| `maxPlayers` | integer | 64 | Maximum simultaneous connections (must be > 0) |
| `tickRate` | integer | 60 | Server simulation tick rate in Hz (must be > 0) |
| `scene` | string | *(empty)* | Path to the `.oloscene` file to load on startup |
| `password` | string | *(empty)* | Password required for client connections |
| `logLevel` | string | `Info` | *(reserved — not yet implemented)* Logging verbosity |
| `autoSaveInterval` | integer | 300 | Interval in seconds between automatic scene saves. Set to `0` to disable auto-saving. When enabled, the server writes the current scene to disk at this interval (same operation as the `save` console command). |

CLI arguments override values from the config file. The `--config` flag is parsed first so the file serves as a base that CLI flags can selectively override.

---

## Server Console Commands

Once running, the server accepts text commands from stdin:

| Command | Description |
|---|---|
| `status` | Show server name and running state |
| `players` | List connected clients with IDs and ping |
| `kick <id>` | Disconnect a client by ID |
| `say <msg>` | Broadcast a message to all clients |
| `save` | Save the current scene to disk |
| `reload` | Stop the current scene and reload from disk |
| `stats` | Print a performance monitoring report (tick timing, network bandwidth) |
| `stop` / `quit` | Graceful shutdown |
| `help` | List available commands |

---

## Monitoring

The server automatically logs a monitoring report every 30 seconds containing:

- **Tick stats**: count, average duration, max duration
- **Network**: connection count, send/receive bandwidth (KB/s), message rates

Example output:

```text
=== Server Monitor Report ===
  Ticks: 1800  |  Avg: 0.42 ms  |  Max: 1.87 ms
  Connections: 12
  Net send: 45.3 KB/s  |  recv: 12.1 KB/s
  Messages sent/s: 720  |  recv/s: 180
=============================
```

Tick budget warnings are logged immediately when a single tick exceeds its time budget:

```text
Server tick exceeded budget: 18.50 ms (budget: 16.67 ms)
```

---

## Logging

All output goes to both **stdout** and the **OloEngine.log** file in the working directory. The log file is overwritten on each server start.

For production deployments, redirect stdout to a log management system or use OS-level log rotation:

```bash
# Windows — redirect to file
OloServer.exe --config server.yaml > server.log 2>&1
```

> **Note:** For Linux log rotation examples, see the
> [Future Platform Support (Linux)](#future-platform-support-linux) section.

---

## Future Platform Support (Linux)

> **Warning:** The engine **does not currently support Linux**.
> [`OloEngine/src/OloEngine/Core/PlatformDetection.h`](../OloEngine/src/OloEngine/Core/PlatformDetection.h)
> will produce a compile error on non-Windows platforms. The Docker, systemd and
> related sections below are provided as a reference for when Linux support is added.
>
> On Linux, the build output would be `bin/<Config>/OloServer/OloServer` (no extension).

### Container Deployment (Docker)

Example `Dockerfile`:

```dockerfile
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /server

COPY bin/Dist/OloServer/OloServer /server/OloServer
COPY server.yaml /server/server.yaml
COPY mono/ /server/mono/
COPY Scenes/ /server/Scenes/

EXPOSE 7777/udp

ENTRYPOINT ["./OloServer", "--config", "server.yaml"]
```

Build and run:

```bash
docker build -t oloserver .
docker run -d -p 7777:7777/udp --name oloserver oloserver
```

### Docker Compose

```yaml
version: "3.8"

services:
  oloserver:
    build: .
    ports:
      - "7777:7777/udp"
    volumes:
      - ./server.yaml:/server/server.yaml:ro
      - ./Scenes:/server/Scenes:ro
    restart: unless-stopped
    deploy:
      resources:
        limits:
          cpus: "2.0"
          memory: 2G
```

### Linux Systemd Service

Create `/etc/systemd/system/oloserver.service`:

```ini
[Unit]
Description=OloEngine Dedicated Server
After=network.target

[Service]
Type=simple
User=oloserver
WorkingDirectory=/opt/oloserver
ExecStart=/opt/oloserver/OloServer --config server.yaml
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable oloserver
sudo systemctl start oloserver

# Check logs
journalctl -u oloserver -f
```

### Linux Log Rotation

```bash
# Log rotation with logrotate
./OloServer --config server.yaml >> /var/log/oloserver/server.log 2>&1
```

---

## Windows Service

Use [NSSM](https://nssm.cc/) (Non-Sucking Service Manager) to run OloServer as a Windows service:

```powershell
nssm install OloServer "C:\server\OloServer.exe" "--config server.yaml"
nssm set OloServer AppDirectory "C:\server"
nssm set OloServer AppStdout "C:\server\logs\stdout.log"
nssm set OloServer AppStderr "C:\server\logs\stderr.log"
nssm start OloServer
```

---

## Performance Tuning

| Parameter | Recommendation |
|---|---|
| Tick rate | 30 Hz for RPGs/MMOs, 60 Hz for action games, 128 Hz for competitive FPS |
| Max players | Set based on available CPU/memory; profile under load |
| Idle timeout | Call `server->SetIdleTimeout(300.0f)` on the `NetworkServer` instance to disconnect AFK clients after 300 seconds |
| CPU affinity | Pin the server process to specific cores on multicore machines |

### Recommended Hardware (per instance)

| Scale | CPU | RAM | Network |
|---|---|---|---|
| Small (1–16 players) | 2 cores | 1 GB | 10 Mbps |
| Medium (16–64 players) | 4 cores | 4 GB | 100 Mbps |
| Large (64–256 players, zone-based) | 8+ cores | 8+ GB | 1 Gbps |
