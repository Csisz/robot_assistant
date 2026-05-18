from __future__ import annotations

import io
import sys
import wave
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.main import app


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_wav(duration_ms: int = 200, sample_rate: int = 48000) -> bytes:
    """Return a minimal valid WAV (silence) for testing."""
    num_frames = int(sample_rate * duration_ms / 1000)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(b"\x00\x00" * num_frames)
    return buf.getvalue()


def _voice_post(client: TestClient, wav_bytes: bytes, **extra) -> dict:
    files = {"audio": ("VOICE.WAV", wav_bytes, "audio/wav")}
    data = {
        "device_id": "robot_assistant_esp32s3",
        "locale": "hu-HU",
        "tts": "false",
        **{k: str(v) for k, v in extra.items()},
    }
    resp = client.post("/api/robot/voice-chat", files=files, data=data)
    assert resp.status_code == 200
    return resp.json()


# ---------------------------------------------------------------------------
# Basic response shape
# ---------------------------------------------------------------------------

def test_voice_chat_returns_200(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _voice_post(client, _make_wav())
    assert isinstance(data, dict)


def test_voice_chat_has_transcript_field(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _voice_post(client, _make_wav())
    assert "transcript" in data


def test_voice_chat_has_reply_text(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _voice_post(client, _make_wav())
    assert isinstance(data.get("reply_text"), str) and len(data["reply_text"]) > 0


def test_voice_chat_has_conversation_id(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _voice_post(client, _make_wav())
    assert data.get("conversation_id")


def test_voice_chat_mock_mode_answer_source(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _voice_post(client, _make_wav())
    assert data.get("answer_source") in (
        "mock", "curated_joke", "curated_riddle", "curated_riddle_check"
    )


# ---------------------------------------------------------------------------
# STT mock injection
# ---------------------------------------------------------------------------

def test_voice_stt_routed_as_joke_request(monkeypatch):
    """STT result 'Mondj egy viccet!' → curated_joke."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-unused")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "transcribe_audio", lambda *a, **kw: "Mondj egy viccet!")
    monkeypatch.setattr(main_mod, "openai_enabled", lambda: True)

    data = _voice_post(client, _make_wav())
    assert data["transcript"] == "Mondj egy viccet!"
    assert data["answer_source"] == "curated_joke"


def test_voice_stt_transcript_in_response(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-unused")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "transcribe_audio", lambda *a, **kw: "Miért kék az ég?")
    monkeypatch.setattr(main_mod, "openai_enabled", lambda: True)
    monkeypatch.setattr(main_mod, "generate_openai_reply",
                        lambda *a, **kw: "Az ég azért kék, mert a levegő szórja a fényt.")

    data = _voice_post(client, _make_wav())
    assert data["transcript"] == "Miért kék az ég?"
    assert data["answer_source"] == "openai"
    assert "kék" in data["reply_text"].casefold()


def test_voice_stt_routed_as_riddle_request(monkeypatch):
    """STT result 'Mondj egy találós kérdést!' → curated_riddle."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-unused")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "transcribe_audio",
                        lambda *a, **kw: "Mondj egy találós kérdést!")
    monkeypatch.setattr(main_mod, "openai_enabled", lambda: True)

    data = _voice_post(client, _make_wav())
    assert data["answer_source"] == "curated_riddle"
    assert "?" in data["reply_text"]


def test_voice_stt_riddle_answer_check(monkeypatch):
    """Voice answer to a pending riddle → curated_riddle_check."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-unused")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "get_random_riddle", lambda *a, **kw: {
        "id": "riddle_001",
        "tts_text": "Háló nélkül halászik. Mi az?",
        "answer": "gólya",
        "accepted_answers": ["gólya"],
        "answer_tts_text": "A megfejtés: gólya.",
    })
    monkeypatch.setattr(main_mod, "openai_enabled", lambda: True)

    # Ask for riddle via text to get conversation_id
    from fastapi.testclient import TestClient as TC
    text_client = TC(app)
    r1 = text_client.post("/api/robot/chat", json={
        "device_id": "robot_assistant_esp32s3",
        "locale": "hu-HU",
        "mode": "companion",
        "message": "Mondj egy találós kérdést!",
        "max_answer_seconds": 30,
        "tts": False,
    })
    assert r1.status_code == 200
    conv_id = r1.json()["conversation_id"]

    # Answer via voice
    monkeypatch.setattr(main_mod, "transcribe_audio", lambda *a, **kw: "gólya")
    data = _voice_post(client, _make_wav(), conversation_id=conv_id)
    assert data["answer_source"] == "curated_riddle_check"
    assert "ügyes" in data["reply_text"].casefold() or "pontosan" in data["reply_text"].casefold()


# ---------------------------------------------------------------------------
# Empty audio guard
# ---------------------------------------------------------------------------

def test_voice_empty_audio_returns_error_response(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    files = {"audio": ("VOICE.WAV", b"", "audio/wav")}
    data = {"device_id": "robot_assistant_esp32s3", "locale": "hu-HU", "tts": "false"}
    resp = client.post("/api/robot/voice-chat", files=files, data=data)
    assert resp.status_code == 200
    body = resp.json()
    assert body.get("answer_source") == "voice_error" or body.get("ok") is False


# ---------------------------------------------------------------------------
# Conversation continuity via voice
# ---------------------------------------------------------------------------

def test_voice_preserves_conversation_id(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-unused")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "transcribe_audio", lambda *a, **kw: "Szia!")
    monkeypatch.setattr(main_mod, "openai_enabled", lambda: True)
    monkeypatch.setattr(main_mod, "generate_openai_reply", lambda *a, **kw: "Szia! Hogy vagy?")

    r1 = _voice_post(client, _make_wav())
    conv_id = r1["conversation_id"]
    assert conv_id

    r2 = _voice_post(client, _make_wav(), conversation_id=conv_id)
    assert r2["conversation_id"] == conv_id
