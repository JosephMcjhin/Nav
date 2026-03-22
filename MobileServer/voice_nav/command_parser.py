"""
command_parser.py
Parses Whisper-transcribed speech into structured navigation commands.
Supports both Chinese (中文) and English voice commands.

Usage:
    from command_parser import parse_navigation_command
    result = parse_navigation_command("导航到入口")
    # Returns: {"action": "navigate_to", "target": "entrance"} or None
"""

import re
from typing import Optional

# ============================================================
# Destination keyword mapping
# Maps spoken words (Chinese & English) -> UE5 Actor Tag name
# ADD YOUR OWN TAGS HERE – must match the tags you set in UE Editor
# ============================================================
DESTINATION_MAP = {
    # --- Chinese keywords -> UE Tag ---
    "工位": "WorkStation",
    "会议桌": "ConferenceTable",
    "沙发": "Sofa",
    "空调": "AirConditioner",
    "门": "Door",
    # --- English keywords -> UE Tag ---
    "work station": "WorkStation",
    "conference table": "ConferenceTable",
    "sofa": "Sofa",
    "air conditioner": "AirConditioner",
    "door": "Door",
}

# Navigation trigger phrases (display path only)
NAV_TRIGGERS_ZH = ["导航到", "带我去", "带我到", "指路", "怎么去"]
NAV_TRIGGERS_EN = ["navigate to", "show me the way to", "guide me to"]

# Move trigger phrases (auto movement)
MOVE_TRIGGERS_ZH = ["去", "前往", "跑到", "走到", "到达", "前去", "移动到"]
MOVE_TRIGGERS_EN = ["go to", "take me to", "move to", "walk to", "find"]

# Stop phrases
STOP_PHRASES = ["停止", "停下", "取消", "stop", "cancel", "halt", "abort"]

# List destinations phrases
LIST_PHRASES = ["有哪些", "列出", "目的地", "destinations", "list", "where can"]


def normalize(text: str) -> str:
    """Lowercase and strip punctuation."""
    text = text.lower().strip()
    text = re.sub(r"[，。？！、,\.?!\s]+", " ", text)
    return text.strip()


def find_destination(text: str) -> Optional[str]:
    """Find a destination tag in the text. Returns UE tag name or None."""
    normalized = normalize(text)
    # Longer keywords first (greedy match)
    for keyword, tag in sorted(DESTINATION_MAP.items(), key=lambda x: -len(x[0])):
        if keyword.lower() in normalized:
            return tag
    return None


def parse_navigation_command(transcript: str) -> Optional[dict]:
    """
    Parse a speech transcript into a navigation command dict.

    Returns one of:
        {"action": "navigate_to", "target": "<ue_tag>"}
        {"action": "move_to", "target": "<ue_tag>"}
        {"action": "stop"}
        {"action": "list_destinations"}
        None  (if not understood)
    """
    text = transcript.strip()
    if not text:
        return None

    normalized = normalize(text)

    # --- Stop command ---
    for phrase in STOP_PHRASES:
        if phrase in normalized:
            return {"action": "stop"}

    # --- List destinations command ---
    for phrase in LIST_PHRASES:
        if phrase in normalized:
            return {"action": "list_destinations"}

    # --- Navigate / Move command ---
    destination = find_destination(text)  # Use original text for CJK char matching

    if destination:
        # Check if it's a movement command
        has_move = any(t in normalized for t in MOVE_TRIGGERS_ZH + MOVE_TRIGGERS_EN)
        has_nav = any(t in normalized for t in NAV_TRIGGERS_ZH + NAV_TRIGGERS_EN)
        
        if has_move:
            return {"action": "move_to", "target": destination}
        else:
            # Default to nav if no explicit move trigger
            return {"action": "navigate_to", "target": destination}

    return None


# ============================================================
# Self-test
# ============================================================
if __name__ == "__main__":
    test_cases = [
        ("导航到入口", {"action": "navigate_to", "target": "entrance"}),
        ("移动到电梯", {"action": "move_to", "target": "elevator"}),
        ("go to exit",  {"action": "move_to", "target": "exit"}),
        ("navigate to goal", {"action": "navigate_to", "target": "Goal"}),
        ("停止",        {"action": "stop"}),
        ("stop",        {"action": "stop"}),
        ("有哪些目的地", {"action": "list_destinations"}),
        ("list destinations", {"action": "list_destinations"}),
        ("今天天气怎么样",  None),
    ]

    print("=== Command Parser Tests ===")
    all_pass = True
    for text, expected in test_cases:
        result = parse_navigation_command(text)
        ok = result == expected
        all_pass = all_pass and ok
        status = "✓" if ok else "✗"
        print(f"  {status} '{text}' → {result}  (expected: {expected})")

    print(f"\n{'All tests passed!' if all_pass else 'Some tests FAILED.'}")
