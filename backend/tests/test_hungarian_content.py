from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.hungarian_content import (
    HUNGARIAN_JOKES,
    HUNGARIAN_RIDDLES,
    detect_hungarian_joke_request,
    detect_hungarian_riddle_request,
    get_hungarian_joke,
    get_hungarian_riddle,
)
from app.main import app


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


# --- unit tests for detection functions ---

def test_detect_joke_plain():
    assert detect_hungarian_joke_request("Mondj egy viccet!")

def test_detect_joke_accented():
    assert detect_hungarian_joke_request("Mondanál egy viccet?")

def test_detect_joke_mesélj():
    assert detect_hungarian_joke_request("Mesélj egy viccet!")

def test_detect_riddle_talalos():
    assert detect_hungarian_riddle_request("Mondj egy találós kérdést!")

def test_detect_riddle_talaloskerdes():
    assert detect_hungarian_riddle_request("Találóskérdést kérek.")

def test_detect_riddle_rejtveny():
    assert detect_hungarian_riddle_request("Adj egy rejtvényt!")

def test_joke_does_not_match_riddle():
    assert not detect_hungarian_riddle_request("Mondj egy viccet!")

def test_riddle_does_not_match_joke():
    assert not detect_hungarian_joke_request("Mondj egy találós kérdést!")

def test_general_question_not_joke():
    assert not detect_hungarian_joke_request("Miért kék az ég?")

def test_general_question_not_riddle():
    assert not detect_hungarian_riddle_request("Miért kék az ég?")


# --- content quality tests ---

def test_all_jokes_contain_accents():
    for joke in HUNGARIAN_JOKES:
        assert any(ord(c) > 127 for c in joke), f"Joke missing accents: {joke!r}"

def test_all_riddles_contain_accents():
    for riddle in HUNGARIAN_RIDDLES:
        assert any(ord(c) > 127 for c in riddle["question"]), (
            f"Riddle question missing accents: {riddle['question']!r}"
        )

def test_at_least_20_jokes():
    assert len(HUNGARIAN_JOKES) >= 20

def test_at_least_20_riddles():
    assert len(HUNGARIAN_RIDDLES) >= 20

def test_get_hungarian_joke_returns_string():
    j = get_hungarian_joke()
    assert isinstance(j, str) and len(j) > 10

def test_get_hungarian_riddle_returns_dict_with_keys():
    r = get_hungarian_riddle()
    assert "question" in r and "answer" in r


# --- integration tests via /api/robot/chat ---

def test_joke_request_returns_curated_joke(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy viccet!")
    assert data["answer_source"] == "curated_joke"
    reply = data["reply_text"]
    assert len(reply) > 10
    assert any(ord(c) > 127 for c in reply), f"Joke reply missing accents: {reply!r}"


def test_joke_request_accented_variant(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondanál egy viccet?")
    assert data["answer_source"] == "curated_joke"


def test_riddle_request_returns_curated_riddle(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy találós kérdést!")
    assert data["answer_source"] == "curated_riddle"
    reply = data["reply_text"]
    assert len(reply) > 10
    assert any(ord(c) > 127 for c in reply), f"Riddle reply missing accents: {reply!r}"


def test_riddle_reply_does_not_immediately_reveal_answer(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy találós kérdést!")
    reply = data["reply_text"]
    # reply should contain the riddle question, not the full "answer:" format
    assert "?" in reply, f"Riddle reply should contain a question mark: {reply!r}"


def test_riddle_not_generic_szia_reply(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy találós kérdést!")
    reply = data["reply_text"].casefold()
    assert "itt vagyok" not in reply, f"Riddle should not be the generic reply: {reply!r}"


def test_general_question_uses_mock_not_joke(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Miért kék az ég?")
    assert data["answer_source"] == "mock"


def test_joke_deterministic_even_when_openai_key_present(monkeypatch):
    """Curated joke handler fires before OpenAI, even when API key is set."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-not-called")
    client = TestClient(app)

    import app.main as main_mod
    called = []

    def _should_not_be_called(*a, **kw):
        called.append(True)
        return "fallback"

    monkeypatch.setattr(main_mod, "generate_openai_reply", _should_not_be_called)

    data = _chat(client, "Mondj egy viccet!")
    assert data["answer_source"] == "curated_joke"
    assert not called, "generate_openai_reply should not be called for joke requests"
