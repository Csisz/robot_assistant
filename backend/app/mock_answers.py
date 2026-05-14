from __future__ import annotations

import re

from .intent_router import IntentName
from .kids_safety import normalize_hungarian
from .profiles import ChildProfile, get_profile

UNKNOWN_SAFE_REPLY = (
    "Ezt most még nem tudom biztosan megválaszolni, de megkérdezhetsz "
    "színekről, állatokról, számokról, időjárásról, vagy kérhetsz tőlem egy rövid mesét."
)

COLOR_REPLIES: dict[frozenset[str], str] = {
    frozenset(("piros", "sarga")): "Ha a pirosat és a sárgát összekevered, narancssárgát kapsz.",
    frozenset(("kek", "sarga")): "Ha a kéket és a sárgát összekevered, zöldet kapsz.",
    frozenset(("piros", "kek")): "Ha a pirosat és a kéket összekevered, lilát kapsz.",
    frozenset(("zold", "sarga")): "Ha a zöldet és a sárgát összekevered, sárgászöld vagy világoszöld színt kapsz.",
    frozenset(("fekete", "feher")): "Ha a feketét és a fehéret összekevered, szürke színt kapsz.",
    frozenset(("piros", "feher")): "Ha a pirosat és a fehéret összekevered, rózsaszín színt kapsz.",
    frozenset(("kek", "feher")): "Ha a kéket és a fehéret összekevered, világoskék színt kapsz.",
}

NUMBER_WORDS = {
    "nulla": 0,
    "egy": 1,
    "ketto": 2,
    "ket": 2,
    "harom": 3,
    "negy": 4,
    "ot": 5,
    "hat": 6,
    "het": 7,
    "nyolc": 8,
    "kilenc": 9,
    "tiz": 10,
}


def _detected_colors(text: str) -> set[str]:
    return {color for color in ("piros", "sarga", "kek", "zold", "fekete", "feher") if color in text}


def _color_mixing_reply(text: str) -> str:
    colors = _detected_colors(text)
    for pair, reply in COLOR_REPLIES.items():
        if pair.issubset(colors):
            return reply
    return "Ezt a színkeverést még gyakorlom, de a piros, sárga és kék nagyon jó kezdő színek."


def _format_math_reply(left: int, op_word: str, right: int, result: int) -> str:
    return f"A {left} {op_word} {right} az {result}."


def _parse_basic_math(text: str) -> tuple[int, str, int, int] | None:
    digit_match = re.search(r"\b(\d{1,2})\s*(\+|-|\*|x|×)\s*(\d{1,2})\b", text)
    if digit_match:
        left = int(digit_match.group(1))
        op = digit_match.group(2)
        right = int(digit_match.group(3))
        return _calculate(left, op, right)

    digit_word_match = re.search(
        r"\b(\d{1,2})\s+(meg|plusz|minusz|kivonva|szor)\s+(\d{1,2})\b",
        text,
    )
    if digit_word_match:
        left = int(digit_word_match.group(1))
        op = digit_word_match.group(2)
        right = int(digit_word_match.group(3))
        return _calculate(left, op, right)

    words = "|".join(sorted(NUMBER_WORDS, key=len, reverse=True))
    word_match = re.search(rf"\b({words})\s+(meg|plusz|minusz|kivonva|szor)\s+({words})\b", text)
    if word_match:
        left = NUMBER_WORDS[word_match.group(1)]
        op = word_match.group(2)
        right = NUMBER_WORDS[word_match.group(3)]
        return _calculate(left, op, right)

    return None


def _calculate(left: int, op: str, right: int) -> tuple[int, str, int, int] | None:
    if left > 20 or right > 20:
        return None
    if op in ("+", "meg", "plusz"):
        return left, "meg", right, left + right
    if op in ("-", "minusz", "kivonva"):
        return left, "mínusz", right, left - right
    if op in ("*", "x", "×", "szor"):
        return left, "szor", right, left * right
    return None


def _math_reply(text: str) -> str:
    parsed = _parse_basic_math(text)
    if not parsed:
        return "Ezt a számolást most nem tudom biztosan, de kis számokkal szívesen gyakorlok veled."
    left, op_word, right, result = parsed
    return _format_math_reply(left, op_word, right, result)


