#!/usr/bin/env python3
"""
zm_probe.py — Call of Duty / Black Ops II Zombies out-of-band UDP probe utility.

Features:
1. Sends out-of-band CoD ping / getchallenge datagrams (\\xff\\xff\\xff\\xffgetchallenge)
   to a target host:port over UDP.
2. Listens for \\xff\\xff\\xff\\xffchallengeResponse, \\xff\\xff\\xff\\xffinfoResponse, or other
   CoD out-of-band responses with configurable timeout and retries.
3. Parses challenge tokens and key-value infostrings (\\key\\val...).
4. Automated --self-test mode testing against both internal mock and dw_local.cpp
   real Darwin UDP sockets.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from typing import Any, Dict, Optional, Tuple

OOB_PREFIX = b"\xff\xff\xff\xff"


def build_query_packet(query_type: str, arg: Optional[str] = None) -> bytes:
    """Builds a Call of Duty out-of-band UDP query packet."""
    q = query_type.strip()
    if q.lower() == "getchallenge":
        payload = f"getchallenge {arg}" if arg else "getchallenge"
    elif q.lower() == "getinfo":
        payload = f"getinfo {arg}" if arg else "getinfo xxx"
    elif q.lower() == "getstatus":
        payload = "getstatus"
    elif q.lower() == "ping":
        payload = "ping"
    else:
        payload = f"{q} {arg}" if arg else q
    return OOB_PREFIX + payload.encode("latin1", errors="replace")


def parse_infostring(raw_str: str) -> Dict[str, str]:
    """Parses standard Quake/CoD infostring format: \\key1\\val1\\key2\\val2..."""
    fields: Dict[str, str] = {}
    tokens = raw_str.strip().split("\\")
    # First token might be empty or command header before the first backslash
    if tokens and tokens[0] and not tokens[0].startswith("\\"):
        tokens = tokens[1:]
    else:
        tokens = tokens[1:]

    for i in range(0, len(tokens) - 1, 2):
        key = tokens[i].strip()
        val = tokens[i + 1].strip()
        if key:
            fields[key] = val
    return fields


def parse_oob_response(data: bytes) -> Dict[str, Any]:
    """
    Parses a Call of Duty out-of-band UDP response packet.
    Expected prefix: \\xff\\xff\\xff\\xff.
    """
    if not data.startswith(OOB_PREFIX):
        return {
            "valid": False,
            "type": "UNKNOWN",
            "payload": "",
            "fields": {},
            "raw": data.hex(),
            "error": "Missing 4-byte 0xFF prefix",
        }

    body = data[len(OOB_PREFIX):]
    text = body.decode("latin1", errors="replace")

    # Command token is typically first word or line
    parts = text.split(None, 1)
    first_token = parts[0] if parts else ""
    rest = parts[1] if len(parts) > 1 else ""

    result: Dict[str, Any] = {
        "valid": True,
        "type": first_token,
        "payload": text.strip(),
        "fields": {},
        "raw": data.hex(),
    }

    # Extract challenge if challengeResponse
    if first_token.startswith("challengeResponse"):
        # e.g. "challengeResponse 12345678" or "challengeResponse\n12345678"
        challenge_val = rest.strip()
        result["challenge"] = challenge_val
        result["type"] = "challengeResponse"

    # Extract fields if infoResponse or statusResponse
    elif "infoResponse" in first_token or "statusResponse" in first_token:
        # e.g. "infoResponse\n\challenge\...\hostname\..."
        result["fields"] = parse_infostring(text)
        result["type"] = "infoResponse" if "infoResponse" in first_token else "statusResponse"

    return result


def probe(
    host: str,
    port: int,
    query: str = "getchallenge",
    query_arg: Optional[str] = None,
    timeout: float = 2.0,
    retries: int = 3,
    verbose: bool = False,
) -> Dict[str, Any]:
    """
    Sends out-of-band packet to target host:port over UDP and waits for response.
    Returns structured result dict.
    """
    packet = build_query_packet(query, query_arg)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)

    result: Dict[str, Any] = {
        "success": False,
        "host": host,
        "port": port,
        "query": query,
        "rtt_ms": None,
        "response_type": None,
        "payload": None,
        "fields": {},
        "error": None,
    }

    last_err: Optional[str] = None

    try:
        # Resolve target IP
        target_ip = socket.gethostbyname(host)
        addr = (target_ip, port)

        for attempt in range(1, retries + 1):
            if verbose:
                print(f"[PROBE] Attempt {attempt}/{retries} -> {target_ip}:{port} query={query!r} ({len(packet)} bytes)")

            t0 = time.perf_counter()
            try:
                sock.sendto(packet, addr)
                resp_data, from_addr = sock.recvfrom(4096)
                rtt = (time.perf_counter() - t0) * 1000.0

                if verbose:
                    print(f"[PROBE] Received {len(resp_data)} bytes from {from_addr[0]}:{from_addr[1]} in {rtt:.2f} ms")

                parsed = parse_oob_response(resp_data)
                result["success"] = parsed.get("valid", False)
                result["rtt_ms"] = round(rtt, 2)
                result["response_type"] = parsed.get("type")
                result["payload"] = parsed.get("payload")
                result["fields"] = parsed.get("fields", {})
                if "challenge" in parsed:
                    result["challenge"] = parsed["challenge"]
                result["from_addr"] = f"{from_addr[0]}:{from_addr[1]}"
                result["raw_hex"] = parsed.get("raw")
                return result

            except socket.timeout:
                last_err = f"Timed out after {timeout}s (attempt {attempt}/{retries})"
                if verbose:
                    print(f"[PROBE] {last_err}")
            except Exception as e:
                last_err = f"Socket error: {e}"
                if verbose:
                    print(f"[PROBE] {last_err}")
                break

    finally:
        sock.close()

    result["error"] = last_err or "Unknown failure"
    return result


def run_self_test(verbose: bool = True) -> bool:
    """
    Automated self-test:
    1. Tests Python mock server with getchallenge and getinfo.
    2. Builds & tests against native dw_local.cpp Darwin POSIX UDP sockets.
    """
    print("=" * 60)
    print(">>> ZM_PROBE SELF-TEST: CoD UDP Protocol & dw_local.cpp <<<")
    print("=" * 60)

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dw_local_path = os.path.join(repo_root, "runtime", "win32", "dw_local.cpp")
    harness_src = os.path.join(repo_root, "tools", "zm_dw_harness.cpp")
    harness_bin = os.path.join(repo_root, "tools", "zm_dw_harness")

    # Step 1: Internal Mock Test
    print("\n[STEP 1] Testing UDP probe against Python mock server...")
    mock_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    mock_sock.bind(("127.0.0.1", 0))
    mock_port = mock_sock.getsockname()[1]

    def mock_server():
        try:
            mock_sock.settimeout(2.0)
            data, addr = mock_sock.recvfrom(1024)
            if data.startswith(OOB_PREFIX) and b"getchallenge" in data:
                mock_sock.sendto(OOB_PREFIX + b"challengeResponse 424242", addr)
        except Exception:
            pass
        finally:
            mock_sock.close()

    import threading
    th = threading.Thread(target=mock_server, daemon=True)
    th.start()

    res = probe("127.0.0.1", mock_port, query="getchallenge", timeout=1.0, verbose=verbose)
    assert res["success"], f"Mock probe failed: {res}"
    assert res["response_type"] == "challengeResponse"
    assert res.get("challenge") == "424242"
    print(f"  Mock probe PASSED! RTT={res['rtt_ms']} ms, challenge={res.get('challenge')}")

    # Step 2: Native dw_local.cpp Test
    print("\n[STEP 2] Compiling native dw_local.cpp harness...")
    compile_cmd = [
        "clang++",
        "-arch", "arm64",
        "-std=c++17",
        "-DADDRESS_SIZE_BITS=32",
        "-DBO2_TARGET_ZMS",
        "-I", os.path.join(repo_root, "runtime", "win32"),
        "-I", os.path.join(repo_root, "tools", "remill-src", "include"),
        dw_local_path,
        harness_src,
        "-lz",
        "-framework", "CoreFoundation",
        "-framework", "Security",
        "-o", harness_bin,
    ]
    if verbose:
        print("  " + " ".join(compile_cmd))

    build_res = subprocess.run(compile_cmd, capture_output=True, text=True)
    if build_res.returncode != 0:
        print(f"Compilation failed:\n{build_res.stderr}")
        return False
    print("  Native harness compiled successfully.")

    # Step 3: Run native dw_local.cpp harness & probe it
    print("\n[STEP 3] Launching dw_local.cpp UDP socket harness...")
    proc = subprocess.Popen(
        [harness_bin],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        # Wait for "READY port=<port>"
        line = proc.stdout.readline()
        if not line.startswith("READY port="):
            print(f"Harness failed to start: {line}\nStderr: {proc.stderr.read()}")
            return False

        harness_port = int(line.strip().split("port=")[1])
        print(f"  Harness bound real Darwin UDP socket to port: {harness_port}")

        # Test 3a: getchallenge
        print(f"\n[STEP 3a] Probing getchallenge on 127.0.0.1:{harness_port}...")
        res_chal = probe("127.0.0.1", harness_port, query="getchallenge", timeout=2.0, verbose=verbose)
        if not res_chal["success"]:
            print(f"getchallenge probe failed: {res_chal['error']}")
            return False
        assert res_chal["response_type"] == "challengeResponse"
        assert res_chal.get("challenge") == "1337042"
        print(f"  [OK] getchallenge SUCCESS: challenge={res_chal.get('challenge')} RTT={res_chal['rtt_ms']} ms")

        # Test 3b: getinfo
        print(f"\n[STEP 3b] Probing getinfo on 127.0.0.1:{harness_port}...")
        res_info = probe("127.0.0.1", harness_port, query="getinfo", timeout=2.0, verbose=verbose)
        if not res_info["success"]:
            print(f"getinfo probe failed: {res_info['error']}")
            return False
        assert res_info["response_type"] == "infoResponse"
        fields = res_info.get("fields", {})
        assert fields.get("hostname") == "BO2-Darwin-Server"
        assert fields.get("mapname") == "zm_transit"
        assert fields.get("gametype") == "zclassic"
        print(f"  [OK] getinfo SUCCESS: hostname='{fields.get('hostname')}' map='{fields.get('mapname')}' clients={fields.get('clients')}/{fields.get('sv_maxclients')}")

        # Test 3c: ping
        print(f"\n[STEP 3c] Probing ping on 127.0.0.1:{harness_port}...")
        res_ping = probe("127.0.0.1", harness_port, query="ping", timeout=2.0, verbose=verbose)
        assert res_ping["success"] and res_ping["payload"] == "ack"
        print(f"  [OK] ping SUCCESS: ack in {res_ping['rtt_ms']} ms")

        # Terminate harness gracefully
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(b"QUIT", ("127.0.0.1", harness_port))
        sock.close()
        proc.wait(timeout=2.0)

    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait()
        if os.path.exists(harness_bin):
            os.remove(harness_bin)

    print("\n" + "=" * 60)
    print(">>> ALL SELF-TESTS PASSED! dw_local.cpp UDP SOCKETS VERIFIED! <<<")
    print("=" * 60)
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Call of Duty / Black Ops II Zombies out-of-band UDP probe utility",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("host", nargs="?", default="127.0.0.1", help="Target server hostname or IP")
    parser.add_argument("port", nargs="?", type=int, default=3074, help="Target UDP port (default: 3074)")
    parser.add_argument("-t", "--timeout", type=float, default=2.0, help="Socket receive timeout in seconds")
    parser.add_argument("-r", "--retries", type=int, default=3, help="Number of retry attempts")
    parser.add_argument(
        "-q",
        "--query",
        default="getchallenge",
        help="Query type: getchallenge, getinfo, getstatus, ping, or custom command",
    )
    parser.add_argument("--arg", default=None, help="Optional argument to accompany the query")
    parser.add_argument("-v", "--verbose", action="store_true", help="Enable verbose debug logging")
    parser.add_argument("--json", action="store_true", help="Output result as JSON")
    parser.add_argument("--self-test", action="store_true", help="Run automated self-tests and exit")

    args = parser.parse_args()

    if args.self_test:
        success = run_self_test(verbose=args.verbose)
        sys.exit(0 if success else 1)

    result = probe(
        host=args.host,
        port=args.port,
        query=args.query,
        query_arg=args.arg,
        timeout=args.timeout,
        retries=args.retries,
        verbose=args.verbose,
    )

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        if result["success"]:
            print(f"[{result['response_type']}] from {result['host']}:{result['port']} in {result['rtt_ms']} ms")
            if "challenge" in result:
                print(f"  Challenge: {result['challenge']}")
            if result.get("fields"):
                print("  Server Info:")
                for k, v in result["fields"].items():
                    print(f"    {k}: {v}")
            elif result.get("payload") and not result.get("challenge"):
                print(f"  Payload: {result['payload']}")
        else:
            print(f"[FAILED] Could not probe {result['host']}:{result['port']}: {result['error']}")
            sys.exit(1)


if __name__ == "__main__":
    main()
