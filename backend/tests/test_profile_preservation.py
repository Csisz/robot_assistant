from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.main import app


def _chat(client: TestClient, message: str, **extra: str) -> dict[str, object]:
    body = {
        "device_id": "robot_assistant_esp32s3",
        "locale": "hu-HU",
        "mode": "kids_chat",
        "message": message,
        "max_answer_seconds": 30,
        **extra,
    }
    response = client.post("/api/robot/chat", json=body)
    assert response.status_code == 200
    return response.json()


def test_invalid_incoming_profile_id_does_not_overwrite_existing_profile(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_STORY_ENGINE", "mock")

    client = TestClient(app)

    first = _chat(client, "Zita vagyok")
    conversation_id = first["conversation_id"]

    assert conversation_id
    assert first["profile_id"] == "zita"

    second = _chat(
        client,
        "Mondj egy unikornisos mesét",
        conversation_id=str(conversation_id),
        profile_id="33072",
    )

    assert second["conversation_id"] == conversation_id
    assert second["profile_id"] == "zita"
    assert "unikornis" in str(second["reply_text"]).casefold()
