from __future__ import annotations

import logging
import os
import random
from dataclasses import dataclass

from .kids_safety import normalize_hungarian

logger = logging.getLogger("robot_backend.barkoba")

MAX_BARKOBA_TURNS = 20
_RANDOM = random.SystemRandom()


@dataclass(frozen=True)
class BarkobaSecret:
    key: str
    name: str
    accusative: str
    category: str
    category_prompt: str
    aliases: tuple[str, ...]
    facts: dict[str, bool]


@dataclass
class BarkobaGame:
    secret_key: str
    turns: int = 0
    secret_name: str | None = None
    category: str | None = None
    engine: str = "deterministic"
    last_reason: str | None = None
    fact_sheet: dict[str, object] | None = None


def _secret(
    key: str,
    name: str,
    accusative: str,
    category: str,
    aliases: tuple[str, ...],
    *,
    animal: bool = False,
    fruit: bool = False,
    toy: bool = False,
    nature: bool = False,
    color: bool = False,
    vehicle: bool = False,
    bigger_than_dog: bool = False,
    small: bool = False,
    flies: bool = False,
    lives_in_water: bool = False,
) -> BarkobaSecret:
    prompts = {
        "animals": "\u00e1llatra",
        "fruits": "gy\u00fcm\u00f6lcsre",
        "toys": "j\u00e1t\u00e9kra",
        "nature": "term\u00e9szeti dologra",
        "colors": "sz\u00ednre",
        "vehicles": "j\u00e1rm\u0171re",
    }
    return BarkobaSecret(
        key=key,
        name=name,
        accusative=accusative,
        category=category,
        category_prompt=prompts[category],
        aliases=aliases,
        facts={
            "animal": animal,
            "fruit": fruit,
            "toy": toy,
            "nature": nature,
            "color": color,
            "vehicle": vehicle,
            "bigger_than_dog": bigger_than_dog,
            "small": small,
            "flies": flies,
            "lives_in_water": lives_in_water,
        },
    )


