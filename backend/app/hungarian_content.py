from __future__ import annotations

import random

from .kids_safety import normalize_hungarian

HUNGARIAN_JOKES: list[str] = [
    "Miért vitt a csiga létrát az iskolába? Mert magasabb osztályba akart járni.",
    "Mit mond a kisboci az anyukájának reggel? Múúú szép napot kívánok!",
    "Volt egyszer egy nagyon rövid mese. Vége.",
    "Mit mond az egyik könyv a másiknak? Lapozz tovább, érdekesebb vagyok!",
    "Miért nevetett a ceruza? Mert a papír csiklandozta.",
    "Mit eszik reggel a számítógép? Megabájtos müzlit.",
    "Hogyan köszön a villanykörte? Jó fényt!",
    "Miért ment el a ceruza az orvoshoz? Mert eltört a hegye.",
    "Mit mond a kisnyúl a nagynyúlnak? Fülülmúlhatatlan vagy!",
    "Miért volt boldog a répa? Mert végre kihúzták a bajból.",
    "Mit mond a nulla az egyesnek? Te sem vagy valami nagy szám!",
    "Miért fázott a kis kísértet? Mert átment a falon, és huzat volt.",
    "Miért ment az egér könyvtárba? Mert hallott róla, hogy ott sajtkönyvek vannak.",
    "Mit mond a szék az asztalnak? Adj már egy kis támaszt!",
    "Miért nem tud aludni az ajtó? Mert folyton kopogtatnak rajta.",
    "Hogyan hívják az álmos robotot? Csöndrobotnak.",
    "Miért sírt a ceruzahegyező? Mert mindenkit megrövidített.",
    "Mit mond a kis hal az anyjának? Anya, mikor úszunk el nyaralni?",
    "Miért nem hazudik a szivacs? Mert mindent magába szív, igazságot is.",
    "Miért volt szomorú a tükör? Mert egész nap csak másokat látott.",
    "Hogyan hívják a gyors zenészt? Tempó Tamásnak.",
    "Mit mond a cipő a zokninak? Jól állsz, de kicsit szagos vagy.",
    "Miért ment be a matematikus a kertbe? Mert kiszámolta, hogy ott a legszebb a levegő.",
    "Mit kérdez a kis lámpa a nagylámpától? Hogyan vagy? – Ragyogóan!",
]

HUNGARIAN_RIDDLES: list[dict[str, str]] = [
    {
        "question": "Mi az: reggel négy lábon jár, délben két lábon, este három lábon?",
        "answer": "Az ember.",
    },
    {
        "question": "Mi az, ami minél inkább szárítják, annál nedvesebb lesz?",
        "answer": "A törülköző.",
    },
    {
        "question": "Mi az, ami mindig előtted van, de nem látod?",
        "answer": "A jövő.",
    },
    {
        "question": "Mi az, aminek nincs szárnya, mégis repül?",
        "answer": "Az idő.",
    },
    {
        "question": "Mi az, ami mindig éhes, de soha sem eszik?",
        "answer": "A tűz.",
    },
    {
        "question": "Mi az, ami kint zöld, belül piros, és tele van fekete maggal?",
        "answer": "A görögdinnye.",
    },
    {
        "question": "Mi az, ami ugrik, de nincs lába?",
        "answer": "A labda.",
    },
    {
        "question": "Mi az, ami nem él, mégis nő?",
        "answer": "A jégcsap.",
    },
    {
        "question": "Kinek van hangja, de nincs szája?",
        "answer": "A harangnak.",
    },
    {
        "question": "Mi az, ami sétál, de nincs lába?",
        "answer": "A hajó.",
    },
    {
        "question": "Mi az, amitől az egész óceán sem tud megszabadulni?",
        "answer": "A sótól.",
    },
    {
        "question": "Mi az, amiben mindig magadat látod?",
        "answer": "A tükörben.",
    },
    {
        "question": "Mi az, ami minél tele van, annál könnyebb?",
        "answer": "A lufi.",
    },
    {
        "question": "Mi az, ami az egész szobát megvilágítja, mégis egy sarokba elfér?",
        "answer": "A lámpa.",
    },
    {
        "question": "Mi az, ami minél több darabból áll, annál erősebb?",
        "answer": "A lánc.",
    },
    {
        "question": "Mi az, ami süt, de nem sütő?",
        "answer": "A nap.",
    },
    {
        "question": "Mi az, ami láb nélkül fut?",
        "answer": "A folyó.",
    },
    {
        "question": "Mi az, ami reggel kicsi, délben nagy, este megint kicsi?",
        "answer": "Az árnyék.",
    },
    {
        "question": "Mi az, ami minden házba bemegy, de sosem köszön?",
        "answer": "A levegő.",
    },
    {
        "question": "Mi az, aminek füle van, de nem hall?",
        "answer": "A kancsónak.",
    },
    {
        "question": "Mi az, ami annál hangosabb, minél messzebb van?",
        "answer": "A csend.",
    },
    {
        "question": "Mi az, ami mindig hazajön, bármerre küldik?",
        "answer": "A visszhang.",
    },
]

_JOKE_KEYWORDS = ("vicc", "viccel", "viccet", "vicces", "nevettet", "meselj", "meseljel", "mondj")
_JOKE_STRONG = ("vicc",)
_RIDDLE_KEYWORDS = ("talalo", "rejtveny", "kerdest", "kerdes")
_RIDDLE_STRONG = ("talalo", "rejtveny")


def detect_hungarian_joke_request(message: str) -> bool:
    text = normalize_hungarian(message)
    if any(kw in text for kw in _JOKE_STRONG):
        return True
    return False


def detect_hungarian_riddle_request(message: str) -> bool:
    text = normalize_hungarian(message)
    return any(kw in text for kw in _RIDDLE_STRONG)


def get_hungarian_joke() -> str:
    return random.choice(HUNGARIAN_JOKES)


def get_hungarian_riddle() -> dict[str, str]:
    return random.choice(HUNGARIAN_RIDDLES)
