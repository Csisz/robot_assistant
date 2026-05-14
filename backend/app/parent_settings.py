from __future__ import annotations

import os
from typing import Any

from .profiles import PROFILES


def parent_mode_enabled() -> bool:
    return os.getenv("ROBOT_PARENT_MODE_ENABLED", "true").strip().casefold() in ("1", "true", "yes", "on")


def parent_pin() -> str:
    return os.getenv("ROBOT_PARENT_PIN", "1234").strip()


def authorized(pin: str | None) -> bool:
    return bool(pin) and pin == parent_pin()


def settings_payload() -> dict[str, Any]:
    return {
        "ok": True,
        "parent_mode_enabled": parent_mode_enabled(),
        "tts_enabled": os.getenv("ROBOT_TTS_ENABLED", "true").strip().casefold() in ("1", "true", "yes", "on"),
        "mock_mode": os.getenv("ROBOT_BACKEND_MOCK", "true").strip().casefold() == "true",
        "allowed_activities": _csv("ROBOT_ALLOWED_ACTIVITIES", "story,barkoba,quiz,drawing,creative,joke,calm,daily"),
        "max_story_seconds": _int_env("ROBOT_MAX_STORY_SECONDS", 180),
        "max_daily_turns": _int_env("ROBOT_MAX_DAILY_TURNS", 100),
        "profiles": [
            {
                "profile_id": profile.profile_id,
                "display_name": profile.display_name,
                "safe_interests": list(profile.safe_interests),
            }
            for profile in PROFILES.values()
        ],
    }


def update_settings(values: dict[str, Any]) -> dict[str, Any]:
    # Development-only placeholder: environment remains the source of truth for now.
    payload = settings_payload()
    payload["updated"] = False
    payload["message"] = "Runtime parent setting persistence is not enabled yet."
    payload["requested"] = {key: value for key, value in values.items() if key != "pin"}
    return payload


def _csv(name: str, default: str) -> list[str]:
    raw = os.getenv(name, default)
    return [item.strip() for item in raw.split(",") if item.strip()]


def _int_env(name: str, default: int) -> int:
    try:
        return int(os.getenv(name, str(default)).strip())
    except ValueError:
        return default