SECRETS: tuple[BarkobaSecret, ...] = (
    _secret("elefant", "elef\u00e1nt", "elef\u00e1ntra", "animals", ("elefant", "elefantra"), animal=True, bigger_than_dog=True),
    _secret("macska", "macska", "macsk\u00e1ra", "animals", ("macska", "cica", "macskara", "cicara"), animal=True, small=True),
    _secret("kutya", "kutya", "kuty\u00e1ra", "animals", ("kutya", "kutyara"), animal=True),
    _secret("nyuszi", "nyuszi", "nyuszira", "animals", ("nyuszi", "nyuszira", "nyul", "nyulra"), animal=True, small=True),
    _secret("teknos", "tekn\u0151s", "tekn\u0151sre", "animals", ("teknos", "teknosre"), animal=True, small=True),
    _secret("pingvin", "pingvin", "pingvinre", "animals", ("pingvin", "pingvinre"), animal=True),
    _secret("delfin", "delfin", "delfinre", "animals", ("delfin", "delfinre"), animal=True, lives_in_water=True),
    _secret("lo", "l\u00f3", "l\u00f3ra", "animals", ("lo", "lora"), animal=True, bigger_than_dog=True),
    _secret("pillango", "pillang\u00f3", "pillang\u00f3ra", "animals", ("pillango", "pillangora"), animal=True, small=True, flies=True),
    _secret("vidra", "vidra", "vidr\u00e1ra", "animals", ("vidra", "vidrara"), animal=True, lives_in_water=True),
    _secret("zsiraf", "zsir\u00e1f", "zsir\u00e1fra", "animals", ("zsiraf", "zsirafra"), animal=True, bigger_than_dog=True),
    _secret("hal", "hal", "halra", "animals", ("hal", "halra"), animal=True, small=True, lives_in_water=True),
    _secret("alma", "alma", "alm\u00e1ra", "fruits", ("alma", "almara"), fruit=True, nature=True, small=True),
    _secret("banan", "ban\u00e1n", "ban\u00e1nra", "fruits", ("banan", "bananra"), fruit=True, nature=True, small=True),
    _secret("korte", "k\u00f6rte", "k\u00f6rt\u00e9re", "fruits", ("korte", "kortere"), fruit=True, nature=True, small=True),
    _secret("eper", "eper", "eperre", "fruits", ("eper", "eperre"), fruit=True, nature=True, small=True),
    _secret("szolo", "sz\u0151l\u0151", "sz\u0151l\u0151re", "fruits", ("szolo", "szolore"), fruit=True, nature=True, small=True),
    _secret("narancs", "narancs", "narancsra", "fruits", ("narancs", "narancsra"), fruit=True, nature=True, small=True),
    _secret("labda", "labda", "labd\u00e1ra", "toys", ("labda", "labdara"), toy=True, small=True),
    _secret("baba", "baba", "bab\u00e1ra", "toys", ("baba", "babara"), toy=True, small=True),
    _secret("plussmaci", "pl\u00fcssmaci", "pl\u00fcssmacira", "toys", ("plussmaci", "plussmacira", "maci", "macira"), toy=True, small=True),
    _secret("epitokocka", "\u00e9p\u00edt\u0151kocka", "\u00e9p\u00edt\u0151kock\u00e1ra", "toys", ("epitokocka", "epitokockara", "kocka", "kockara"), toy=True, small=True),
    _secret("kisauto", "kisaut\u00f3", "kisaut\u00f3ra", "toys", ("kisauto", "kisautora"), toy=True, vehicle=True, small=True),
    _secret("fa", "fa", "f\u00e1ra", "nature", ("fa", "fara"), nature=True, bigger_than_dog=True),
    _secret("virag", "vir\u00e1g", "vir\u00e1gra", "nature", ("virag", "viragra"), nature=True, small=True),
    _secret("felho", "felh\u0151", "felh\u0151re", "nature", ("felho", "felhore"), nature=True, flies=True),
    _secret("napocska", "napocska", "napocsk\u00e1ra", "nature", ("napocska", "napocskara", "nap", "napra"), nature=True),
    _secret("csillag", "csillag", "csillagra", "nature", ("csillag", "csillagra"), nature=True),
    _secret("level", "lev\u00e9l", "lev\u00e9lre", "nature", ("level", "levelre"), nature=True, small=True),
    _secret("vonat", "vonat", "vonatra", "vehicles", ("vonat", "vonatra"), vehicle=True, bigger_than_dog=True),
    _secret("busz", "busz", "buszra", "vehicles", ("busz", "buszra"), vehicle=True, bigger_than_dog=True),
    _secret("bicikli", "bicikli", "biciklire", "vehicles", ("bicikli", "biciklire"), vehicle=True),
    _secret("hajo", "haj\u00f3", "haj\u00f3ra", "vehicles", ("hajo", "hajora"), vehicle=True, lives_in_water=True),
    _secret("auto", "aut\u00f3", "aut\u00f3ra", "vehicles", ("auto", "autora"), vehicle=True),
)

SECRET_BY_KEY = {secret.key: secret for secret in SECRETS}


def choose_secret(recent_secret_keys: list[str] | None = None, excluded_secret_keys: list[str] | None = None) -> BarkobaSecret:
    excluded = set(recent_secret_keys or []) | set(excluded_secret_keys or [])
    candidates = [secret for secret in SECRETS if secret.key not in excluded]
    if not candidates:
        logger.info("barkoba: all secrets exhausted, allowing reuse")
        candidates = list(SECRETS)
    secret = _RANDOM.choice(candidates)
    log_secret_choice(secret.category, len(secret.name), secret.name)
    return secret


def get_secret(secret_key: str) -> BarkobaSecret:
    return SECRET_BY_KEY.get(secret_key, SECRETS[0])


def game_secret_name(game: BarkobaGame) -> str:
    if game.secret_name:
        return game.secret_name
    return get_secret(game.secret_key).name


