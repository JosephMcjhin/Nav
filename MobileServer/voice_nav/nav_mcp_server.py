"""
nav_mcp_server.py
MCP server that exposes navigation tools to MCP-compatible AI agents
(e.g. Claude Desktop, Cursor, etc.).

This makes it possible for an AI assistant to call:
    navigate_to("entrance")   → sends TCP command to UE5
    stop_navigation()
    list_destinations()

Start with:
    python nav_mcp_server.py

Then configure your MCP client (e.g. claude_desktop_config.json).
"""

from fastmcp import FastMCP
from nav_tcp_client import navigate_to as tcp_navigate, stop_navigation as tcp_stop, list_destinations as tcp_list

# Create MCP server
mcp = FastMCP(
    name="UnrealNavMCP",
    instructions=(
        "This MCP server controls a blind navigation system inside Unreal Engine 5. "
        "Use navigate_to() to move the character to a named destination. "
        "Available destinations are configured in the Unreal scene as tagged Actors."
    ),
)


@mcp.tool()
def navigate_to(destination: str) -> str:
    """
    Navigate the player character to the named destination in the Unreal Engine scene.

    Args:
        destination: The name/tag of the destination actor in the scene.
                     Common values: 'entrance', 'exit', 'elevator', 'stairs', 'Goal'

    Returns:
        Status message.
    """
    ok = tcp_navigate(destination)
    if ok:
        return f"Navigation started to '{destination}'. The character is now moving to the destination."
    else:
        return f"Failed to send navigate_to command. Make sure Unreal Engine is running in Play mode."


@mcp.tool()
def stop_navigation() -> str:
    """
    Stop the current navigation. The character will halt immediately.

    Returns:
        Status message.
    """
    ok = tcp_stop()
    if ok:
        return "Navigation stopped."
    else:
        return "Failed to send stop command. Is Unreal Engine running in Play mode?"


@mcp.tool()
def list_destinations() -> str:
    """
    Ask Unreal Engine to display available navigation destinations on screen.
    Check the UE Output Log for the list.

    Returns:
        Status message.
    """
    ok = tcp_list()
    if ok:
        return "Destination list requested. Check the Unreal Engine screen or Output Log (filter: LogTemp)."
    else:
        return "Failed to connect to Unreal Engine. Is it running in Play mode?"


if __name__ == "__main__":
    print("Starting UnrealNavMCP server...")
    mcp.run()
