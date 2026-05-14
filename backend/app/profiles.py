from __future__ import annotations

from dataclasses import dataclass

from .kids_safety import normalize_hungarian


@dataclass(frozen=True)
class ChildProfile:
    profile_id: str
    display_name: str
    safe_interests: tuple[str, ...]


PROFILES: dict[str, ChildProfile] = {
    "zita": ChildProfile("zita", "Zita", ("unikornis", "tündérek", "színek", "mesék")),
    "ida": ChildProfile("ida", "Ida", ("állatok", "dalok", "mesék")),
    "guest": ChildProfile("guest", "barátom", ()),
}


def normalize_profile_id(profile_id: str | None) -> str:
    return normalize_hungarian(profile_id or "")


def is_known_profile(profile_id: str | None) -> bool:
    normalized = normalize_profile_id(profile_id)
    return bool(normalized) and normalized in PROFILES


def get_profile(profile_id: str | None) -> ChildProfile:
    normalized = normalize_profile_id(profile_id)
    if not normalized:
        return PROFILES["guest"]
    return PROFILES.get(normalized, PROFILES["guest"])


def detect_profile_claim(message: str) -> tuple[str | None, bool]:
    text = normalize_hungarian(message)
    for profile_id in PROFILES:
        if profile_id == "guest":
            continue
        if (
            f"{profile_id} vagyok" in text
            or f"a nevem {profile_id}" in text
            or f"hivj {profile_id}" in text
        ):
            return profile_id, True

    if " vagyok" in text or "a nevem " in text or "hivj " in text:
        return None, False
    return None, False


def profile_claim_reply(profile: ChildProfile, *, known: bool) -> str:
    if known and profile.profile_id != "guest":
        return (
            f"Szia, {profile.display_name}! Örülök, hogy itt vagy. "
            "Mesélhetek neked rövid mesét, válaszolhatok színes kérdésekre, vagy játszhatunk barkóbát."
        )
    return "Szia! Örülök, hogy itt vagy. Egyelőre barátomnak foglak szólítani."
