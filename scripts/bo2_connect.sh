#!/bin/bash
# bo2_connect.sh — Helper script to connect to Black Ops II Zombies or Multiplayer host.
#
# Usage:
#   scripts/bo2_connect.sh                  # Interactive mode: shows your IP & prompts for target
#   scripts/bo2_connect.sh 192.168.1.100    # Connects to 192.168.1.100:3074
#   scripts/bo2_connect.sh 100.64.0.5:3075  # Connects to custom port
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Function to detect local network IP addresses
get_local_ips() {
    local en0_ip en1_ip tailscale_ip
    en0_ip=$(ipconfig getifaddr en0 2>/dev/null || true)
    en1_ip=$(ipconfig getifaddr en1 2>/dev/null || true)

    if command -v tailscale >/dev/null 2>&1; then
        tailscale_ip=$(tailscale ip -4 2>/dev/null || true)
    else
        # Check utun interfaces often used by Tailscale / Wireguard
        tailscale_ip=$(ifconfig 2>/dev/null | awk '/utun/ {getline; while ($0 ~ /inet /) {if ($2 ~ /^100\./) {print $2; exit} getline}}' || true)
    fi

    echo "Your Local IP Addresses (share with friends who want to join your game):"
    if [ -n "$en0_ip" ]; then
        echo "  - LAN / Wi-Fi (en0): $en0_ip"
    fi
    if [ -n "$en1_ip" ]; then
        echo "  - LAN / Ethernet (en1): $en1_ip"
    fi
    if [ -n "$tailscale_ip" ]; then
        echo "  - Tailscale VPN:       $tailscale_ip"
    fi
    if [ -z "$en0_ip" ] && [ -z "$en1_ip" ] && [ -z "$tailscale_ip" ]; then
        local any_ip
        any_ip=$(ifconfig 2>/dev/null | awk '/inet / && !/127\.0\.0\.1/ {print $2}' | head -n 1 || true)
        if [ -n "$any_ip" ]; then
            echo "  - Local IPv4:          $any_ip"
        else
            echo "  - (No active network connection detected)"
        fi
    fi
    echo "  - Default Game Port:   3074 (UDP)"
    echo ""
}

TARGET="${1:-}"

if [ -z "$TARGET" ]; then
    echo "============================================================="
    echo "    Black Ops II (macOS) Connection & Host Helper"
    echo "============================================================="
    echo ""
    get_local_ips
    read -r -p "Enter Host IP or IP:PORT to connect (or press Enter to exit): " TARGET
    if [ -z "$TARGET" ]; then
        echo "Exiting."
        exit 0
    fi
fi

# Trim whitespace
TARGET=$(echo "$TARGET" | tr -d '[:space:]')

# Parse host and port
if [[ "$TARGET" =~ : ]]; then
    HOST="${TARGET%:*}"
    PORT="${TARGET##*:}"
else
    HOST="$TARGET"
    PORT="3074"
fi

echo "Connecting to Black Ops II host at ${HOST}:${PORT}..."

# Probe server responsiveness using zm_probe.py
if [ -f "$REPO_ROOT/tools/zm_probe.py" ]; then
    echo "Probing server via UDP..."
    if python3 "$REPO_ROOT/tools/zm_probe.py" "$HOST" "$PORT" -t 2.0 -r 2; then
        echo "Server is responding and ready for connection!"
    else
        echo "Notice: Target server did not respond to UDP ping probe."
        echo "        Proceeding with connect command (server might be in map load or firewalled)."
    fi
fi

# Detect active command pipe
CMD_FIFO=""
MODE_NAME=""

if [ -p "/tmp/bo2zms.cmd" ]; then
    CMD_FIFO="/tmp/bo2zms.cmd"
    MODE_NAME="Zombies (t6zm)"
elif [ -p "/tmp/bo2mps.cmd" ]; then
    CMD_FIFO="/tmp/bo2mps.cmd"
    MODE_NAME="Multiplayer (t6mp)"
elif [ -e "/tmp/bo2zms.cmd" ]; then
    CMD_FIFO="/tmp/bo2zms.cmd"
    MODE_NAME="Zombies (file)"
elif [ -e "/tmp/bo2mps.cmd" ]; then
    CMD_FIFO="/tmp/bo2mps.cmd"
    MODE_NAME="Multiplayer (file)"
else
    CMD_FIFO="/tmp/bo2zms.cmd"
    MODE_NAME="Zombies (default)"
fi

CONNECT_CMD="connect ${HOST}:${PORT}"
echo "Sending command '${CONNECT_CMD}' to ${MODE_NAME}..."

if [ -x "$REPO_ROOT/scripts/sendcmd" ] && [ -p "$CMD_FIFO" ]; then
    "$REPO_ROOT/scripts/sendcmd" "$CMD_FIFO" "$CONNECT_CMD"
else
    if [ -p "$CMD_FIFO" ]; then
        ( printf '%s\n' "$CONNECT_CMD" > "$CMD_FIFO" ) &
        pid=$!
        for _ in $(seq 1 20); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            echo "Warning: Timed out writing to $CMD_FIFO. Is the game running?"
        else
            echo "Command delivered successfully."
        fi
    else
        printf '%s\n' "$CONNECT_CMD" > "$CMD_FIFO"
        echo "Wrote '${CONNECT_CMD}' to $CMD_FIFO."
        echo "Note: If the game is not yet running, start it with BO2_CMDPIPE=$CMD_FIFO"
    fi
fi

echo ""
echo "Connection command dispatched to Black Ops II!"
