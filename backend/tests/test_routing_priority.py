from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.barkoba import BarkobaGame
from app.barkoba_facts import make_fact_sheet
from app.conversation_state import _CONVERSATIONS, get_or_create_conversation
from app.main import app


def _configure(monkeypatch) -> None:
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    _CONVERSATIONS.clear()


def _chat(client: TestClient, message: str, conversation_id: str | None = None) -> dict[str, object]:
    body = {
        "device_id": "robot_assistant_simulator",
        "locale": "hu-HU",
        "mode": "kids_chat",
        "message": message,
        "max_answer_seconds": 30,
        "tts": False,
    }
    if conversation_id:
        body["conversation_id"] = conversation_id
    response = client.post("/api/robot/chat", json=body)
    assert response.status_code == 200
    return response.json()


def _force_barkoba(conversation_id: str, profile_id: str = "zita") -> None:
    state, _created = get_or_create_conversation(conversation_id, profile_id)
    facts, _source = make_fact_sheet("elefánt", "animals")
    state.profile_id = profile_id
    state.active_mode = "barkoba"
    state.barkoba = BarkobaGame("elefant", secret_name="elefánt", category="animals", engine="gpt")
    state.barkoba.fact_sheet = facts


def test_profile_switch_zita_to_ida_is_deterministic(monkeypatch):
    _configure(monkeypatch)

    def fail_openai(*args, **kwargs):
        raise AssertionError("profile switch must not call OpenAI")

    monkeypatch.setattr("app.main.generate_openai_reply", fail_openai)
    client = TestClient(app)

    first = _chat(client, "Zita vagyok")
    response = _chat(client, "Szia Ida vagyok", str(first["conversation_id"]))

    assert response["conversation_id"] == first["conversation_id"]
    assert response["profile_id"] == "ida"
    assert response["active_mode"] == "kids_chat"
    assert response["answer_source"] == "profile"
    assert "Ida" in str(response["reply_text"])


def test_profile_switch_ida_to_zita(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    first = _chat(client, "Ida vagyok")
    response = _chat(client, "Szia Zita vagyok", str(first["conversation_id"]))

    assert response["conversation_id"] == first["conversation_id"]
    assert response["profile_id"] == "zita"
    assert response["active_mode"] == "kids_chat"
    assert response["answer_source"] == "profile"
    assert "Zita" in str(response["reply_text"])


def test_active_barkoba_give_up_reveals_secret(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)
    conversation_id = "routing-give-up-active"
    _force_barkoba(conversation_id)

    response = _chat(client, "Szabad a gazda.", conversation_id)

    assert response["conversation_id"] == conversation_id
    assert response["profile_id"] == "zita"
    assert response["active_mode"] == "kids_chat"
    assert response["barkoba_reason"] == "give_up"
    assert response["answer_source"] == "barkoba_give_up"
    assert "elefánt" in str(response["reply_text"])
    assert _CONVERSATIONS[conversation_id].barkoba is None


def test_inactive_barkoba_give_up_does_not_hit_generic_fallback(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Szabad a gazda.")

    assert response["active_mode"] == "kids_chat"
    assert response["answer_source"] == "barkoba_not_active"
    assert "nem barkóbázunk" in str(response["reply_text"])
    assert "Ezt most még nem tudom" not in str(response["reply_text"])


def test_openai_failure_preserves_conversation_profile_and_mode(monkeypatch):
    _configure(monkeypatch)
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")

    def fail_openai(*args, **kwargs):
        raise RuntimeError("simulated OpenAI outage")

    monkeypatch.setattr("app.main.generate_openai_reply", fail_openai)
    client = TestClient(app)

    first = _chat(client, "Zita vagyok")
    response = _chat(client, "Mi az a csillámpor?", str(first["conversation_id"]))

    assert response["conversation_id"] == first["conversation_id"]
    assert response["profile_id"] == "zita"
    assert response["active_mode"] == "kids_chat"
    assert response["answer_source"] == "openai_error_fallback"
    assert response["reply_text"] == "Most kicsit lassan gondolkodom. Próbáljuk meg még egyszer."
