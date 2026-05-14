from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.conversation_state import _CONVERSATIONS
from app.main import app


def _configure_story_gpt(monkeypatch) -> None:
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_STORY_ENGINE", "gpt")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
    _CONVERSATIONS.clear()


def _chat(client: TestClient, message: str, *, conversation_id: str | None = None, profile_id: str | None = None, tts: bool = False) -> dict[str, object]:
    body: dict[str, object] = {
        "device_id": "robot_assistant_simulator",
        "locale": "hu-HU",
        "mode": "kids_chat",
        "message": message,
        "max_answer_seconds": 30,
        "tts": tts,
    }
    if conversation_id:
        body["conversation_id"] = conversation_id
    if profile_id:
        body["profile_id"] = profile_id
    response = client.post("/api/robot/chat", json=body)
    assert response.status_code == 200
    return response.json()


def _mock_story_openai(monkeypatch, response: str, prompts: list[str] | None = None) -> None:
    def fake_call_openai_text(prompt: str) -> str:
        if prompts is not None:
            prompts.append(prompt)
        return response

    monkeypatch.setattr("app.story_engine._call_openai_text", fake_call_openai_text)


def test_valid_story_json_returns_story_text(monkeypatch):
    _configure_story_gpt(monkeypatch)
    _mock_story_openai(
        monkeypatch,
        '{"title":"A kis nyuszi","story_text":"Volt egyszer egy kedves nyuszi. Megosztotta a r\\u00e9p\\u00e1j\\u00e1t a bar\\u00e1taival. Este nyugodtan elaludt.","mood":"gentle","estimated_seconds":40}',
    )
    client = TestClient(app)

    response = _chat(client, "Mondj egy r\u00f6vid mes\u00e9t egy nyuszir\u00f3l")

    assert response["answer_source"] == "story_gpt"
    assert response["robot_mood"] == "story"
    assert "nyuszi" in str(response["reply_text"]).casefold()


def test_too_long_story_is_rejected(monkeypatch):
    _configure_story_gpt(monkeypatch)
    long_story = "Kedves mondat. " * 400
    _mock_story_openai(
        monkeypatch,
        '{"title":"T\\u00fal hossz\\u00fa","story_text":' + repr(long_story).replace("'", '"') + ',"mood":"gentle","estimated_seconds":300}',
    )
    client = TestClient(app)

    response = _chat(client, "Mondj egy mes\u00e9t")

    assert response["answer_source"] == "story_gpt_error"
    assert "nem siker\u00fclt" in str(response["reply_text"])


def test_unsafe_story_is_rejected(monkeypatch):
    _configure_story_gpt(monkeypatch)
    _mock_story_openai(
        monkeypatch,
        '{"title":"Nem j\\u00f3","story_text":"A mese egy fegyver k\\u00f6r\\u00fcl forgott.","mood":"gentle","estimated_seconds":20}',
    )
    client = TestClient(app)

    response = _chat(client, "Mondj egy mes\u00e9t")

    assert response["answer_source"] == "story_gpt_error"
    assert "nem siker\u00fclt" in str(response["reply_text"])


def test_profile_zita_interests_are_in_story_prompt(monkeypatch):
    _configure_story_gpt(monkeypatch)
    prompts: list[str] = []
    _mock_story_openai(
        monkeypatch,
        '{"title":"Unikornis","story_text":"Zita tal\\u00e1lt egy kedves unikornist. Egy\\u00fctt sz\\u00ednes kavicsokat rendeztek.","mood":"gentle","estimated_seconds":35}',
        prompts,
    )
    client = TestClient(app)

    response = _chat(client, "Mondj egy unikornisos mes\u00e9t", profile_id="zita")

    assert response["answer_source"] == "story_gpt"
    assert prompts and "unikornis" in prompts[0]
    assert "Zita" in prompts[0]


def test_story_tts_false_does_not_generate_audio(monkeypatch):
    _configure_story_gpt(monkeypatch)
    calls = []
    _mock_story_openai(
        monkeypatch,
        '{"title":"Nyuszi","story_text":"A nyuszi kedvesen seg\\u00edtett egy bar\\u00e1tj\\u00e1nak.","mood":"gentle","estimated_seconds":20}',
    )

    def fake_generate_tts_audio(text: str, conversation_id: str = "test") -> str:
        calls.append((text, conversation_id))
        return "http://127.0.0.1:8000/audio/generated/story.mp3"

    monkeypatch.setattr("app.main.generate_tts_audio", fake_generate_tts_audio)
    client = TestClient(app)

    response = _chat(client, "Mondj egy mes\u00e9t", tts=False)

    assert response["answer_source"] == "story_gpt"
    assert response["reply_audio_url"] is None
    assert calls == []


def test_story_tts_true_generates_audio_url(monkeypatch):
    _configure_story_gpt(monkeypatch)
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "true")
    _mock_story_openai(
        monkeypatch,
        '{"title":"Nyuszi","story_text":"A nyuszi kedvesen seg\\u00edtett egy bar\\u00e1tj\\u00e1nak.","mood":"gentle","estimated_seconds":20}',
    )

    def fake_generate_tts_audio(text: str, conversation_id: str = "test") -> str:
        assert "nyuszi" in text
        return "http://127.0.0.1:8000/audio/generated/story.mp3"

    monkeypatch.setattr("app.main.generate_tts_audio", fake_generate_tts_audio)
    client = TestClient(app)

    response = _chat(client, "Mondj egy mes\u00e9t", tts=True)

    assert response["answer_source"] == "story_gpt"
    assert response["reply_audio_url"] == "http://127.0.0.1:8000/audio/generated/story.mp3"
