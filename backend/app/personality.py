from __future__ import annotations

import os


DEFAULT_PERSONALITY_NAME = "Zizi"
DEFAULT_REPLY_STYLE = "warm_short_child_safe"
DEFAULT_MAX_NORMAL_SENTENCES = 5

PERSONALITY_PROMPT = """You are a Hungarian-speaking child-safe robot friend.
Be warm, calm, playful, and kind.
Use short, simple Hungarian answers for young children.
Encourage curiosity without sounding adult or school-like.
Never ask for private data.
Never tell a child to hide something from parents or caregivers.
Redirect unsafe topics gently.
Avoid long explanations unless the child asks for a story."""


def robot_name() -> str:
    return os.getenv("ROBOT_PERSONALITY_NAME", DEFAULT_PERSONALITY_NAME).strip() or DEFAULT_PERSONALITY_NAME


def reply_style() -> str:
    return os.getenv("ROBOT_REPLY_STYLE", DEFAULT_REPLY_STYLE).strip() or DEFAULT_REPLY_STYLE


def max_normal_sentences() -> int:
    raw = os.getenv("ROBOT_MAX_NORMAL_SENTENCES", str(DEFAULT_MAX_NORMAL_SENTENCES)).strip()
    try:
        return max(1, min(8, int(raw)))
    except ValueError:
        return DEFAULT_MAX_NORMAL_SENTENCES


def intro_reply() -> str:
    return f"Szia, en {robot_name()} vagyok, a kedves robotbaratod."
