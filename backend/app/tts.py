from __future__ import annotations

import logging
import os
import re
from datetime import datetime
from pathlib import Path
from typing import Any

from openai import OpenAI

logger = logging.getLogger("robot_backend.tts")

MAX_TTS_CHARS = 1200


def tts_enabled() -> bool:
    raw = os.getenv("ROBOT_TTS_ENABLED")
    if raw is None:
        return True
    return raw.strip().casefold() in ("1", "true", "yes", "on")


def tts_model() -> str:
    return os.getenv("OPENAI_TTS_MODEL", "gpt-4o-mini-tts").strip() or "gpt-4o-mini-tts"


def tts_voice() -> str:
    return os.getenv("OPENAI_TTS_VOICE", "alloy").strip() or "alloy"


def audio_dir() -> Path:
    configured = os.getenv("ROBOT_AUDIO_DIR", "generated_audio").strip() or "generated_audio"
    path = Path(configured)
    if not path.is_absolute():
        path = Path(__file__).resolve().parents[1] / path
    path.mkdir(parents=True, exist_ok=True)
    return path


def generate_tts_audio(text: str, conversation_id: str = "test") -> str | None:
    enabled = tts_enabled()
    logger.info("tts: enabled %s", str(enabled).lower())
    if not enabled:
        logger.info("tts skipped: ROBOT_TTS_ENABLED=false")
        return None

    if not text.strip():
        logger.info("tts skipped: empty text")
        return None

    if os.getenv("ROBOT_TTS_PROVIDER", "openai").strip().casefold() != "openai":
        logger.warning("tts: failed, reason=unsupported provider")
        return None

    if not os.getenv("OPENAI_API_KEY"):
        logger.warning("tts skipped: OPENAI_API_KEY missing")
        return None

    limited_text = _limit_tts_text(text)
    fmt = os.getenv("ROBOT_TTS_FORMAT", "mp3").strip().casefold() or "mp3"
    model = tts_model()
    voice = tts_voice()
    instructions = _tts_instructions_for_model(model)
    filename = _make_filename(conversation_id, fmt)
    output_path = audio_dir() / filename

    logger.info("tts: model=%s", model)
    logger.info("tts: voice=%s", voice)
    logger.info("tts: instructions enabled=%s", str(bool(instructions)).lower())
    logger.info("tts: generating audio, text length=%d", len(limited_text))

    client = OpenAI(api_key=os.getenv("OPENAI_API_KEY"), timeout=35)
    try:
        _write_speech_file(
            client=client,
            output_path=output_path,
            model=model,
            voice=voice,
            text=limited_text,
            fmt=fmt,
            instructions=instructions,
        )
    except Exception as exc:
        if instructions:
            logger.exception("tts: failed, reason=%s; retrying without instructions", exc)
            try:
                _write_speech_file(
                    client=client,
                    output_path=output_path,
                    model=model,
                    voice=voice,
                    text=limited_text,
                    fmt=fmt,
                    instructions=None,
                )
            except Exception as retry_exc:
                logger.exception("tts: failed, reason=%s", retry_exc)
                return None
        else:
            logger.exception("tts: failed, reason=%s", exc)
            return None

    audio_url = f"{_base_url()}/audio/generated/{filename}"
    file_size = output_path.stat().st_size if output_path.exists() else 0
    logger.info("tts: saved file=%s", output_path)
    logger.info("tts: generated audio file size=%d", file_size)
    logger.info("tts: audio url=%s", audio_url)
    return audio_url


def _write_speech_file(
    *,
    client: OpenAI,
    output_path: Path,
    model: str,
    voice: str,
    text: str,
    fmt: str,
    instructions: str | None,
) -> None:
    params: dict[str, Any] = {
        "model": model,
        "voice": voice,
        "input": text,
        "response_format": fmt,
    }
    if instructions:
        params["instructions"] = instructions

    with client.audio.speech.with_streaming_response.create(**params) as response:
        response.stream_to_file(output_path)


def _tts_instructions_for_model(model: str) -> str | None:
    instructions = os.getenv("OPENAI_TTS_INSTRUCTIONS", "").strip()
    if not instructions:
        return None

    normalized = model.strip().casefold()
    if normalized in ("tts-1", "tts-1-hd"):
        logger.info("tts: instructions skipped because model does not support it")
        return None

    if normalized.startswith("gpt-4o-mini-tts"):
        return instructions

    logger.info("tts: instructions skipped because model does not support it")
    return None


def _base_url() -> str:
    return os.getenv("ROBOT_AUDIO_BASE_URL", "http://127.0.0.1:8000").strip().rstrip("/")


def _make_filename(conversation_id: str, fmt: str) -> str:
    safe_id = re.sub(r"[^a-zA-Z0-9_-]+", "_", conversation_id or "test")[:80]
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    safe_fmt = re.sub(r"[^a-z0-9]+", "", fmt) or "mp3"
    return f"response_{safe_id}_{timestamp}.{safe_fmt}"


def _limit_tts_text(text: str) -> str:
    stripped = " ".join(text.strip().split())
    if len(stripped) <= MAX_TTS_CHARS:
        return stripped

    candidate = stripped[:MAX_TTS_CHARS]
    sentence_end = max(candidate.rfind("."), candidate.rfind("!"), candidate.rfind("?"))
    if sentence_end >= 200:
        return candidate[: sentence_end + 1].strip()
    return candidate.rstrip()
