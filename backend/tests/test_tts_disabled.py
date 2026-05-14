from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.main import app


def test_chat_does_not_generate_audio_when_tts_disabled(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_STORY_ENGINE", "mock")

    client = TestClient(app)
    response = client.post(
        "/api/robot/chat",
        json={
            "device_id": "robot_assistant_esp32s3",
            "locale": "hu-HU",
            "mode": "kids_chat",
            "message": "Mondj egy nagyon rövid mesét egy nyusziról.",
            "max_answer_seconds": 30,
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["safe"] is True
    assert data["reply_text"]
    assert data["reply_audio_url"] is None


def test_tts_test_endpoint_reports_disabled(monkeypatch):
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")

    client = TestClient(app)
    response = client.post("/api/robot/tts-test", json={"text": "Szia Zita! Ez egy próba hang."})

    assert response.status_code == 200
    assert response.json() == {
        "ok": False,
        "audio_url": None,
        "error": "tts unavailable or disabled",
    }


def test_chat_request_tts_false_skips_generation(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "true")
    monkeypatch.setenv("ROBOT_STORY_ENGINE", "mock")

    calls = []

    def fake_generate_tts_audio(text: str, conversation_id: str = "test") -> str:
        calls.append((text, conversation_id))
        return "http://127.0.0.1:8000/audio/generated/test.mp3"

    monkeypatch.setattr("app.main.generate_tts_audio", fake_generate_tts_audio)

    client = TestClient(app)
    response = client.post(
        "/api/robot/chat",
        json={
            "device_id": "robot_assistant_simulator",
            "locale": "hu-HU",
            "mode": "kids_chat",
            "message": "Mondj egy nagyon rövid mesét egy nyusziról.",
            "max_answer_seconds": 30,
            "tts": False,
        },
    )

    assert response.status_code == 200
    assert response.json()["reply_audio_url"] is None
    assert calls == []


def test_chat_request_tts_true_uses_generation(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "true")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
    monkeypatch.setenv("ROBOT_STORY_ENGINE", "mock")

    def fake_generate_tts_audio(text: str, conversation_id: str = "test") -> str:
        return "http://127.0.0.1:8000/audio/generated/test.mp3"

    monkeypatch.setattr("app.main.generate_tts_audio", fake_generate_tts_audio)

    client = TestClient(app)
    response = client.post(
        "/api/robot/chat",
        json={
            "device_id": "robot_assistant_simulator",
            "locale": "hu-HU",
            "mode": "kids_chat",
            "message": "Mondj egy nagyon rövid mesét egy nyusziról.",
            "max_answer_seconds": 30,
            "tts": True,
        },
    )

    assert response.status_code == 200
    assert response.json()["reply_audio_url"] == "http://127.0.0.1:8000/audio/generated/test.mp3"


def test_mock_mode_tts_true_generates_when_key_present(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.delenv("ROBOT_TTS_ENABLED", raising=False)
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")

    def fake_generate_tts_audio(text: str, conversation_id: str = "test") -> str:
        assert text
        return "http://127.0.0.1:8000/audio/generated/mock.mp3"

    monkeypatch.setattr("app.main.generate_tts_audio", fake_generate_tts_audio)

    client = TestClient(app)
    response = client.post(
        "/api/robot/chat",
        json={
            "device_id": "robot_assistant_esp32s3",
            "locale": "hu-HU",
            "mode": "kids_chat",
            "message": "Zita vagyok",
            "max_answer_seconds": 30,
            "tts": True,
        },
    )

    assert response.status_code == 200
    data = response.json()
    assert data["backend_mock"] is True
    assert data["reply_text"]
    assert data["reply_audio_url"] == "http://127.0.0.1:8000/audio/generated/mock.mp3"
