from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.conversation_state import _CONVERSATIONS
from app.main import app


def _configure(monkeypatch) -> None:
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_PARENT_PIN", "1234")
    monkeypatch.delenv("OPENAI_API_KEY", raising=False)
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


def test_quiz_start(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Te kerdezz tolem valamit")

    assert response["active_mode"] == "quiz"
    assert response["activity"] == "robot_asks_quiz"
    assert "?" in str(response["reply_text"])
    assert response["activity_state"]["expected_answer"]


def test_gpt_quiz_miau_accepts_cica(monkeypatch):
    _configure(monkeypatch)
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
    monkeypatch.setenv("ROBOT_QUIZ_ENGINE", "gpt")

    class FakeResponses:
        def create(self, **kwargs):
            class Response:
                output_text = (
                    '{"question":"Melyik állat mondja azt, hogy miau?",'
                    '"expected_answers":["macska","cica","cicus"],'
                    '"display_answer":"cica",'
                    '"wrong_options":["kutya","elefánt"],'
                    '"explanation":"A cica mondja azt, hogy miau.",'
                    '"topic":"animals","difficulty":"easy"}'
                )

            return Response()

    class FakeOpenAI:
        def __init__(self, *, api_key: str, timeout: int) -> None:
            self.responses = FakeResponses()

    monkeypatch.setattr("app.activities.OpenAI", FakeOpenAI)
    client = TestClient(app)

    started = _chat(client, "Te kérdezz tőlem valamit")
    assert started["answer_source"] == "quiz_gpt"
    assert "Cica" in started["suggested_replies"]
    assert "Elefánt" not in started["suggested_replies"]
    assert "Nyuszi" not in started["suggested_replies"]

    response = _chat(client, "Cica", str(started["conversation_id"]))

    assert response["safe"] is True
    assert response["answer_source"] == "quiz_eval"
    assert response["activity"] == "robot_asks_quiz"
    assert response["activity_state"]["correct"] is True
    assert "cica" in str(response["reply_text"]).casefold()
    assert "elefánt volt a jó válasz" not in str(response["reply_text"]).casefold()


def test_quiz_answer_normalization_accepts_cica_for_macska(monkeypatch):
    _configure(monkeypatch)
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
    monkeypatch.setenv("ROBOT_QUIZ_ENGINE", "gpt")

    class FakeResponses:
        def create(self, **kwargs):
            class Response:
                output_text = (
                    '{"question":"Melyik állat mondja azt, hogy miau?",'
                    '"expected_answers":["macska"],'
                    '"display_answer":"macska",'
                    '"wrong_options":["kutya"],'
                    '"explanation":"A macska mondja azt, hogy miau.",'
                    '"topic":"animals","difficulty":"easy"}'
                )

            return Response()

    class FakeOpenAI:
        def __init__(self, *, api_key: str, timeout: int) -> None:
            self.responses = FakeResponses()

    monkeypatch.setattr("app.activities.OpenAI", FakeOpenAI)
    client = TestClient(app)

    started = _chat(client, "Te kérdezz tőlem valamit")
    response = _chat(client, "Cica", str(started["conversation_id"]))

    assert response["activity_state"]["correct"] is True
    assert "macska" in str(response["reply_text"]).casefold()


def test_drawing_idea(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Mit rajzoljak?")

    assert response["activity"] == "drawing_idea"
    assert "Rajzolj" in str(response["reply_text"])
    assert response["suggested_replies"]


def test_creative_task(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Adj kreativ feladatot")

    assert response["activity"] == "creative_task"
    assert response["active_mode"] == "creative_task"
    assert response["awaiting_input_type"] == "color"
    assert response["awaiting_input_for"] == "creative_task"
    assert response["suggested_replies"]


def test_creative_task_followup(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    started = _chat(client, "Adj kreativ feladatot")
    response = _chat(client, "Lila", str(started["conversation_id"]))

    assert response["activity"] == "creative_task"
    assert response["answer_source"] == "creative_task_followup"
    assert "lila" in str(response["reply_text"]).casefold()
    assert "nem tudom biztosan" not in str(response["reply_text"]).casefold()
    assert response["active_mode"] == "kids_chat"
    assert response["awaiting_input_for"] is None


def test_animal_fact_accented(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Állatos tényt kérek")

    assert response["activity"] == "animal_fact"
    assert response["robot_mood"] in ("curious", "happy")
    assert "delfin" in str(response["reply_text"]).casefold() or "elefánt" in str(response["reply_text"]).casefold() or "pingvin" in str(response["reply_text"]).casefold()
    assert "természet tele" not in str(response["reply_text"]).casefold()


def test_animal_fact_unaccented(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Allatos tenyt kerek")

    assert response["activity"] == "animal_fact"
    assert response["answer_source"] == "animal_fact_fallback"


def test_generic_continuation_after_animal_fact(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    started = _chat(client, "Mondj még egy állatos tényt")
    response = _chat(client, "Mondj még egyet", str(started["conversation_id"]))

    assert started["activity"] == "animal_fact"
    assert response["activity"] == "animal_fact"
    assert response["answer_source"] in ("animal_fact_gpt", "animal_fact_fallback")
    assert "számolást" not in str(response["reply_text"]).casefold()


def test_generic_continuation_after_drawing(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    started = _chat(client, "Mit rajzoljak?")
    response = _chat(client, "Kérek másikat", str(started["conversation_id"]))

    assert started["activity"] == "drawing_idea"
    assert response["activity"] == "drawing_idea"


def test_generic_continuation_after_joke(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    started = _chat(client, "Mondj egy gyerekviccet")
    response = _chat(client, "Mondj még egyet", str(started["conversation_id"]))

    assert started["activity"] == "joke_or_riddle"
    assert response["activity"] == "joke_or_riddle"


def test_joke(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Mondj egy gyerekviccet")

    assert response["activity"] == "joke_or_riddle"
    assert response["active_mode"] == "kids_chat"


def test_riddle_request_accented(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Mondj egy találós kérdést")

    assert response["activity"] == "joke_or_riddle"
    assert response["active_mode"] == "riddle"
    assert response["answer_source"] in ("riddle_gpt", "riddle_fallback")
    assert "?" in str(response["reply_text"])
    assert "Ezt most még nem tudom" not in str(response["reply_text"])


def test_riddle_request_unaccented(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "talalos kerdest kerek")

    assert response["activity"] == "joke_or_riddle"
    assert response["active_mode"] == "riddle"
    assert "?" in str(response["reply_text"])


def test_riddle_answer_correct(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    started = _chat(client, "Mondj egy találós kérdést")
    response = _chat(client, "Alma", str(started["conversation_id"]))

    assert response["activity"] == "joke_or_riddle"
    assert response["active_mode"] == "kids_chat"
    assert response["answer_source"] == "riddle_eval"
    assert response["activity_state"]["correct"] is True


def test_riddle_answer_synonym(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    started = _chat(client, "Mondj egy találós kérdést")
    state = _CONVERSATIONS[str(started["conversation_id"])]
    state.active_mode = "riddle"
    state.activity_state = {
        "riddle_text": "Puha, nyávog, és szereti, ha megsimogatják. Mi az?",
        "expected_answers": ["macska"],
        "expected_answer": "macska",
        "normalized_expected_answers": ["macska"],
        "display_answer": "macska",
        "topic": "animals",
    }

    response = _chat(client, "Cica", str(started["conversation_id"]))

    assert response["active_mode"] == "kids_chat"
    assert response["answer_source"] == "riddle_eval"
    assert response["activity_state"]["correct"] is True


def test_calm(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Altato mod")

    assert response["activity"] == "calm_sleep_mode"
    assert response["robot_mood"] in ("calm", "sleepy")


def test_daily(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Jo reggelt")

    assert response["activity"] == "daily_routine"
    assert response["robot_mood"] == "happy"
    assert "Jó reggelt" in str(response["reply_text"])
    assert "Választhatsz" in str(response["reply_text"])
    assert "Állatos tényt kérek" in response["suggested_replies"]


def test_session_memory_allowed(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Jegyezd meg, hogy a kedvenc szinem a lila")

    assert response["activity"] == "teach_me_session_memory"
    assert response["session_memory"]["favorite_color"] == "lila"


def test_session_memory_forbidden(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Jegyezd meg a cimem, kerlek")

    assert response["safe"] is False
    assert response["blocked_reason"] == "topic_not_allowed"


def test_parent_settings_unauthorized(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = client.get("/parent/settings")

    assert response.status_code == 403


def test_parent_settings_authorized(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    response = client.get("/parent/settings", headers={"X-Parent-PIN": "1234"})

    assert response.status_code == 200
    payload = response.json()
    assert payload["ok"] is True
    assert "allowed_activities" in payload
    assert "profiles" in payload


def test_activity_routing_priority_for_creative_followup(monkeypatch):
    _configure(monkeypatch)
    client = TestClient(app)

    started = _chat(client, "Adj kreativ feladatot")
    response = _chat(client, "Kék", str(started["conversation_id"]))

    assert response["activity"] == "creative_task"
    assert response["answer_source"] == "creative_task_followup"
    assert "kék" in str(response["reply_text"]).casefold()


def test_animal_fact_gpt_success_even_in_mock_mode(monkeypatch):
    _configure(monkeypatch)
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
    monkeypatch.setenv("ROBOT_ANIMAL_FACT_ENGINE", "gpt")

    class FakeResponses:
        def create(self, **kwargs):
            assert kwargs["model"]
            assert "animal fact" in kwargs["input"]

            class Response:
                output_text = '{"reply_text":"A vidrák játékosan úsznak, és ügyesen használják a mancsaikat."}'

            return Response()

    class FakeOpenAI:
        def __init__(self, *, api_key: str, timeout: int) -> None:
            assert api_key == "test-key"
            self.responses = FakeResponses()

    monkeypatch.setattr("app.activities.OpenAI", FakeOpenAI)
    client = TestClient(app)

    response = _chat(client, "Állatos tényt kérek")

    assert response["activity"] == "animal_fact"
    assert response["answer_source"] == "animal_fact_gpt"
    assert "vidrák" in str(response["reply_text"]).casefold()


def test_animal_fact_gpt_failure_falls_back(monkeypatch):
    _configure(monkeypatch)
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
    monkeypatch.setenv("ROBOT_ANIMAL_FACT_ENGINE", "gpt")

    class FakeOpenAI:
        def __init__(self, *, api_key: str, timeout: int) -> None:
            raise RuntimeError("boom")

    monkeypatch.setattr("app.activities.OpenAI", FakeOpenAI)
    client = TestClient(app)

    response = _chat(client, "Állatos tényt kérek")

    assert response["activity"] == "animal_fact"
    assert response["answer_source"] == "animal_fact_fallback"
    assert response["safe"] is True
