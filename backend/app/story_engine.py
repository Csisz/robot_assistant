from __future__ import annotations

import json
import logging
import os
from dataclasses import dataclass

from openai import OpenAI

from .barkoba_gpt import openai_chat_model
from .kids_safety import check_message_safe, normalize_hungarian
from .profiles import ChildProfile

logger = logging.getLogger("robot_backend.story")

STORY_ERROR_REPLY = "Most nem siker\u00fclt sz\u00e9pen mes\u00e9t mondanom, de pr\u00f3b\u00e1ljuk meg \u00fajra egy m\u00e1sik mes\u00e9vel."
MAX_STORY_CHARS = 3200


@dataclass(frozen=True)
class StoryReply:
    reply_text: str
    answer_source: str
    title: str | None = None
    estimated_seconds: int | None = None


def story_engine_name() -> str:
    value = os.getenv("ROBOT_STORY_ENGINE", "gpt").strip().casefold()
    return value if value in ("mock", "gpt") else "gpt"


def story_gpt_enabled() -> bool:
    return story_engine_name() == "gpt" and bool(os.getenv("OPENAI_API_KEY"))


def generate_story_reply(
    *,
    message: str,
    locale: str,
    max_answer_seconds: int,
    profile: ChildProfile,
) -> StoryReply:
    if not story_gpt_enabled():
        raise RuntimeError("story GPT is not enabled")
    prompt = _story_prompt(message, locale, max_answer_seconds, profile)
    try:
        payload = json.loads(_call_openai_text(prompt))
        title = str(payload.get("title", "")).strip()
        story_text = str(payload.get("story_text", "")).strip()
        estimated_raw = payload.get("estimated_seconds", None)
        estimated_seconds = int(estimated_raw) if estimated_raw is not None else None
        _validate_story(story_text)
        logger.info(
            "story_engine=gpt profile_id=%s title=%s story_length=%d estimated_seconds=%s answer_source=story_gpt",
            profile.profile_id,
            title,
            len(story_text),
            estimated_seconds,
        )
        return StoryReply(
            reply_text=story_text,
            answer_source="story_gpt",
            title=title or None,
            estimated_seconds=estimated_seconds,
        )
    except Exception as exc:
        logger.exception("story OpenAI error answer_source=story_gpt_error reason=%s", exc)
        return StoryReply(reply_text=STORY_ERROR_REPLY, answer_source="story_gpt_error")


def _story_prompt(message: str, locale: str, max_answer_seconds: int, profile: ChildProfile) -> str:
    interests = ", ".join(profile.safe_interests) if profile.safe_interests else "none"
    topic = _story_topic(message)
    logger.info("story_engine=%s profile_id=%s story_topic=%s", story_engine_name(), profile.profile_id, topic)
    return (
        "Create a child-safe short Hungarian story. Return strict JSON only with keys: "
        '{"title":"...","story_text":"...","mood":"gentle","estimated_seconds":90}.\n'
        f"Locale: {locale}\n"
        f"Maximum answer seconds requested: {max_answer_seconds}\n"
        f"Child-safe display name: {profile.display_name}\n"
        f"Safe profile interests: {interests}\n"
        f"Story request: {message}\n"
        "Rules: Hungarian language, for young children, warm, kind, calm tone. "
        "No scary content, violence, adult topics, politics, medical or legal advice, personal data requests, "
        "secrets from parents, death, kidnapping, monsters, horror, or danger. "
        "Default length: 8-14 short sentences, maximum 2-3 minutes. "
        "Gentle themes: kindness, courage, friendship, patience, asking for help, sharing, curiosity."
    )


def _call_openai_text(prompt: str) -> str:
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is not set")
    model = openai_chat_model()
    client = OpenAI(api_key=api_key, timeout=30)
    response = client.responses.create(model=model, input=prompt)
    return (getattr(response, "output_text", "") or "").strip()


def _validate_story(story_text: str) -> None:
    if not story_text:
        raise ValueError("missing story_text")
    if len(story_text) > MAX_STORY_CHARS:
        raise ValueError("story too long")
    safe, reason = check_message_safe(story_text)
    if not safe:
        raise ValueError(f"unsafe story: {reason}")


def _story_topic(message: str) -> str:
    text = normalize_hungarian(message)
    if "nyusz" in text:
        return "nyuszi"
    if "unikornis" in text:
        return "unikornis"
    if "altato" in text:
        return "altato"
    if "tunder" in text:
        return "tunder"
    if "bator" in text:
        return "batorsag"
    return "short_story"
