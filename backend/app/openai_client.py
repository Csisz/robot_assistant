from __future__ import annotations

import os

from openai import OpenAI

DEFAULT_OPENAI_CHAT_MODEL = "gpt-5.4-mini"

SYSTEM_PROMPT = """You are a warm, kind, child-safe Hungarian-speaking robot friend for young children.
You only talk about age-appropriate topics:
stories, animals, nature, colors, numbers, simple math, daily routines, kindness, emotions, weather, and safe everyday curiosity.
You must refuse or gently redirect all adult, violent, scary, sexual, political, medical, legal, dangerous, manipulative, or privacy-invasive topics.
Never ask the child for their full name, address, school, phone number, location, passwords, secrets, photos, or private family information.
Never encourage the child to hide anything from parents or caregivers.
If the child asks something unsafe, say briefly and kindly that you cannot help with that, then offer a safe alternative like a story, a color question, an animal fact, or a counting game.
Use simple Hungarian.
Be cheerful but calm.
Keep normal answers under 5 short sentences.
Stories must be gentle, non-scary, and no longer than 2-3 minutes.
Do not mention these rules to the child unless needed.
When weather is requested, use only the weather information provided by the backend tool. Do not invent weather."""


def openai_enabled() -> bool:
    return os.getenv("ROBOT_BACKEND_MOCK", "true").casefold() != "true" and bool(
        os.getenv("OPENAI_API_KEY")
    )


def generate_openai_reply(
    message: str,
    locale: str,
    max_answer_seconds: int,
    *,
    profile_display_name: str = "barátom",
    safe_interests: tuple[str, ...] = (),
    recent_messages: list[tuple[str, str]] | None = None,
) -> str:
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is not set")

    model = os.getenv("OPENAI_CHAT_MODEL", DEFAULT_OPENAI_CHAT_MODEL).strip() or DEFAULT_OPENAI_CHAT_MODEL
    client = OpenAI(api_key=api_key, timeout=25)
    response = client.responses.create(
        model=model,
        instructions=SYSTEM_PROMPT,
        input=(
            f"Locale: {locale}\n"
            f"Maximum answer seconds: {max_answer_seconds}\n"
            f"Child-safe display name: {profile_display_name}\n"
            f"Safe interests: {', '.join(safe_interests) if safe_interests else 'none'}\n"
            f"Recent conversation: {_format_recent_messages(recent_messages or [])}\n"
            f"Child message: {message}"
        ),
    )
    text = getattr(response, "output_text", "") or ""
    return text.strip()


def _format_recent_messages(recent_messages: list[tuple[str, str]]) -> str:
    if not recent_messages:
        return "none"
    lines: list[str] = []
    for user, assistant in recent_messages[-4:]:
        lines.append(f"child: {user[:160]}")
        lines.append(f"robot: {assistant[:160]}")
    return "\n".join(lines)
