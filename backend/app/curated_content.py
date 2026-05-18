from __future__ import annotations

import json
import random
import unicodedata
from collections import defaultdict
from pathlib import Path

_DATA_DIR = Path(__file__).resolve().parent / "data"
_RECENT_LIMIT = 5

_JOKE_FALLBACK: list[dict] = [
    {
        "id": "fallback_joke",
        "text": "Miért vitt a csiga létrát az iskolába? Mert magasabb osztályba akart járni.",
        "tts_text": "Miért vitt a csiga létrát az iskolába? Mert magasabb osztályba akart járni.",
    }
]
_RIDDLE_FALLBACK: list[dict] = [
    {
        "id": "fallback_riddle",
        "question": "Éjjel-nappal mindig jár, mégis egyhelyben áll. Mi az?",
        "answer": "óra",
        "accepted_answers": ["óra"],
        "tts_text": "Éjjel-nappal mindig jár, mégis egyhelyben áll. Mi az?",
        "answer_tts_text": "A megfejtés: óra.",
    }
]

# Per-conversation recent history to avoid repetition
_RECENT_JOKES: dict[str, list[str]] = defaultdict(list)
_RECENT_RIDDLES: dict[str, list[str]] = defaultdict(list)

_JOKE_KEYWORDS = ("vicc",)
_RIDDLE_KEYWORDS = ("talalo", "rejtveny", "fejtoro")


def _load_json(path: Path, fallback: list[dict]) -> list[dict]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(data, list) and data:
            return data
    except Exception:
        pass
    return fallback


def _jokes() -> list[dict]:
    return _load_json(_DATA_DIR / "hungarian_jokes.json", _JOKE_FALLBACK)


def _riddles() -> list[dict]:
    items = _load_json(_DATA_DIR / "hungarian_riddles.json", _RIDDLE_FALLBACK)
    # skip riddles with no valid accepted_answers (e.g. incomplete entries)
    return [r for r in items if r.get("accepted_answers")] or _RIDDLE_FALLBACK


def normalize_hu_answer(text: str) -> str:
    """Casefold + strip combining diacritics for accent-insensitive comparison."""
    stripped = text.strip().casefold()
    folded = unicodedata.normalize("NFKD", stripped)
    return "".join(ch for ch in folded if not unicodedata.combining(ch))


def is_correct_riddle_answer(user_answer: str, accepted_answers: list[str]) -> bool:
    """Return True if user_answer matches any accepted answer accent-insensitively."""
    norm = normalize_hu_answer(user_answer)
    return any(normalize_hu_answer(a) == norm for a in accepted_answers)


def is_joke_request(message: str) -> bool:
    norm = normalize_hu_answer(message)
    return any(kw in norm for kw in _JOKE_KEYWORDS)


def is_riddle_request(message: str) -> bool:
    norm = normalize_hu_answer(message)
    return any(kw in norm for kw in _RIDDLE_KEYWORDS)


def get_random_joke(conversation_id: str | None = None) -> dict:
    """Return a random joke, avoiding the last _RECENT_LIMIT per conversation."""
    cid = conversation_id or "__global__"
    jokes = _jokes()
    recent = _RECENT_JOKES[cid]
    candidates = [j for j in jokes if j.get("id") not in recent] or jokes
    choice = random.choice(candidates)
    recent.append(choice.get("id", ""))
    if len(recent) > _RECENT_LIMIT:
        recent.pop(0)
    return choice


def get_random_riddle(conversation_id: str | None = None) -> dict:
    """Return a random riddle with valid answers, avoiding the last _RECENT_LIMIT per conversation."""
    cid = conversation_id or "__global__"
    riddles = _riddles()
    recent = _RECENT_RIDDLES[cid]
    candidates = [r for r in riddles if r.get("id") not in recent] or riddles
    choice = random.choice(candidates)
    recent.append(choice.get("id", ""))
    if len(recent) > _RECENT_LIMIT:
        recent.pop(0)
    return choice
