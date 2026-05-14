from __future__ import annotations

import re
from typing import Any

from .kids_safety import normalize_hungarian

ALLOWED_KEYS = ("favorite_color", "favorite_animal", "favorite_story_topic", "preferred_game_mode")

COLORS = {
    "lila": "lila",
    "piros": "piros",
    "kek": "kek",
    "zold": "zold",
    "sarga": "sarga",
    "rozsaszin": "rozsaszin",
    "narancssarga": "narancssarga",
}

ANIMALS = {
    "nyuszi": "nyuszi",
    "kutya": "kutya",
    "macska": "macska",
    "cica": "macska",
    "lo": "lo",
    "elefant": "elefant",
    "delfin": "delfin",
    "pingvin": "pingvin",
}

STORY_TOPICS = {
    "unikornis": "unikornis",
    "tunder": "tunder",
    "allat": "allatok",
    "nyuszi": "nyuszi",
    "urhajo": "urhajo",
}

GAME_MODES = {
    "barkoba": "barkoba",
    "talalos": "találós",
    "rajz": "rajz",
    "mese": "mese",
}

SENSITIVE_WORDS = (
    "cim",
    "cimem",
    "lakcim",
    "iskola",
    "telefonszam",
    "teljes nev",
    "jelszo",
    "titok",
    "betegseg",
    "hol lakom",
)


def session_memory_snapshot(state: Any) -> dict[str, str]:
    memory = getattr(state, "session_memory", {})
    if not isinstance(memory, dict):
        return {}
    return {str(key): str(value) for key, value in memory.items() if key in ALLOWED_KEYS and str(value).strip()}


def handle_session_memory(message: str, state: Any) -> tuple[bool, str, dict[str, str]]:
    text = normalize_hungarian(message)
    if any(word in text for word in SENSITIVE_WORDS):
        return (
            False,
            "Erről nem kell mesélned nekem. Beszélgessünk inkább színekről, mesékről vagy állatokról.",
            session_memory_snapshot(state),
        )

    key, value = _extract_allowed_memory(text)
    if not key or not value:
        return (
            True,
            "Ezt most nem jegyzem meg, de elmondhatod például a kedvenc színedet vagy kedvenc állatodat.",
            session_memory_snapshot(state),
        )

    if not hasattr(state, "session_memory") or not isinstance(state.session_memory, dict):
        state.session_memory = {}
    state.session_memory[key] = value
    return True, _memory_reply(key, value), session_memory_snapshot(state)


def _extract_allowed_memory(text: str) -> tuple[str | None, str | None]:
    if "kedvenc szinem" in text:
        return "favorite_color", _find_value(text, COLORS)
    if "kedvenc allatom" in text:
        return "favorite_animal", _find_value(text, ANIMALS)
    if "kedvenc mesem" in text or "meseben szeretem" in text:
        return "favorite_story_topic", _find_value(text, STORY_TOPICS)
    if "szeretek" in text or "szeretem a" in text or "ezt szeretem" in text:
        for key, values in (
            ("favorite_color", COLORS),
            ("favorite_animal", ANIMALS),
            ("favorite_story_topic", STORY_TOPICS),
            ("preferred_game_mode", GAME_MODES),
        ):
            value = _find_value(text, values)
            if value:
                return key, value
    return None, None


def _find_value(text: str, values: dict[str, str]) -> str | None:
    for needle, value in values.items():
        if re.search(rf"\b{re.escape(needle)}\b", text):
            return value
    return None


def _memory_reply(key: str, value: str) -> str:
    labels = {
        "favorite_color": "szereted ezt a színt",
        "favorite_animal": "szereted ezt az állatot",
        "favorite_story_topic": "szereted ezt a mesetémát",
        "preferred_game_mode": "szereted ezt a játékot",
    }
    if key == "favorite_color":
        return f"Megjegyzem erre a beszélgetésre, hogy szereted a {value} színt."
    return f"Megjegyzem erre a beszélgetésre, hogy {labels.get(key, 'ezt szereted')}: {value}."
