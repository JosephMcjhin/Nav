"""
run_voice_nav.py
Main launcher for the voice navigation system.
Starts both the MCP server (in background) and the microphone listener.

Usage:
    python run_voice_nav.py

Or just the speech listener (no MCP server):
    python speech_to_nav.py
"""

import threading
import subprocess
import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def start_mcp_server():
    """Launch the MCP server as a subprocess."""
    proc = subprocess.Popen(
        [sys.executable, os.path.join(SCRIPT_DIR, "nav_mcp_server.py")],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    print(f"[Launcher] MCP server started (PID {proc.pid})")
    return proc


if __name__ == "__main__":
    print("=" * 55)
    print("  Blind Navigation Voice Control System")
    print("=" * 55)

    # Start MCP server in background
    mcp_proc = start_mcp_server()

    # Start speech listener in foreground (blocks until Ctrl+C)
    import sys
    sys.path.insert(0, SCRIPT_DIR)
    from speech_to_nav import load_model, start_listening

    model = load_model()
    try:
        start_listening(model)
    finally:
        print("[Launcher] Stopping MCP server...")
        mcp_proc.terminate()
        mcp_proc.wait()
        print("[Launcher] Done.")
