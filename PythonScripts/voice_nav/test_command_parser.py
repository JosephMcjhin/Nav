"""
test_command_parser.py
Unit tests for command_parser.py.
Run: python test_command_parser.py
"""

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from command_parser import parse_navigation_command

test_cases = [
    # Chinese commands
    ("导航到入口",         {"action": "navigate_to", "target": "entrance"}),
    ("带我去电梯",         {"action": "navigate_to", "target": "elevator"}),
    ("前往出口",           {"action": "navigate_to", "target": "exit"}),
    ("去洗手间",           {"action": "navigate_to", "target": "restroom"}),
    ("停止",               {"action": "stop"}),
    ("停下来",             {"action": "stop"}),
    ("取消",               {"action": "stop"}),
    ("有哪些目的地",       {"action": "list_destinations"}),
    # English commands
    ("go to entrance",     {"action": "navigate_to", "target": "entrance"}),
    ("navigate to exit",   {"action": "navigate_to", "target": "exit"}),
    ("take me to elevator",{"action": "navigate_to", "target": "elevator"}),
    ("stop",               {"action": "stop"}),
    ("cancel",             {"action": "stop"}),
    ("list destinations",  {"action": "list_destinations"}),
    # Negative cases
    ("今天天气怎么样",     None),
    ("hello world",        None),
]

print("=== Command Parser Tests ===\n")
passed = 0
failed = 0
for text, expected in test_cases:
    result = parse_navigation_command(text)
    ok = result == expected
    if ok:
        passed += 1
        print(f"  ✓ '{text}'")
    else:
        failed += 1
        print(f"  ✗ '{text}'")
        print(f"    Expected : {expected}")
        print(f"    Got      : {result}")

print(f"\n{passed}/{len(test_cases)} passed, {failed} failed.")
sys.exit(0 if failed == 0 else 1)
