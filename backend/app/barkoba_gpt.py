from __future__ import annotations

import json
import logging
import os
from dataclasses import dataclass
from typing import Any

from openai import OpenAI

from .kids_safety import check_message_safe, normalize_hungarian

logger = logging.getLogger("robot_backend.barkoba_gpt")

ALLOWED_CATEGORIES = ("animals", "fruits", "toys", "nature", "colors", "vehicles")
SAFE_FALLBACK_REPLY = (
    "Most egy kicsit \u00f6sszezavarodtam. K\u00e9rdezz \u00fajra olyat, "
    "amire igennel vagy nemmel tudok v\u00e1laszolni."
)
BARKOBA_MODEL_CONFIG_REPLY = "Most nem tudok j\u00f3l bark\u00f3b\u00e1zni, mert a besz\u00e9lget\u0151s modell be\u00e1ll\u00edt\u00e1sa hib\u00e1s."
DEFAULT_OPENAI_CHAT_MODEL = "gpt-5.4-mini"
WRONG_GUESS_REPLY = "Nem, nem erre gondoltam. K\u00e9rdezz tov\u00e1bb!"
UNCLEAR_REPLY = "Ezt most nem tudom j\u00f3l eld\u00f6nteni. K\u00e9rdezz olyat, amire igennel vagy nemmel tudok v\u00e1laszolni."


class GptBarkobaModelConfigError(RuntimeError):
    def __init__(self, model: str) -> None:
        super().__init__(f"Configured OPENAI_CHAT_MODEL is invalid or unavailable: {model}")
        self.model = model


@dataclass(frozen=True)
class GptSecret:
    secret: str
    category: str


@dataclass(frozen=True)
class GptBarkobaTurn:
    reply_text: str
    game_over: bool
    active_mode: str
    turn_count_increment: int
    reason: str
    parse_ok: bool = True


def configured_engine() -> str:
    value = os.getenv("ROBOT_BARKOBA_ENGINE", "gpt").strip().casefold()
    return value if value in ("gpt", "deterministic") else "gpt"


def openai_chat_model() -> str:
    return os.getenv("OPENAI_CHAT_MODEL", DEFAULT_OPENAI_CHAT_MODEL).strip() or DEFAULT_OPENAI_CHAT_MODEL


def should_use_gpt_engine() -> bool:
    return configured_engine() == "gpt" and bool(os.getenv("OPENAI_API_KEY"))


def choose_gpt_secret(
    recent_secret_keys: list[str] | None = None,
    excluded_secret_keys: list[str] | None = None,
) -> GptSecret:
    recent_keys = set(recent_secret_keys or []) | set(excluded_secret_keys or [])
    recent = ", ".join(sorted(recent_keys)) or "none"
    prompt = (
        "Choose one child-safe Hungarian barkoba secret. Return strict JSON only with keys: "
        '{"secret":"...","category":"animals|fruits|toys|nature|colors|vehicles"}.\n'
        "Allowed categories: animals, fruits, toys, nature, colors, vehicles.\n"
        "Forbidden: scary things, weapons, violence, adult topics, real people, politics, illness, private data.\n"
        f"Avoid these recent normalized secrets if possible: {recent}."
    )
    last_secret: GptSecret | None = None
    for attempt in range(2):
        payload = _parse_json(_call_openai_text(prompt))
        secret = str(payload.get("secret", "")).strip()
        category = str(payload.get("category", "")).strip().casefold()
        if not _valid_secret(secret, category):
            raise ValueError("invalid GPT barkoba secret")
        candidate = GptSecret(secret=secret, category=category)
        last_secret = candidate
        if _secret_key(secret) not in recent_keys:
            return candidate
        logger.info("barkoba secret rejected because repeated attempt=%d", attempt + 1)
    if last_secret is None:
        raise ValueError("invalid GPT barkoba secret")
    return last_secret