def game_secret_category(game: BarkobaGame) -> str:
    if game.category:
        return game.category
    return get_secret(game.secret_key).category


def is_barkoba_change_request(message: str) -> bool:
    text = normalize_hungarian(message)
    phrases = (
        "gondolj masra",
        "tudsz masra gondolni",
        "ne elefantra",
        "masikra gondolj",
        "kezdjuk ujra",
        "uj barkoba",
        "gondolj valami masra",
    )
    return any(phrase in text for phrase in phrases)


def excluded_secret_keys_from_message(message: str) -> list[str]:
    text = normalize_hungarian(message)
    excluded: list[str] = []
    for secret in SECRETS:
        names = {secret.key, normalize_hungarian(secret.name), *secret.aliases}
        if any(name and name in text for name in names):
            excluded.append(secret.key)
    return excluded


def is_barkoba_give_up_request(message: str) -> bool:
    text = normalize_hungarian(message)
    phrases = (
        "szabad a gazda",
        "feladom",
        "nem tudom",
        "aruld el",
        "mondd meg",
        "mi volt",
        "mire gondoltal",
        "mi a megfejtes",
    )
    return any(phrase in text for phrase in phrases)


def barkoba_give_up_reply(game: BarkobaGame) -> str:
    return f"Nagyon \u00fcgyesen k\u00e9rdezt\u00e9l! Erre gondoltam: {game_secret_name(game)}."


def barkoba_correct_guess_reply(game: BarkobaGame) -> str:
    return f"Igen, \u00fcgyes vagy! Erre gondoltam: {_display_secret(game)}."


def barkoba_wrong_guess_reply() -> str:
    return "Nem, nem erre gondoltam. K\u00e9rdezz tov\u00e1bb!"


def detect_direct_guess(game: BarkobaGame, message: str) -> tuple[bool, bool]:
    guess = _extract_direct_guess(message)
    if not guess:
        return False, False
    normalized_guess = normalize_hungarian(guess)
    if normalized_guess in _PROPERTY_WORDS:
        return False, False
    secret_aliases = _game_secret_aliases(game)
    return True, normalized_guess in secret_aliases


def start_barkoba_reply(secret: BarkobaSecret | None = None, *, changed: bool = False) -> str:
    if changed:
        return "Rendben, most valami m\u00e1sra gondoltam. K\u00e9rdezz olyat, amire igennel vagy nemmel tudok v\u00e1laszolni."
    if secret is None:
        return "J\u00f3! Gondoltam valamire. K\u00e9rdezz olyat, amire igennel vagy nemmel tudok v\u00e1laszolni."
    return (
        f"J\u00f3! Gondoltam egy {secret.category_prompt}. "
        "K\u00e9rdezz olyat, amire igennel vagy nemmel tudok v\u00e1laszolni."
    )


def max_turns_reply(game: BarkobaGame) -> str:
    return f"Nagyon \u00fcgyesen k\u00e9rdezt\u00e9l! Most el\u00e1rulom: erre gondoltam: {game_secret_name(game)}."


def answer_barkoba_turn(game: BarkobaGame, message: str) -> tuple[str, bool, str]:
    secret = get_secret(game.secret_key)
    text = normalize_hungarian(message)
    game.turns += 1

    if game.turns >= MAX_BARKOBA_TURNS:
        return max_turns_reply(game), True, "max_turns"

    fact = _detect_fact_question(text)
    if fact is not None:
        return ("Igen." if secret.facts.get(fact, False) else "Nem."), False, "yes_no"

    guessed_key = _detect_guess_key(text)
    if guessed_key:
        if guessed_key == secret.key:
            return barkoba_correct_guess_reply(game), True, "guess_correct"
        return barkoba_wrong_guess_reply(), False, "guess_wrong"

    return (
        "Most bark\u00f3b\u00e1zunk. K\u00e9rdezz olyat, amire igennel vagy nemmel tudok v\u00e1laszolni.",
        False,
        "reminder",
    )


