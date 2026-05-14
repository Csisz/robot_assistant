from __future__ import annotations

import logging
import os

from openai import OpenAI

logger = logging.getLogger("robot_backend.stt")

DEFAULT_STT_MODEL = "gpt-4o-mini-transcribe"


def stt_model() -> str:
    return os.getenv("OPENAI_STT_MODEL", DEFAULT_STT_MODEL).strip() or DEFAULT_STT_MODEL


def max_upload_bytes() -> int:
    raw = os.getenv("ROBOT_VOICE_MAX_UPLOAD_MB", "5").strip() or "5"
    try:
        mb = max(1, min(25, int(raw)))
    except ValueError:
        mb = 5
    return mb * 1024 * 1024


def transcribe_wav(audio: bytes, filename: str = "VOICE.WAV", content_type: str = "audio/wav") -> str:
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is not set")
    if not audio:
        raise ValueError("empty audio")

    model = stt_model()
    logger.info("voice: STT model=%s", model)
    client = OpenAI(api_key=api_key, timeout=45)
    result = client.audio.transcriptions.create(
        model=model,
        file=(filename or "VOICE.WAV", audio, content_type or "audio/wav"),
        language="hu",
    )
    transcript = getattr(result, "text", "") or ""
    return transcript.strip()