def handle_gpt_barkoba_turn(
    *,
    secret: str,
    category: str,
    fact_sheet: dict[str, Any] | None,
    turn_count: int,
    profile_display_name: str,
    message: str,
) -> GptBarkobaTurn:
    fact_json = json.dumps(fact_sheet or {}, ensure_ascii=False, sort_keys=True)
    prompt = (
        "You are judging a Hungarian child-safe barkoba guessing game.\n"
        f"Secret: {secret}\n"
        f"Category: {category}\n"
        f"Fact sheet JSON: {fact_json}\n"
        f"Current turn count: {turn_count}\n"
        f"Child-safe display name: {profile_display_name}\n"
        f"Child message: {message}\n\n"
        "Use the provided fact sheet as the source of truth. Do not contradict it. "
        "If the question maps to a fact in the fact sheet, answer according to that fact. "
        "Decide whether this is a yes/no property/category question, a direct guess, correct, wrong, or unclear. "
        "Return strict JSON only with exactly these keys: "
        '{"reply_text":"...","game_over":false,"active_mode":"barkoba","turn_count_increment":1,"reason":"yes_no"}.\n'
        "Valid active_mode values: barkoba, kids_chat. Valid turn_count_increment values: 0 or 1.\n"
        "For normal yes/no property or category questions, reply_text must be exactly Igen. or exactly Nem. "
        "Use reason=yes_no, game_over=false, active_mode=barkoba. Do not add encouragement, hints, explanations, "
        "or meta comments like jo kerdes.\n"
        "For correct guesses, game_over=true and active_mode=kids_chat. "
        "For wrong guesses, reply_text must be exactly: Nem, nem erre gondoltam. Kerdezz tovabb! "
        "For unclear messages, do not increment the turn and use the standard unclear reminder.\n"
        "These are yes/no property/category questions, not guesses: "
        "Piros ez a gyumolcs?; Kisebb mint a tenyerem?; Nagyobb mint egy kutya?; Gyumolcs?; "
        "Ez egy allat?; Tud repulni?; Vizben el?; Edes?; Gurul?; Jarmu?. "
        "If the message is a category word like Gyumolcs? or Allat?, answer whether the secret belongs to that category.\n"
        "These are direct guesses: Alma?; Elefant?; Szerintem alma.; Ez egy banan?; A megfejtes macska?."
    )
    try:
        payload = _parse_json(_call_openai_text(prompt))
        result = _validate_turn_payload(payload)
        logger.info("barkoba GPT JSON parse success reason=%s", result.reason)
        return result
    except GptBarkobaModelConfigError:
        raise
    except Exception as exc:
        logger.warning("barkoba GPT JSON parse failure: %s", exc)
        return GptBarkobaTurn(
            reply_text=SAFE_FALLBACK_REPLY,
            game_over=False,
            active_mode="barkoba",
            turn_count_increment=0,
            reason="invalid_gpt_json",
            parse_ok=False,
        )


def _call_openai_text(prompt: str) -> str:
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is not set")
    model = openai_chat_model()
    logger.info("OpenAI chat model=%s", model)
    client = OpenAI(api_key=api_key, timeout=25)
    try:
        response = client.responses.create(
            model=model,
            input=prompt,
        )
    except Exception as exc:
        if _is_model_not_found(exc):
            logger.error("Configured OPENAI_CHAT_MODEL is invalid or unavailable model=%s", model)
            raise GptBarkobaModelConfigError(model) from exc
        raise
    return (getattr(response, "output_text", "") or "").strip()


def _parse_json(text: str) -> dict[str, object]:
    return json.loads(text)


def _is_model_not_found(exc: Exception) -> bool:
    code = getattr(exc, "code", None)
    if code == "model_not_found":
        return True
    body = getattr(exc, "body", None)
    if isinstance(body, dict):
        error = body.get("error")
        if isinstance(error, dict) and error.get("code") == "model_not_found":
            return True
    message = str(exc).casefold()
    return "model_not_found" in message or "requested model" in message and "does not exist" in message


def _valid_secret(secret: str, category: str) -> bool:
    if not secret or len(secret) > 40:
        return False
    if category not in ALLOWED_CATEGORIES:
        return False
    safe, _reason = check_message_safe(secret)
    return safe


def _secret_key(secret: str) -> str:
    return normalize_hungarian(secret).replace(" ", "_")[:64] or "secret"


def _validate_turn_payload(payload: dict[str, object]) -> GptBarkobaTurn:
    reply_text = str(payload.get("reply_text", "")).strip()
    if not reply_text or len(reply_text) > 220:
        raise ValueError("invalid reply_text")

    active_mode = str(payload.get("active_mode", "")).strip()
    if active_mode not in ("barkoba", "kids_chat"):
        raise ValueError("invalid active_mode")

    increment_raw = payload.get("turn_count_increment", 0)
    try:
        increment = int(increment_raw)
    except (TypeError, ValueError):
        raise ValueError("invalid turn_count_increment") from None
    increment = 1 if increment > 0 else 0

    game_over = bool(payload.get("game_over", False))
    if game_over:
        active_mode = "kids_chat"

    reason = normalize_hungarian(str(payload.get("reason", "unknown")))[:48] or "unknown"
    reason = _normalize_reason(reason)
    reply_text = _sanitize_reply_text(reply_text, reason)
    safe, _reason = check_message_safe(reply_text)
    if not safe:
        raise ValueError("unsafe reply_text")
    return GptBarkobaTurn(
        reply_text=reply_text,
        game_over=game_over,
        active_mode=active_mode,
        turn_count_increment=increment,
        reason=reason,
    )


def _normalize_reason(reason: str) -> str:
    if reason in ("property_question", "category_question", "yes_or_no", "yes_no_question"):
        return "yes_no"
    if reason in ("guess_wrong", "incorrect_guess"):
        return "wrong_guess"
    if reason in ("guess_correct",):
        return "correct_guess"
    return reason


def _sanitize_reply_text(reply_text: str, reason: str) -> str:
    if reason == "yes_no":
        normalized = normalize_hungarian(reply_text)
        if normalized.startswith("igen") or normalized.startswith("yes"):
            return "Igen."
        if normalized.startswith("nem") or normalized.startswith("no"):
            return "Nem."
        raise ValueError("yes_no reply_text must start with yes or no")
    if reason == "wrong_guess":
        return WRONG_GUESS_REPLY
    if reason == "unclear":
        return UNCLEAR_REPLY
    return reply_text
