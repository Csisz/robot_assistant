from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.main import CHAT_ERROR_FALLBACK_REPLY, app

_ACCENT_WORDS = ("örülök", "szívesen", "kérdésedre", "segíthetek", "segítek", "miben")


def _chat(client: TestClient, message: str, **extra: object) -> dict:
    body = {
        "device_id": "robot_assistant_esp32s3",
        "locale": "hu-HU",
        "mode": "companion",
        "message": message,
        "max_answer_seconds": 30,
        "tts": False,
        **extra,
    }
    response = client.post("/api/robot/chat", json=body)
    assert response.status_code == 200
    return response.json()


def test_zsoli_greeting_returns_accented_hungarian(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Szia, Zsoli vagyok")
    reply = data["reply_text"]

    assert any(w in reply.casefold() for w in _ACCENT_WORDS), (
        f"Expected accented Hungarian word ({_ACCENT_WORDS}) in reply, got: {reply!r}"
    )


def test_reply_text_preserves_non_ascii(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Szia, Zsoli vagyok")
    reply = data["reply_text"]

    assert any(ord(c) > 127 for c in reply), (
        f"reply_text must contain non-ASCII Hungarian accents, got: {reply!r}"
    )


def test_generic_mock_reply_preserves_accents(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Szia!")
    reply = data["reply_text"]

    assert any(ord(c) > 127 for c in reply), (
        f"Generic mock reply must contain accented characters, got: {reply!r}"
    )


def test_fallback_reply_contains_accents():
    assert any(ord(c) > 127 for c in CHAT_ERROR_FALLBACK_REPLY), (
        f"CHAT_ERROR_FALLBACK_REPLY must contain accented characters, got: {CHAT_ERROR_FALLBACK_REPLY!r}"
    )
