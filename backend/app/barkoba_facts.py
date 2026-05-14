from __future__ import annotations

from copy import deepcopy
from typing import Any

from .kids_safety import normalize_hungarian

COMMON_FACT_KEYS = (
    "is_animal",
    "is_fruit",
    "is_living_thing",
    "is_mammal",
    "is_pet",
    "is_carnivore",
    "is_herbivore",
    "bigger_than_dog",
    "smaller_than_dog",
    "smaller_than_palm",
    "lives_in_water",
    "can_swim",
    "has_fins",
    "has_trunk",
    "has_tail",
    "has_fur",
    "has_big_ears",
    "has_shell",
    "can_fly",
    "meows",
    "barks",
    "is_vehicle",
    "is_toy",
    "is_nature_object",
    "is_color",
    "is_edible",
    "is_sweet",
    "can_roll",
    "is_red",
)


def make_fact_sheet(secret: str, category: str) -> tuple[dict[str, Any], str]:
    key = normalize_hungarian(secret)
    if key in BUILTIN_FACT_SHEETS:
        return deepcopy(BUILTIN_FACT_SHEETS[key]), "barkoba_fact_builtin"
    return _generic_sheet(secret, category), "barkoba_fact_fallback"


def validate_fact_sheet(sheet: dict[str, Any]) -> bool:
    if not isinstance(sheet.get("secret"), str) or not sheet["secret"].strip():
        return False
    if not isinstance(sheet.get("category"), str) or not sheet["category"].strip():
        return False
    aliases = sheet.get("aliases")
    if not isinstance(aliases, list) or not aliases or not all(isinstance(alias, str) for alias in aliases):
        return False
    facts = sheet.get("facts")
    if not isinstance(facts, dict):
        return False
    if not all(isinstance(value, bool) for value in facts.values()):
        return False
    return all(key in facts for key in COMMON_FACT_KEYS)


def fact_sheet_aliases(sheet: dict[str, Any] | None) -> set[str]:
    if not sheet:
        return set()
    aliases = sheet.get("aliases")
    if not isinstance(aliases, list):
        return set()
    return {normalize_hungarian(str(alias)) for alias in aliases if str(alias).strip()}


def answer_from_fact_sheet(sheet: dict[str, Any] | None, message: str) -> tuple[bool, bool, str | None]:
    if not sheet:
        return False, False, None
    fact_key = map_question_to_fact_key(message)
    if not fact_key:
        return False, False, None
    facts = sheet.get("facts")
    if not isinstance(facts, dict) or fact_key not in facts or not isinstance(facts[fact_key], bool):
        return False, False, None
    return True, bool(facts[fact_key]), fact_key


def map_question_to_fact_key(message: str) -> str | None:
    text = normalize_hungarian(message)
    if "allat" in text:
        return "is_animal"
    if "gyumolcs" in text:
        return "is_fruit"
    if "eloleny" in text or "elo leny" in text or "el-e" in text:
        return "is_living_thing"
    if "emlos" in text:
        return "is_mammal"
    if "hazi kedvenc" in text or "haziallat" in text or "kis kedvenc" in text:
        return "is_pet"
    if "husevo" in text or "hust eszik" in text:
        return "is_carnivore"
    if "novenyevo" in text or "fuvet eszik" in text or "levelet eszik" in text:
        return "is_herbivore"
    if "nagyobb" in text and "kutya" in text:
        return "bigger_than_dog"
    if "kisebb" in text and "kutya" in text:
        return "smaller_than_dog"
    if "kisebb" in text and ("tenyer" in text or "kezem" in text):
        return "smaller_than_palm"
    if "vizben el" in text or "vizben lakik" in text or "viz alatt" in text:
        return "lives_in_water"
    if "uszik" in text or "tud uszni" in text:
        return "can_swim"
    if "uszonya" in text or "uszony" in text:
        return "has_fins"
    if "ormanya" in text or "ormany" in text:
        return "has_trunk"
    if "farka" in text or "farok" in text:
        return "has_tail"
    if "szoros" in text or "szore" in text:
        return "has_fur"
    if "nagy fule" in text or "nagy ful" in text:
        return "has_big_ears"
    if "pancelja" in text or "pancel" in text:
        return "has_shell"
    if "repul" in text or "tud repulni" in text:
        return "can_fly"
    if "nyavog" in text:
        return "meows"
    if "ugat" in text:
        return "barks"
    if "piros" in text:
        return "is_red"
    if "edes" in text:
        return "is_sweet"
    return None