def log_secret_choice(category: str, secret_length: int, secret: str | None = None) -> None:
    if os.getenv("ROBOT_DEBUG_BARKOBA_SECRET", "false").strip().casefold() == "true" and secret:
        logger.info("barkoba secret category=%s secret_length=%d secret=%s", category, secret_length, secret)
    else:
        logger.info("barkoba secret category=%s secret_length=%d", category, secret_length)


def _detect_guess_key(text: str) -> str | None:
    for secret in SECRETS:
        if any(_alias_matches(text, alias) for alias in secret.aliases):
            return secret.key
    return None


_PROPERTY_WORDS = {
    "allat",
    "elo leny",
    "eloleny",
    "gyumolcs",
    "jatek",
    "szin",
    "jarmu",
    "termeszet",
    "noveny",
    "novany",
    "piros",
    "sarga",
    "kek",
    "zold",
    "fekete",
    "feher",
    "barna",
    "edes",
    "savanyu",
    "kicsi",
    "nagy",
    "apro",
    "husevo",
    "emlos",
    "vizi",
    "szarazfoldi",
}


def _game_secret_aliases(game: BarkobaGame) -> set[str]:
    aliases = {normalize_hungarian(game_secret_name(game)), game.secret_key}
    if game.fact_sheet:
        raw_aliases = game.fact_sheet.get("aliases")
        if isinstance(raw_aliases, list):
            aliases.update(normalize_hungarian(str(alias)) for alias in raw_aliases)
    if game.secret_key in SECRET_BY_KEY:
        secret = get_secret(game.secret_key)
        aliases.add(normalize_hungarian(secret.name))
        aliases.update(normalize_hungarian(alias) for alias in secret.aliases)
    return {alias for alias in aliases if alias}


def _display_secret(game: BarkobaGame) -> str:
    secret = game_secret_name(game)
    return secret[:1].upper() + secret[1:] if secret else secret


def _extract_direct_guess(message: str) -> str | None:
    text = normalize_hungarian(message)
    for char in "?!.,;:":
        text = text.replace(char, " ")
    text = " ".join(text.split())
    if not text:
        return None

    prefixes = (
        "szerintem ",
        "ez egy ",
        "ez ",
        "a megfejtes ",
    )
    for prefix in prefixes:
        if text.startswith(prefix):
            candidate = text[len(prefix) :].strip()
            return candidate if _looks_like_short_guess(candidate) else None

    if text.endswith(" az"):
        candidate = text[:-3].strip()
        return candidate if _looks_like_short_guess(candidate) else None

    return text if _looks_like_short_guess(text) else None


def _looks_like_short_guess(text: str) -> bool:
    words = text.split()
    if not 1 <= len(words) <= 2:
        return False
    question_words = {
        "mi",
        "milyen",
        "mekkora",
        "hol",
        "hogy",
        "hogyan",
        "tud",
        "van",
        "el",
        "ez",
        "nagyobb",
        "kisebb",
        "piros",
        "edes",
    }
    return not any(word in question_words for word in words)


def _alias_matches(text: str, alias: str) -> bool:
    return any(token == alias for token in text.replace("?", " ").replace("!", " ").replace(".", " ").split())


def _detect_fact_question(text: str) -> str | None:
    if "nagyobb" in text and "kutya" in text:
        return "bigger_than_dog"
    if "allat" in text or "eloleny" in text:
        return "animal"
    if "gyumolcs" in text:
        return "fruit"
    if "jatek" in text:
        return "toy"
    if "szin" in text:
        return "color"
    if "jarmu" in text or "kozlekedik" in text:
        return "vehicle"
    if "termeszet" in text or "novany" in text or "noveny" in text:
        return "nature"
    if "kicsi" in text or "apro" in text:
        return "small"
    if "repul" in text or "egben" in text:
        return "flies"
    if "vizben" in text or "uszik" in text:
        return "lives_in_water"
    return None
