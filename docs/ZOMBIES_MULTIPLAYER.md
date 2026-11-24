# Zombies & Multiplayer Co-op / LAN Guide (Black Ops II on macOS)

## Overview
Black Ops II on macOS features native Darwin POSIX UDP networking (`runtime/win32/dw_local.cpp`).
Direct peer-to-peer and client-server connectivity is supported across:
- **Local Area Network (LAN / Wi-Fi / Ethernet)**
- **Virtual Private Networks (Tailscale, WireGuard, ZeroTier)**

---

## 1. Hosting a Match

### Step 1: Start the Game
Start Black Ops II Zombies or Multiplayer with the command pipe enabled (standard in `play_steam.sh`):
```bash
# For Zombies:
./scripts/play_steam.sh zms

# For Multiplayer:
./scripts/play_steam.sh mps
```

### Step 2: Load a Map
From the game menu or via the command pipe, load a map to start the listen server:
```bash
# For Zombies (e.g. Green Run / Tranzit):
./scripts/sendcmd /tmp/bo2zms.cmd "map zm_transit"

# Or Town survival:
./scripts/sendcmd /tmp/bo2zms.cmd "map zm_transit_dr"

# For Multiplayer (e.g. Nuketown):
./scripts/sendcmd /tmp/bo2mps.cmd "map mp_nuketown_2020"
```

### Step 3: Find Your Host IP
Run the connection helper without arguments to view your reachable IP addresses:
```bash
./scripts/bo2_connect.sh
```
It will display your active network interfaces:
- **LAN / Wi-Fi (`en0`)**: e.g., `192.168.1.100` (for friends on your local Wi-Fi / router)
- **Tailscale**: e.g., `100.x.y.z` (for friends playing remotely over Tailscale)
- **Port**: `3074` (default UDP port)

Share your IP address with the players who will join you.

---

## 2. Joining a Match

### Method A: Using the Connection Helper (Recommended)
While your game client is at the main menu, run:
```bash
./scripts/bo2_connect.sh <HOST_IP>
```
Examples:
```bash
# Over local Wi-Fi:
./scripts/bo2_connect.sh 192.168.1.100

# Over Tailscale:
./scripts/bo2_connect.sh 100.85.12.34

# With custom port:
./scripts/bo2_connect.sh 192.168.1.100:3075
```

The script automatically:
1. Pings and probes the host's UDP socket via `tools/zm_probe.py` to confirm the server is running and reachable.
2. Injects the `connect <HOST_IP>:<PORT>` command into the active game command pipe (`/tmp/bo2zms.cmd` or `/tmp/bo2mps.cmd`).
3. Your client immediately loads into the host's lobby or game.

### Method B: Manual Console Pipe
If you prefer raw console pipe commands:
```bash
./scripts/sendcmd /tmp/bo2zms.cmd "connect <HOST_IP>:3074"
```

---

## 3. Remote Play Over Tailscale (Recommended for Internet Co-op)

Tailscale creates direct, encrypted, zero-config peer-to-peer UDP connections between Macs without port forwarding or router firewall changes:

1. **Install Tailscale** on each player's Mac (`brew install tailscale` or from the Mac App Store).
2. **Sign in** and verify all players are on the same Tailnet (or share nodes via Tailscale Share).
3. **Host Player**:
   - Starts the game and loads the map.
   - Finds their Tailscale IP via `./scripts/bo2_connect.sh` (or `tailscale ip -4`).
4. **Joining Players**:
   - Run `./scripts/bo2_connect.sh <HOST_TAILSCALE_IP>`.
5. Packets travel directly over Darwin POSIX UDP sockets through Tailscale's WireGuard tunnel with minimal latency.

---

## 4. Diagnostics & Troubleshooting

### Probe Server Responsiveness
You can verify if a host is listening before launching or connecting:
```bash
# Query challenge token:
python3 tools/zm_probe.py <HOST_IP> 3074

# Query server info and metadata:
python3 tools/zm_probe.py <HOST_IP> 3074 -q getinfo
```

Expected output for an active server:
```
[challengeResponse] from 192.168.1.100:3074 in 1.45 ms
  Challenge: 1337042
```

### Self-Test UDP Subsystem
Verify the Darwin UDP sockets and responder engine on your machine:
```bash
python3 tools/zm_probe.py --self-test
```

### Port Customization
By default, the game binds to UDP port `3074`. If port 3074 is already occupied, the Darwin socket runtime automatically falls back to an ephemeral port. You can override the port by setting `BO2_PORT`:
```bash
BO2_PORT=3075 ./scripts/play_steam.sh zms
```