def _sheet(secret: str, category: str, aliases: list[str], facts: dict[str, bool]) -> dict[str, Any]:
    merged = {key: False for key in COMMON_FACT_KEYS}
    merged.update(facts)
    return {
        "secret": secret,
        "category": category,
        "aliases": aliases,
        "facts": merged,
    }


def _generic_sheet(secret: str, category: str) -> dict[str, Any]:
    key = normalize_hungarian(secret)
    facts: dict[str, bool] = {
        "is_animal": category == "animals",
        "is_fruit": category == "fruits",
        "is_living_thing": category in ("animals", "fruits", "nature"),
        "is_vehicle": category == "vehicles",
        "is_toy": category == "toys",
        "is_nature_object": category == "nature",
        "is_color": category == "colors",
        "is_edible": category == "fruits",
        "is_sweet": category == "fruits",
        "smaller_than_dog": category in ("fruits", "toys", "colors"),
    }
    return _sheet(secret, category, [secret, key], facts)


BUILTIN_FACT_SHEETS: dict[str, dict[str, Any]] = {
    "elefant": _sheet(
        "elef\u00e1nt",
        "animals",
        ["elef\u00e1nt", "elefant"],
        {
            "is_animal": True,
            "is_living_thing": True,
            "is_mammal": True,
            "is_herbivore": True,
            "bigger_than_dog": True,
            "smaller_than_palm": False,
            "lives_in_water": False,
            "can_swim": True,
            "has_fins": False,
            "has_trunk": True,
            "has_tail": True,
            "has_big_ears": True,
        },
    ),
    "hal": _sheet(
        "hal",
        "animals",
        ["hal"],
        {
            "is_animal": True,
            "is_living_thing": True,
            "is_mammal": False,
            "is_carnivore": False,
            "bigger_than_dog": False,
            "smaller_than_dog": True,
            "smaller_than_palm": True,
            "lives_in_water": True,
            "can_swim": True,
            "has_fins": True,
            "has_tail": True,
        },
    ),
    "alma": _sheet(
        "alma",
        "fruits",
        ["alma", "alm\u00e1ra", "almara"],
        {
            "is_fruit": True,
            "is_living_thing": False,
            "is_red": True,
            "is_sweet": True,
            "bigger_than_dog": False,
            "smaller_than_dog": True,
            "smaller_than_palm": False,
        },
    ),
    "vidra": _sheet(
        "vidra",
        "animals",
        ["vidra", "vidr\u00e1ra", "vidrara"],
        {
            "is_animal": True,
            "is_living_thing": True,
            "is_mammal": True,
            "is_carnivore": True,
            "bigger_than_dog": False,
            "smaller_than_dog": False,
            "smaller_than_palm": False,
            "lives_in_water": True,
            "can_swim": True,
            "has_fins": False,
            "has_tail": True,
            "has_fur": True,
        },
    ),
    "macska": _sheet("macska", "animals", ["macska", "cica"], {"is_animal": True, "is_living_thing": True, "is_mammal": True, "is_pet": True, "is_carnivore": True, "smaller_than_dog": True, "smaller_than_palm": False, "has_tail": True, "has_fur": True, "meows": True}),
    "kutya": _sheet("kutya", "animals", ["kutya"], {"is_animal": True, "is_living_thing": True, "is_mammal": True, "is_pet": True, "is_carnivore": True, "smaller_than_palm": False, "has_tail": True, "has_fur": True, "barks": True}),
    "nyuszi": _sheet("nyuszi", "animals", ["nyuszi", "nyul"], {"is_animal": True, "is_living_thing": True, "is_mammal": True, "is_pet": True, "is_herbivore": True, "smaller_than_dog": True, "smaller_than_palm": False, "has_tail": True, "has_fur": True}),
    "teknos": _sheet("tekn\u0151s", "animals", ["tekn\u0151s", "teknos"], {"is_animal": True, "is_living_thing": True, "smaller_than_dog": True, "has_shell": True, "has_tail": True, "lives_in_water": False, "can_swim": True}),
    "pingvin": _sheet("pingvin", "animals", ["pingvin"], {"is_animal": True, "is_living_thing": True, "is_carnivore": True, "smaller_than_dog": True, "lives_in_water": False, "can_swim": True, "has_fins": False, "can_fly": False}),
    "delfin": _sheet("delfin", "animals", ["delfin"], {"is_animal": True, "is_living_thing": True, "is_mammal": True, "is_carnivore": True, "bigger_than_dog": True, "lives_in_water": True, "can_swim": True, "has_fins": True, "has_tail": True}),
    "lo": _sheet("l\u00f3", "animals", ["l\u00f3", "lo"], {"is_animal": True, "is_living_thing": True, "is_mammal": True, "is_pet": False, "is_herbivore": True, "bigger_than_dog": True, "smaller_than_palm": False, "has_tail": True, "has_fur": True}),
    "pillango": _sheet("pillang\u00f3", "animals", ["pillang\u00f3", "pillango"], {"is_animal": True, "is_living_thing": True, "smaller_than_dog": True, "smaller_than_palm": True, "can_fly": True}),
    "zsiraf": _sheet("zsir\u00e1f", "animals", ["zsir\u00e1f", "zsiraf"], {"is_animal": True, "is_living_thing": True, "is_mammal": True, "is_herbivore": True, "bigger_than_dog": True, "smaller_than_palm": False, "has_tail": True, "has_fur": True}),
    "banan": _sheet("ban\u00e1n", "fruits", ["ban\u00e1n", "banan"], {"is_fruit": True, "is_edible": True, "is_sweet": True, "is_living_thing": False, "smaller_than_dog": True, "smaller_than_palm": False}),
    "korte": _sheet("k\u00f6rte", "fruits", ["k\u00f6rte", "korte"], {"is_fruit": True, "is_edible": True, "is_sweet": True, "is_living_thing": False, "smaller_than_dog": True, "smaller_than_palm": False}),
    "eper": _sheet("eper", "fruits", ["eper"], {"is_fruit": True, "is_edible": True, "is_sweet": True, "is_living_thing": False, "is_red": True, "smaller_than_dog": True, "smaller_than_palm": True}),
    "szolo": _sheet("sz\u0151l\u0151", "fruits", ["sz\u0151l\u0151", "szolo"], {"is_fruit": True, "is_edible": True, "is_sweet": True, "is_living_thing": False, "smaller_than_dog": True, "smaller_than_palm": True}),
    "narancs": _sheet("narancs", "fruits", ["narancs"], {"is_fruit": True, "is_edible": True, "is_sweet": True, "is_living_thing": False, "smaller_than_dog": True, "smaller_than_palm": False}),
    "labda": _sheet("labda", "toys", ["labda"], {"is_toy": True, "can_roll": True, "smaller_than_dog": True}),
    "baba": _sheet("baba", "toys", ["baba"], {"is_toy": True, "smaller_than_dog": True}),
    "plussmaci": _sheet("pl\u00fcssmaci", "toys", ["pl\u00fcssmaci", "plussmaci", "maci"], {"is_toy": True, "has_fur": True, "smaller_than_dog": True}),
    "epitokocka": _sheet("\u00e9p\u00edt\u0151kocka", "toys", ["\u00e9p\u00edt\u0151kocka", "epitokocka", "kocka"], {"is_toy": True, "smaller_than_dog": True}),
    "kisauto": _sheet("kisaut\u00f3", "toys", ["kisaut\u00f3", "kisauto"], {"is_toy": True, "is_vehicle": True, "can_roll": True, "smaller_than_dog": True}),
    "fa": _sheet("fa", "nature", ["fa"], {"is_nature_object": True, "is_living_thing": True, "bigger_than_dog": True}),
    "virag": _sheet("vir\u00e1g", "nature", ["vir\u00e1g", "virag"], {"is_nature_object": True, "is_living_thing": True, "smaller_than_dog": True}),
    "felho": _sheet("felh\u0151", "nature", ["felh\u0151", "felho"], {"is_nature_object": True, "can_fly": False, "bigger_than_dog": True}),
    "napocska": _sheet("napocska", "nature", ["napocska", "nap"], {"is_nature_object": True, "bigger_than_dog": True}),
    "csillag": _sheet("csillag", "nature", ["csillag"], {"is_nature_object": True, "bigger_than_dog": True}),
    "level": _sheet("lev\u00e9l", "nature", ["lev\u00e9l", "level"], {"is_nature_object": True, "smaller_than_dog": True, "smaller_than_palm": True}),
    "vonat": _sheet("vonat", "vehicles", ["vonat"], {"is_vehicle": True, "bigger_than_dog": True, "can_roll": True}),
    "busz": _sheet("busz", "vehicles", ["busz"], {"is_vehicle": True, "bigger_than_dog": True, "can_roll": True}),
    "bicikli": _sheet("bicikli", "vehicles", ["bicikli"], {"is_vehicle": True, "smaller_than_dog": False, "can_roll": True}),
    "hajo": _sheet("haj\u00f3", "vehicles", ["haj\u00f3", "hajo"], {"is_vehicle": True, "lives_in_water": False, "can_swim": False}),
    "auto": _sheet("aut\u00f3", "vehicles", ["aut\u00f3", "auto"], {"is_vehicle": True, "bigger_than_dog": True, "can_roll": True}),
}
