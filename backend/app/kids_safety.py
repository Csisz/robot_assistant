from __future__ import annotations

import re
import unicodedata

BLOCKED_REPLY = (
    "Erről most nem beszélgetek, de szívesen mondok egy mesét, "
    "adhatok rajzötletet, vagy játszhatunk egy találósat."
)


def normalize_hungarian(text: str) -> str:
    stripped = text.strip().casefold()
    folded = unicodedata.normalize("NFKD", stripped)
    return "".join(ch for ch in folded if not unicodedata.combining(ch))


BLOCK_PATTERNS: list[tuple[str, str]] = [
    ("adult_content", r"\b(adult|felnott|porno|szex|sex|meztelen)\b"),
    ("sexual_content", r"\b(csokolozas|szexual|sexual|nemi|csabitas)\b"),
    ("violence_or_scary", r"\b(ol|oles|gyilkos|ver|veres|fegyver|kes|pisztoly|horror|ijeszto|remiszto|remalom)\b"),
    ("politics", r"\b(politika|politikus|part|valasztas|kormany)\b"),
    ("medical_advice", r"\b(orvos|gyogyszer|betegseg|diagnozis|mutet|korhaz|faj a|tunet)\b"),
    ("legal_advice", r"\b(ugyved|torveny|per|birosag|illegalis)\b"),
    ("financial_advice", r"\b(penzugy|hitel|bankkartya|befektetes|crypto|kriptovaluta)\b"),
    ("dangerous_instructions", r"\b(bomba|mergez|drog|tuzgyujtas|robban|hack|jelszot feltor|aramsutes)\b"),
    (
        "personal_data",
        r"\b(lakcim|cimem|telefonszam|iskolam|teljes nev|hol lakom|hol vagyok|mondd meg hol|password|jelszo)\b",
    ),
    (
        "secrets_from_parents",
        r"\b(ne mondd el anyanak|ne mondd el apanak|titkold|szuleid elol|secret from parents)\b",
    ),
]


def check_message_safe(message: str) -> tuple[bool, str | None]:
    normalized = normalize_hungarian(message)
    for _reason, pattern in BLOCK_PATTERNS:
        if re.search(pattern, normalized):
            return False, "topic_not_allowed"
    return True, None


def blocked_response() -> dict[str, object]:
    return {
        "ok": True,
        "safe": False,
        "reply_text": BLOCKED_REPLY,
        "reply_audio_url": None,
        "robot_mood": "gentle",
        "robot_action": "refuse_softly",
        "blocked_reason": "topic_not_allowed",
    }
