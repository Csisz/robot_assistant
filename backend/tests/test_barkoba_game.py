from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.barkoba import get_secret
from app.conversation_state import _CONVERSATIONS
from app.main import app


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


def _secret_key(conversation_id: str) -> str:
    state = _CONVERSATIONS[conversation_id]
    assert state.barkoba is not None
    return state.barkoba.secret_key


def test_barkoba_randomization_not_all_elephant(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_BARKOBA_ENGINE", "deterministic")
    client = TestClient(app)

    keys = []
    for _ in range(10):
        response = _chat(client, "Játsszunk barkóbát")
        keys.append(_secret_key(str(response["conversation_id"])))

    assert any(key != "elefant" for key in keys)


def test_barkoba_change_secret_avoids_immediate_repeat(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_BARKOBA_ENGINE", "deterministic")
    client = TestClient(app)

    started = _chat(client, "Játsszunk barkóbát")
    conversation_id = str(started["conversation_id"])
    first_secret = _secret_key(conversation_id)

    secret = get_secret(first_secret)
    finished = _chat(client, f"{secret.name}?", conversation_id)
    assert finished["active_mode"] == "kids_chat"

    restarted = _chat(client, "Tudsz másra gondolni mint az elefántra?", conversation_id)
    second_secret = _secret_key(conversation_id)

    assert restarted["active_mode"] == "barkoba"
    assert second_secret != first_secret


def test_active_barkoba_property_question_stays_in_game(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_BARKOBA_ENGINE", "deterministic")
    client = TestClient(app)

    started = _chat(client, "Játsszunk barkóbát")
    response = _chat(client, "Nagyobb mint egy kutya?", str(started["conversation_id"]))

    assert response["active_mode"] == "barkoba"
    assert response["reply_text"] in ("Igen.", "Nem.")
    assert "kutyák" not in str(response["reply_text"]).casefold()


def test_correct_guess_ends_barkoba(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_BARKOBA_ENGINE", "deterministic")
    client = TestClient(app)

    started = _chat(client, "Játsszunk barkóbát")
    conversation_id = str(started["conversation_id"])
    secret = get_secret(_secret_key(conversation_id))

    response = _chat(client, f"{secret.name}?", conversation_id)

    assert response["active_mode"] == "kids_chat"
    assert "ügyes vagy" in str(response["reply_text"]).casefold()


def test_wrong_guess_does_not_return_generic_fact(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_BARKOBA_ENGINE", "deterministic")
    client = TestClient(app)

    started = _chat(client, "Játsszunk barkóbát")
    conversation_id = str(started["conversation_id"])
    current = _secret_key(conversation_id)
    wrong_secret = "kutya" if current != "kutya" else "macska"
    response = _chat(client, f"{get_secret(wrong_secret).name}?", conversation_id)

    assert response["active_mode"] == "barkoba"
    assert response["reply_text"] == "Nem, nem erre gondoltam. Kérdezz tovább!"
