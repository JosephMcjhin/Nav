import re
import difflib
from typing import Optional

DESTINATION_MAP = {
    # --- Chinese keywords -> UE Tag ---
    "工位": "WorkStation",
    "会议桌": "ConferenceTable",
    "沙发": "Sofa",
    "空调": "AirConditioner",
    "门": "Door",
}

# Combined navigation triggers (merging previous nav and move triggers)
ACTION_TRIGGERS_ZH = ["导航到", "带我去", "带我到", "指路", "怎么去", "去", "前往", "跑到", "走到", "到达", "前去", "移动到"]
ACTION_TRIGGERS_EN = ["navigate to", "show me the way to", "guide me to", "go to", "take me to", "move to", "walk to", "find"]


def normalize(text: str) -> str:
    """Lowercase and strip punctuation."""
    text = text.lower().strip()
    text = re.sub(r"[，。？！、,\.?!\s]+", " ", text)
    return text.strip()


def get_best_partial_match(query: str, text: str) -> float:
    """
    Returns a score between 0.0 and 1.0 representing the best partial match
    of 'query' within 'text' based on SequenceMatcher.
    """
    if not query or not text:
        return 0.0
    if query in text:
        return 1.0
        
    query_len = len(query)
    max_ratio = 0.0
    # Check windows of length similar to the query to handle omissions/additions
    lengths_to_check = {max(1, query_len - 1), query_len, query_len + 1}
    
    for length in lengths_to_check:
        if length > len(text):
            ratio = difflib.SequenceMatcher(None, query, text).ratio()
            if ratio > max_ratio:
                max_ratio = ratio
            continue
            
        for i in range(len(text) - length + 1):
            window = text[i:i+length]
            ratio = difflib.SequenceMatcher(None, query, window).ratio()
            if ratio > max_ratio:
                max_ratio = ratio
                
    return max_ratio


def find_destination(text: str, threshold: float = 0.5) -> Optional[str]:
    """Find a destination tag in the text using similarity matching. Returns UE tag name or None."""
    normalized = normalize(text)
    best_match_tag = None
    highest_score = 0.0
    
    for keyword, tag in DESTINATION_MAP.items():
        score = get_best_partial_match(keyword.lower(), normalized)
        if score > highest_score and score >= threshold:
            highest_score = score
            best_match_tag = tag
            
    return best_match_tag


def parse_navigation_command(transcript: str) -> Optional[dict]:
    """Parse the voice command and return a navigation action based on similarity matching."""
    text = transcript.strip()
    if not text:
        return None

    destination = find_destination(text, threshold=0.5)

    if destination:
        return {"action": "navigate_to", "target": destination}

    return None