def _story_reply(text: str, profile: ChildProfile) -> str:
    prefix = f"{profile.display_name}, " if profile.profile_id != "guest" else ""
    if "unikornis" in text or "unikornis" in profile.safe_interests:
        return (
            f"{prefix}egyszer egy kedves unikornis talált egy csillogó kavicsot a réten. "
            "Nem tartotta meg egyedül, hanem megmutatta a barátainak. "
            "Együtt kitalálták, hogy a kavics legyen a kedvesség jele. "
            "Aki aznap segített valakinek, az a kavics mellé tehetett egy virágszirmot. "
            "Estére sok színes szirom gyűlt össze, és mindenki mosolygott. "
            "Az unikornis boldogan aludt el, mert tudta, hogy a kedvesség tovább tud vándorolni."
        )
    if "nyusz" in text:
        return (
            f"{prefix}volt egyszer egy pici nyuszi, aki egy puha réten lakott. "
            "Reggel talált egy harmatos lóherelevelet, és nagyon megörült neki. "
            "Elvitte a barátainak, hogy együtt nézzék meg. "
            "A sün, a rigó és a kis robot mind azt mondták, hogy ez igazi kincs. "
            "A nyuszi ekkor rájött, hogy a legjobb dolgok még szebbek, ha megosztjuk őket. "
            "Este boldogan bújt be a kuckójába, és egy napfényes rétről álmodott."
        )
    return (
        f"{prefix}egyszer egy kis robot talált egy sárga falevelet a kertben. "
        "Óvatosan felemelte, és megmutatta a barátainak. "
        "A falevél olyan volt, mint egy apró napocska. "
        "Együtt rajzoltak mellé felhőt, virágot és mosolygós házikót. "
        "Amikor elkészültek, kitették a rajzot az ablakba. "
        "A kis robot örült, mert egy apró levélből kedves közös játék lett."
    )


def _distance_reply(text: str) -> str:
    if "balaton" in text:
        return (
            "A Balaton Budapesttől nagyjából 100–120 kilométerre van, attól függően, "
            "hogy a tó melyik részére mentek. Autóval általában körülbelül 1–2 óra az út."
        )
    return (
        "Attól függ, honnan indultok. Ezt a távolságot most nem tudom biztosan kiszámolni, "
        "de egy felnőttel megnézhetitek a térképen."
    )


def _animal_or_nature_reply(text: str) -> str:
    if "nyusz" in text:
        return "A nyuszik hosszú fülükkel jól hallanak, és erős hátsó lábukkal nagyokat tudnak ugrani."
    if "kutya" in text:
        return "A kutyák sokszor nagyon figyelmes társak, és szagok alapján rengeteg mindent felismernek."
    if "macska" in text or "cica" in text:
        return "A cicák puha léptekkel járnak, és a bajszuk segít nekik érezni, mennyi hely van körülöttük."
    if "madar" in text:
        return "A madarak tollai segítik a repülést és melegen is tartják őket."
    return "A természet tele van apró csodákkal: levelekkel, felhőkkel, madarakkal és kíváncsi kis állatokkal."


def _greeting_reply(profile: ChildProfile) -> str:
    name = profile.display_name
    return (
        f"Szia, {name}! Örülök, hogy beszélgetünk. "
        "Kérdezhetsz színekről, állatokról, számokról, időjárásról, vagy kérhetsz egy rövid mesét."
    )


def get_mock_answer(intent: IntentName, message: str, profile_id: str | None = None) -> tuple[str, str]:
    text = normalize_hungarian(message)
    profile = get_profile(profile_id)
    if intent == "color_mixing":
        return _color_mixing_reply(text), "mock"
    if intent == "basic_math":
        return _math_reply(text), "mock"
    if intent == "short_story":
        return _story_reply(text, profile), "mock"
    if intent == "distance_or_place_question":
        return _distance_reply(text), "mock"
    if intent == "animal_or_nature_question":
        return _animal_or_nature_reply(text), "mock"
    if intent == "greeting":
        return _greeting_reply(profile), "mock"
    return UNKNOWN_SAFE_REPLY, "fallback"
