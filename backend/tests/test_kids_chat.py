from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.main import app


def _chat(client: TestClient, message: str, **extra) -> dict:
    body = {
        "device_id": "robot_assistant_esp32s3",
        "locale": "hu-HU",
        "message": message,
        "max_answer_seconds": 30,
        "tts": False,
        **extra,
    }
    resp = client.post("/api/robot/chat", json=body)
    assert resp.status_code == 200
    return resp.json()


# ---------------------------------------------------------------------------
# Default mode
# ---------------------------------------------------------------------------

def test_default_active_mode_is_kids_chat(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Szia!")
    assert data.get("active_mode") == "kids_chat"


def test_explicit_mode_is_preserved(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Szia!", mode="companion")
    assert data.get("active_mode") == "companion"


# ---------------------------------------------------------------------------
# Greeting
# ---------------------------------------------------------------------------

def test_greeting_returns_nonempty_reply(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Szia, Zita vagyok")
    assert isinstance(data["reply_text"], str) and len(data["reply_text"]) > 5


def test_greeting_reply_has_hungarian_accents(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Szia, Zita vagyok")
    reply = data["reply_text"]
    assert any(ord(c) > 127 for c in reply), f"Reply missing Hungarian accents: {reply!r}"


# ---------------------------------------------------------------------------
# Joke and riddle routing
# ---------------------------------------------------------------------------

def test_joke_request_returns_curated_source(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Mondj egy viccet")
    assert data["answer_source"] == "curated_joke"


def test_joke_reply_has_accents(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Mondj egy viccet!")
    reply = data["reply_text"]
    assert any(ord(c) > 127 for c in reply), f"Joke reply missing accents: {reply!r}"


def test_riddle_request_returns_curated_source(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Mondj egy találós kérdést")
    assert data["answer_source"] == "curated_riddle"
    assert "?" in data["reply_text"], "Riddle reply should contain a question"


# ---------------------------------------------------------------------------
# Safety filter
# ---------------------------------------------------------------------------

def test_unsafe_topic_horror_is_blocked(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_CHILD_SAFE_MODE", "true")
    client = TestClient(app)
    data = _chat(client, "Szeretek horror filmet nézni")
    assert data.get("safe") is False
    assert data.get("answer_source") == "safety_block"


def test_unsafe_topic_reply_redirects_gently(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_CHILD_SAFE_MODE", "true")
    client = TestClient(app)
    data = _chat(client, "Mutass pisztoly képet")
    assert data.get("safe") is False
    reply = data["reply_text"]
    assert "inkább" in reply or "gyerek" in reply.casefold() or "állatokról" in reply


def test_safety_filter_disabled_does_not_block(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_CHILD_SAFE_MODE", "false")
    client = TestClient(app)
    data = _chat(client, "horror film")
    assert data.get("safe") is not False
    assert data.get("answer_source") != "safety_block"


def test_innocent_question_not_blocked(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_CHILD_SAFE_MODE", "true")
    client = TestClient(app)
    data = _chat(client, "Miért kék az ég?")
    assert data.get("safe") is not False
    assert data.get("answer_source") != "safety_block"


def test_riddle_answer_not_blocked_by_safety(monkeypatch):
    """Pending riddle answers bypass the safety filter so answer words are not blocked."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_CHILD_SAFE_MODE", "true")
    client = TestClient(app)

    # Set up a pending riddle
    r1 = _chat(client, "Mondj egy találós kérdést!")
    conv_id = r1["conversation_id"]
    assert r1["answer_source"] == "curated_riddle"

    # Answer with a word that might otherwise trigger safety
    r2 = _chat(client, "vér", conversation_id=conv_id)
    assert r2["answer_source"] == "curated_riddle_check"
    assert r2.get("safe") is not False


# ---------------------------------------------------------------------------
# Simple science / OpenAI routing
# ---------------------------------------------------------------------------

def test_science_question_in_mock_mode(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Miért kék az ég?")
    assert isinstance(data["reply_text"], str) and len(data["reply_text"]) > 10
    assert data["answer_source"] == "mock"


def test_no_mock_when_openai_enabled(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-unused")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "generate_openai_reply",
                        lambda *a, **kw: "Az ég azért kék, mert a levegő szétszórja a fényt.")
    monkeypatch.setattr(main_mod, "openai_enabled", lambda: True)

    data = _chat(client, "Miért kék az ég?")
    assert data["answer_source"] == "openai"
    assert "kék" in data["reply_text"].casefold()


# ---------------------------------------------------------------------------
# Reply length
# ---------------------------------------------------------------------------

def test_mock_reply_not_excessively_long(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)
    data = _chat(client, "Miért kék az ég?")
    reply = data["reply_text"]
    sentence_count = sum(1 for c in reply if c in ".!?")
    assert sentence_count <= 5, f"Reply has too many sentences ({sentence_count}): {reply!r}"
