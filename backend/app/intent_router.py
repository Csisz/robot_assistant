from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

from .kids_safety import normalize_hungarian

IntentName = Literal[
    "color_mixing",
    "basic_math",
    "weather",
    "short_story",
    "distance_or_place_question",
    "animal_or_nature_question",
    "greeting",
    "barkoba_start",
    "barkoba_change",
    "profile_claim",
    "unknown_safe",
    "blocked_unsafe",
]


@dataclass(frozen=True)
class IntentResult:
    intent: IntentName
    normalized_message: str


COLOR_ROOTS = ("piros", "sarga", "kek", "zold", "fekete", "feher")


def _has_any(text: str, words: tuple[str, ...]) -> bool:
    return any(word in text for word in words)


def _count_colors(text: str) -> int:
    return sum(1 for color in COLOR_ROOTS if color in text)


def detect_intent(message: str, *, safe: bool = True) -> IntentResult:
    text = normalize_hungarian(message)
    if not safe:
        return IntentResult("blocked_unsafe", text)

    if _has_any(
        text,
        (
            "gondolj masra",
            "tudsz masra gondolni",
            "ne elefantra",
            "masikra gondolj",
            "kezdjuk ujra",
            "uj barkoba",
            "gondolj valami masra",
        ),
    ):
        return IntentResult("barkoba_change", text)

    if _has_any(text, ("barkoba", "gondolj valamire", "talalos jatek")):
        return IntentResult("barkoba_start", text)

    if _has_any(text, (" vagyok", "a nevem ", "hivj ")):
        return IntentResult("profile_claim", text)

    if _count_colors(text) >= 2 and _has_any(
        text,
        ("kever", "osszekever", "szint kapok", "milyen szint", "mit kapok"),
    ):
        return IntentResult("color_mixing", text)

    if _has_any(
        text,
        (
            "idojaras",
            "milyen ido",
            "ido lesz",
            "ido van",
            "esni fog",
            "eso lesz",
            "kell kabat",
            "kabatos",
            "elorejelzes",
        ),
    ):
        return IntentResult("weather", text)

    if _has_any(text, ("mese", "meset", "tortenet", "meselj")):
        return IntentResult("short_story", text)

    if _has_any(text, ("milyen messze", "tavol", "hany kilometer", "hany km", "merre van")):
        return IntentResult("distance_or_place_question", text)

    if _has_any(text, ("mennyi", "szamol", "plusz", "minusz", "szor", "meg")) and (
        any(ch.isdigit() for ch in text)
        or _has_any(text, ("egy", "ketto", "harom", "negy", "ot", "hat", "het", "nyolc", "kilenc", "tiz"))
    ):
        return IntentResult("basic_math", text)

    if any(op in text for op in ("+", "-", "*", " x ", "×")) and any(ch.isdigit() for ch in text):
        return IntentResult("basic_math", text)

    if _has_any(
        text,
        (
            "allat",
            "nyusz",
            "kutya",
            "cica",
            "macska",
            "madar",
            "hal",
            "fa",
            "virag",
            "termeszet",
            "erdo",
            "to ",
        ),
    ):
        return IntentResult("animal_or_nature_question", text)

    if _has_any(text, ("szia", "hello", "helo", "jo reggelt", "jo napot", "szervusz")):
        return IntentResult("greeting", text)

    return IntentResult("unknown_safe", text)
