from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

from .kids_safety import normalize_hungarian

ActivityName = Literal[
    "robot_asks_quiz",
    "calm_sleep_mode",
    "daily_routine",
    "drawing_idea",
    "creative_task",
    "joke_or_riddle",
    "animal_fact",
    "teach_me_session_memory",
    "parent_mode_basic",
]


@dataclass(frozen=True)
class ActivityIntent:
    activity: ActivityName | None
    normalized_message: str


def detect_activity(message: str) -> ActivityIntent:
    text = normalize_hungarian(message)

    if _has_any(text, ("szuloi mod", "parent mode")):
        return ActivityIntent("parent_mode_basic", text)

    if _has_any(text, ("jegyezd meg", "tanulj meg", "a kedvenc szinem", "szeretem a", "ezt szeretem")):
        return ActivityIntent("teach_me_session_memory", text)

    if _has_any(
        text,
        (
            "allatos tenyt kerek",
            "mondj allatos tenyt",
            "mondj egy allatos tenyt",
            "allatokrol mondj valamit",
            "allatos erdekesseg",
            "mondj meg egy allatos tenyt",
        ),
    ):
        return ActivityIntent("animal_fact", text)

    if _has_any(
        text,
        (
            "mondj talalos kerdest",
            "mondj egy talalos kerdest",
            "talalos kerdest kerek",
            "talalost kerek",
            "talalosat kerek",
            "mondj talalost",
            "mondj egy talalost",
            "talalos",
        ),
    ):
        return ActivityIntent("joke_or_riddle", text)

    if _has_any(
        text,
        (
            "te kerdezz",
            "kerdezz tolem valamit",
            "jatsszunk kerdezz-feleleket",
            "en valaszolok",
        ),
    ):
        return ActivityIntent("robot_asks_quiz", text)

    if _has_any(
        text,
        ("altato mod", "nyugtato mese", "segits megnyugodni", "almos vagyok", "mondj altatot", "legzos jatek"),
    ):
        return ActivityIntent("calm_sleep_mode", text)

    if _has_any(text, ("jo reggelt", "jo ejszakat", "mit csinaljunk ma", "esti rutin", "reggeli rutin")):
        return ActivityIntent("daily_routine", text)

    if _has_any(text, ("mit rajzoljak", "adj rajzotletet", "rajzolni szeretnek")):
        return ActivityIntent("drawing_idea", text)

    if _has_any(
        text,
        ("talaljunk ki valamit", "talaljunk ki egy nevet", "varazsige", "epitsunk kepzeletben", "kreativ feladatot kerek", "adj kreativ feladatot"),
    ):
        return ActivityIntent("creative_task", text)

    if _has_any(text, ("mondj egy viccet", "mondj egy gyerekviccet", "gyerekviccet")):
        return ActivityIntent("joke_or_riddle", text)

    return ActivityIntent(None, text)


def _has_any(text: str, needles: tuple[str, ...]) -> bool:
    return any(needle in text for needle in needles)
