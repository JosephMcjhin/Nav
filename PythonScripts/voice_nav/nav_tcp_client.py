"""
nav_tcp_client.py
Utility to send JSON navigation commands to the UE5 TCP server (port 9001).
Used internally by the MCP server and speech pipeline.
"""

import socket
import json
import time

UE_HOST = "127.0.0.1"
UE_PORT = 9001
TIMEOUT_SEC = 5.0


def send_command(command: dict, host: str = UE_HOST, port: int = UE_PORT) -> bool:
    """
    Send a JSON command to the UE5 TcpCommandServer.
    Returns True on success, False on failure.

    Command examples:
        {"action": "navigate_to", "target": "entrance"}
        {"action": "stop"}
        {"action": "list_destinations"}
    """
    try:
        with socket.create_connection((host, port), timeout=TIMEOUT_SEC) as sock:
            payload = json.dumps(command, ensure_ascii=False) + "\n"
            sock.sendall(payload.encode("utf-8"))
            time.sleep(0.1)  # Brief wait to let UE process
        return True
    except ConnectionRefusedError:
        print(f"[NavTCP] Connection refused – is UE5 running with Play active? ({host}:{port})")
        return False
    except TimeoutError:
        print(f"[NavTCP] Connection timed out ({host}:{port})")
        return False
    except Exception as e:
        print(f"[NavTCP] Error: {e}")
        return False


def navigate_to(target_tag: str) -> bool:
    """Navigate the character to the given destination tag."""
    return send_command({"action": "navigate_to", "target": target_tag})


def stop_navigation() -> bool:
    """Stop the current navigation."""
    return send_command({"action": "stop"})


def list_destinations() -> bool:
    """Ask UE to log available destinations to screen."""
    return send_command({"action": "list_destinations"})


if __name__ == "__main__":
    # Quick manual test
    import sys
    if len(sys.argv) < 2:
        print("Usage: python nav_tcp_client.py <tag>")
        print("  e.g. python nav_tcp_client.py Goal")
        sys.exit(1)
    tag = sys.argv[1]
    ok = navigate_to(tag)
    print(f"navigate_to('{tag}') -> {'OK' if ok else 'FAILED'}")
