from __future__ import annotations

import sys
import json
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.conversation_state import _CONVERSATIONS
from app.main import app


def _configure_gpt_barkoba(monkeypatch) -> None:
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_BARKOBA_ENGINE", "gpt")
    monkeypatch.setenv("ROBOT_BARKOBA_SECRET_PROVIDER", "gpt")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
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


def _chat_debug(client: TestClient, message: str, conversation_id: str | None = None) -> dict[str, object]:
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


def _mock_openai(monkeypatch, responses: list[str]) -> None:
    queue = list(responses)

    def fake_call_openai_text(prompt: str) -> str:
        assert prompt
        if not queue:
            raise AssertionError("unexpected OpenAI call")
        return queue.pop(0)

    monkeypatch.setattr("app.barkoba_gpt._call_openai_text", fake_call_openai_text)


def _start_with_secret(monkeypatch, client: TestClient, secret: str, category: str = "animals") -> str:
    _mock_openai(monkeypatch, [json.dumps({"secret": secret, "category": category})])
    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    return str(started["conversation_id"])


def _start_with_responses(monkeypatch, client: TestClient, responses: list[dict[str, str]]) -> dict[str, object]:
    _mock_openai(monkeypatch, [json.dumps(response) for response in responses])
    return _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")


def test_gpt_barkoba_start_creates_active_game(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(monkeypatch, ['{"secret":"elef\\u00e1nt","category":"animals"}'])
    client = TestClient(app)

    response = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")

    assert response["active_mode"] == "barkoba"
    assert response["barkoba_reason"] == "start_game"
    assert response["turn_count"] == 0
    assert response["conversation_id"]


def test_gpt_barkoba_property_question_returns_yes_or_no(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"elef\\u00e1nt","category":"animals"}',
            '{"reply_text":"Igen.","game_over":false,"active_mode":"barkoba","turn_count_increment":1,"reason":"yes_no"}',
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Szereti a naps\u00fct\u00e9st?", str(started["conversation_id"]))

    assert response["reply_text"] == "Igen."
    assert response["answer_source"] == "barkoba_gpt"
    assert response["active_mode"] == "barkoba"
    assert response["barkoba_reason"] == "yes_no"
    assert response["turn_count"] == 1


def test_gpt_barkoba_wrong_guess_stays_in_game(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"elef\\u00e1nt","category":"animals"}',
            '{"reply_text":"Nem, nem erre gondoltam. K\\u00e9rdezz tov\\u00e1bb!","game_over":false,"active_mode":"barkoba","turn_count_increment":1,"reason":"wrong_guess"}',
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Macska?", str(started["conversation_id"]))

    assert response["active_mode"] == "barkoba"
    assert response["barkoba_reason"] == "wrong_guess"
    assert "nem erre" in str(response["reply_text"]).casefold()


def test_gpt_barkoba_correct_guess_ends_game(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"elef\\u00e1nt","category":"animals"}',
            '{"reply_text":"Igen, \\u00fcgyes vagy! Elef\\u00e1ntra gondoltam.","game_over":true,"active_mode":"kids_chat","turn_count_increment":1,"reason":"correct_guess"}',
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Elef\u00e1nt?", str(started["conversation_id"]))

    assert response["active_mode"] == "kids_chat"
    assert response["barkoba_reason"] == "correct_guess"
    assert "\u00fcgyes" in str(response["reply_text"]).casefold()


def test_gpt_barkoba_invalid_json_uses_safe_fallback(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"elef\\u00e1nt","category":"animals"}',
            "not-json",
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Szereti a naps\u00fct\u00e9st?", str(started["conversation_id"]))

    assert response["active_mode"] == "barkoba"
    assert response["barkoba_reason"] == "invalid_gpt_json"
    assert response["turn_count"] == 0
    assert "\u00f6sszezavarodtam" in str(response["reply_text"]).casefold()


def test_unsafe_barkoba_question_is_blocked_before_gpt(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    calls = ["secret"]

    def fake_call_openai_text(prompt: str) -> str:
        if calls:
            calls.pop()
            return '{"secret":"elef\\u00e1nt","category":"animals"}'
        raise AssertionError("unsafe message should not be sent to GPT")

    monkeypatch.setattr("app.barkoba_gpt._call_openai_text", fake_call_openai_text)
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Hol lakom?", str(started["conversation_id"]))

    assert response["safe"] is False
    assert response["blocked_reason"] == "topic_not_allowed"
    assert response["active_mode"] == "barkoba"


def test_invalid_openai_chat_model_does_not_fall_back_to_deterministic(monkeypatch, caplog):
    _configure_gpt_barkoba(monkeypatch)
    monkeypatch.setenv("OPENAI_CHAT_MODEL", "not-a-real-model")

    class FakeModelError(Exception):
        code = "model_not_found"

    class FakeResponses:
        def create(self, *, model: str, input: str) -> object:
            assert model == "not-a-real-model"
            assert input
            raise FakeModelError("The requested model does not exist.")

    class FakeOpenAI:
        def __init__(self, *, api_key: str, timeout: int) -> None:
            assert api_key == "test-key"
            assert timeout == 25
            self.responses = FakeResponses()

    monkeypatch.setattr("app.barkoba_gpt.OpenAI", FakeOpenAI)
    client = TestClient(app)

    response = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")

    assert response["safe"] is True
    assert response["active_mode"] == "kids_chat"
    assert response["answer_source"] == "barkoba_gpt_error"
    assert response["barkoba_reason"] == "model_config_error"
    assert "modell be\u00e1ll\u00edt\u00e1sa hib\u00e1s" in str(response["reply_text"])
    assert "Configured OPENAI_CHAT_MODEL is invalid or unavailable" in caplog.text


def test_gpt_yes_no_reply_with_extra_text_is_normalized_to_igen(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"alma","category":"fruits"}',
            '{"reply_text":"Igen, j\\u00f3 k\\u00e9rd\\u00e9s!","game_over":false,"active_mode":"barkoba","turn_count_increment":1,"reason":"yes_no"}',
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Szereti a naps\u00fct\u00e9st?", str(started["conversation_id"]))

    assert response["reply_text"] == "Igen."
    assert response["barkoba_reason"] == "yes_no"


def test_gpt_yes_no_reply_with_extra_text_is_normalized_to_nem(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"ban\\u00e1n","category":"fruits"}',
            '{"reply_text":"Nem, ez egy j\\u00f3 k\\u00e9rd\\u00e9s.","game_over":false,"active_mode":"barkoba","turn_count_increment":1,"reason":"yes_no"}',
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Szereti a naps\u00fct\u00e9st?", str(started["conversation_id"]))

    assert response["reply_text"] == "Nem."
    assert response["barkoba_reason"] == "yes_no"


def test_gpt_category_question_returns_plain_yes_no(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"alma","category":"fruits"}',
            '{"reply_text":"Igen.","game_over":false,"active_mode":"barkoba","turn_count_increment":1,"reason":"yes_no"}',
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Szereti a naps\u00fct\u00e9st?", str(started["conversation_id"]))

    assert response["reply_text"] in ("Igen.", "Nem.")
    assert response["reply_text"] == "Igen."
    assert response["barkoba_reason"] == "yes_no"


def test_gpt_property_question_returns_plain_yes_no(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"alma","category":"fruits"}',
            '{"reply_text":"Igen, igen/nem k\\u00e9rd\\u00e9s.","game_over":false,"active_mode":"barkoba","turn_count_increment":1,"reason":"property_question"}',
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Szereti a naps\u00fct\u00e9st?", str(started["conversation_id"]))

    assert response["reply_text"] == "Igen."
    assert response["barkoba_reason"] == "yes_no"


def test_barkoba_szabad_a_gazda_reveals_secret_without_gpt(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(monkeypatch, ['{"secret":"vidra","category":"animals"}'])
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Szabad a gazda.", str(started["conversation_id"]))

    assert response["active_mode"] == "kids_chat"
    assert response["barkoba_reason"] == "give_up"
    assert response["answer_source"] == "barkoba_give_up"
    assert "vidra" in str(response["reply_text"]).casefold()


def test_barkoba_feladom_reveals_secret_without_gpt(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(monkeypatch, ['{"secret":"vidra","category":"animals"}'])
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Feladom.", str(started["conversation_id"]))

    assert response["active_mode"] == "kids_chat"
    assert response["barkoba_reason"] == "give_up"
    assert "vidra" in str(response["reply_text"]).casefold()


def test_barkoba_direct_wrong_guess_is_handled_before_gpt(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(monkeypatch, ['{"secret":"vidra","category":"animals"}'])
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Macska?", str(started["conversation_id"]))

    assert response["active_mode"] == "barkoba"
    assert response["barkoba_reason"] == "wrong_guess"
    assert response["answer_source"] == "barkoba_direct_guess"
    assert response["reply_text"] == "Nem, nem erre gondoltam. K\u00e9rdezz tov\u00e1bb!"


def test_barkoba_direct_correct_guess_is_handled_before_gpt(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(monkeypatch, ['{"secret":"vidra","category":"animals"}'])
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Vidra?", str(started["conversation_id"]))

    assert response["active_mode"] == "kids_chat"
    assert response["barkoba_reason"] == "correct_guess"
    assert response["answer_source"] == "barkoba_direct_guess"
    assert "Vidra" in str(response["reply_text"])


def test_barkoba_property_question_still_uses_gpt(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(
        monkeypatch,
        [
            '{"secret":"vidra","category":"animals"}',
            '{"reply_text":"Igen.","game_over":false,"active_mode":"barkoba","turn_count_increment":1,"reason":"yes_no"}',
        ],
    )
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    response = _chat(client, "Szerinted szereti a naps\u00fct\u00e9st?", str(started["conversation_id"]))

    assert response["active_mode"] == "barkoba"
    assert response["barkoba_reason"] == "yes_no"
    assert response["answer_source"] == "barkoba_gpt"
    assert response["reply_text"] in ("Igen.", "Nem.")


def test_barkoba_max_turns_reveals_secret_before_processing(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    _mock_openai(monkeypatch, ['{"secret":"vidra","category":"animals"}'])
    client = TestClient(app)

    started = _chat(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    conversation_id = str(started["conversation_id"])
    state = _CONVERSATIONS[conversation_id]
    assert state.barkoba is not None
    state.barkoba.turns = 20

    response = _chat(client, "V\u00edzben \u00e9l?", conversation_id)

    assert response["active_mode"] == "kids_chat"
    assert response["barkoba_reason"] == "max_turns"
    assert response["turn_count"] == 20
    assert "vidra" in str(response["reply_text"]).casefold()


def test_elephant_fact_sheet_answers_are_consistent(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    client = TestClient(app)
    conversation_id = _start_with_secret(monkeypatch, client, "elef\u00e1nt", "animals")

    expected = [
        ("Ez egy \u00e1llat?", "Igen."),
        ("Kisebb mint a tenyerem?", "Nem."),
        ("V\u00edzben \u00e9l?", "Nem."),
        ("Van uszonya?", "Nem."),
        ("Van orm\u00e1nya?", "Igen."),
        ("H\u00fasev\u0151?", "Nem."),
        ("Eml\u0151s?", "Igen."),
    ]
    for message, reply in expected:
        response = _chat(client, message, conversation_id)
        assert response["reply_text"] == reply
        assert response["answer_source"] == "barkoba_fact_sheet"
        assert response["barkoba_reason"] == "yes_no_fact"

    guess = _chat(client, "Elef\u00e1nt?", conversation_id)
    assert guess["active_mode"] == "kids_chat"
    assert guess["barkoba_reason"] == "correct_guess"


def test_fish_fact_sheet_answers_are_consistent(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    client = TestClient(app)
    conversation_id = _start_with_secret(monkeypatch, client, "hal", "animals")

    expected = [
        ("Ez egy \u00e1llat?", "Igen."),
        ("V\u00edzben \u00e9l?", "Igen."),
        ("Van uszonya?", "Igen."),
    ]
    for message, reply in expected:
        response = _chat(client, message, conversation_id)
        assert response["reply_text"] == reply
        assert response["answer_source"] == "barkoba_fact_sheet"

    guess = _chat(client, "Hal?", conversation_id)
    assert guess["active_mode"] == "kids_chat"
    assert guess["barkoba_reason"] == "correct_guess"


def test_apple_fact_sheet_answers_are_consistent(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    client = TestClient(app)
    conversation_id = _start_with_secret(monkeypatch, client, "alma", "fruits")

    expected = [
        ("Ez egy gy\u00fcm\u00f6lcs?", "Igen."),
        ("Piros ez a gy\u00fcm\u00f6lcs?", "Igen."),
        ("Ez egy \u00e1llat?", "Nem."),
    ]
    for message, reply in expected:
        response = _chat(client, message, conversation_id)
        assert response["reply_text"] == reply
        assert response["answer_source"] == "barkoba_fact_sheet"

    guess = _chat(client, "Alma?", conversation_id)
    assert guess["active_mode"] == "kids_chat"
    assert guess["barkoba_reason"] == "correct_guess"


def test_correct_guess_clears_barkoba_state_and_future_guess_is_normal_chat(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    monkeypatch.setenv("ROBOT_DEBUG_BARKOBA_SECRET", "true")
    client = TestClient(app)
    conversation_id = _start_with_secret(monkeypatch, client, "elef\u00e1nt", "animals")

    response = _chat_debug(client, "Elef\u00e1nt?", conversation_id)

    assert response["active_mode"] == "kids_chat"
    assert response["barkoba_reason"] == "correct_guess"
    assert response["debug_secret"] is None
    state = _CONVERSATIONS[conversation_id]
    assert state.barkoba is None

    later = _chat_debug(client, "Elef\u00e1nt", conversation_id)
    assert later["barkoba_reason"] != "correct_guess"
    assert later["answer_source"] != "barkoba_direct_guess"


def test_give_up_clears_barkoba_state(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    monkeypatch.setenv("ROBOT_DEBUG_BARKOBA_SECRET", "true")
    client = TestClient(app)
    conversation_id = _start_with_secret(monkeypatch, client, "elef\u00e1nt", "animals")

    response = _chat_debug(client, "Szabad a gazda.", conversation_id)

    assert response["active_mode"] == "kids_chat"
    assert response["barkoba_reason"] == "give_up"
    assert response["debug_secret"] is None
    assert _CONVERSATIONS[conversation_id].barkoba is None


def test_change_secret_excludes_current_and_mentioned_secret(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    monkeypatch.setenv("ROBOT_DEBUG_BARKOBA_SECRET", "true")
    _mock_openai(
        monkeypatch,
        [
            json.dumps({"secret": "elef\u00e1nt", "category": "animals"}),
            json.dumps({"secret": "elef\u00e1nt", "category": "animals"}),
            json.dumps({"secret": "alma", "category": "fruits"}),
        ],
    )
    client = TestClient(app)

    started = _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    conversation_id = str(started["conversation_id"])
    assert started["debug_secret"] == "elef\u00e1nt"

    response = _chat_debug(client, "Tudsz m\u00e1sra gondolni mint az elef\u00e1ntra?", conversation_id)

    assert response["active_mode"] == "barkoba"
    assert response["barkoba_reason"] == "change_secret"
    assert response["debug_secret"] != "elef\u00e1nt"


def test_new_game_avoids_recent_secret_after_finish(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    monkeypatch.setenv("ROBOT_DEBUG_BARKOBA_SECRET", "true")
    _mock_openai(
        monkeypatch,
        [
            json.dumps({"secret": "elef\u00e1nt", "category": "animals"}),
            json.dumps({"secret": "elef\u00e1nt", "category": "animals"}),
            json.dumps({"secret": "alma", "category": "fruits"}),
        ],
    )
    client = TestClient(app)

    started = _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    conversation_id = str(started["conversation_id"])
    _chat_debug(client, "Elef\u00e1nt?", conversation_id)
    restarted = _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t", conversation_id)

    assert restarted["active_mode"] == "barkoba"
    assert restarted["debug_secret"] != "elef\u00e1nt"


def test_direct_guess_only_runs_in_barkoba_mode(monkeypatch):
    _configure_gpt_barkoba(monkeypatch)
    client = TestClient(app)

    response = _chat(client, "Elef\u00e1nt")

    assert response["active_mode"] == "kids_chat"
    assert response["answer_source"] != "barkoba_direct_guess"
    assert response["barkoba_reason"] is None


class _PreferElephantRandom:
    def choice(self, candidates):
        for secret in candidates:
            if secret.key == "elefant":
                return secret
        return candidates[0]


def _configure_curated_barkoba(monkeypatch) -> None:
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    monkeypatch.setenv("ROBOT_TTS_ENABLED", "false")
    monkeypatch.setenv("ROBOT_BARKOBA_ENGINE", "gpt")
    monkeypatch.setenv("ROBOT_BARKOBA_SECRET_PROVIDER", "curated")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
    monkeypatch.setenv("ROBOT_DEBUG_BARKOBA_SECRET", "true")
    monkeypatch.setattr("app.barkoba._RANDOM", _PreferElephantRandom())
    _CONVERSATIONS.clear()


def test_curated_new_game_does_not_repeat_elephant(monkeypatch):
    _configure_curated_barkoba(monkeypatch)
    client = TestClient(app)

    started = _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    conversation_id = str(started["conversation_id"])
    assert started["debug_secret"] == "elef\u00e1nt"

    finished = _chat_debug(client, "Elef\u00e1nt?", conversation_id)
    assert finished["active_mode"] == "kids_chat"
    assert "elefant" in finished["recent_barkoba_secrets"]

    restarted = _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t", conversation_id)

    assert restarted["active_mode"] == "barkoba"
    assert restarted["debug_secret"] != "elef\u00e1nt"
    assert "elefant" in restarted["barkoba_excluded_secrets"]
    assert restarted["barkoba_secret_provider"] == "curated"


def test_curated_change_secret_excludes_mentioned_elephant(monkeypatch):
    _configure_curated_barkoba(monkeypatch)
    client = TestClient(app)

    started = _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    conversation_id = str(started["conversation_id"])
    assert started["debug_secret"] == "elef\u00e1nt"

    response = _chat_debug(client, "Tudsz m\u00e1sra gondolni mint az elef\u00e1ntra?", conversation_id)

    assert response["active_mode"] == "barkoba"
    assert response["barkoba_reason"] == "change_secret"
    assert response["debug_secret"] != "elef\u00e1nt"
    assert "elefant" in response["barkoba_excluded_secrets"]


def test_curated_provider_does_not_call_openai_for_secret_selection(monkeypatch):
    _configure_curated_barkoba(monkeypatch)

    def fail_openai(prompt: str) -> str:
        raise AssertionError("curated provider must not call OpenAI for secret selection")

    monkeypatch.setattr("app.barkoba_gpt._call_openai_text", fail_openai)
    client = TestClient(app)

    response = _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")

    assert response["active_mode"] == "barkoba"
    assert response["answer_source"] == "barkoba_curated"
    assert response["barkoba_secret_provider"] == "curated"


def test_curated_elephant_fact_sheet_still_answers(monkeypatch):
    _configure_curated_barkoba(monkeypatch)
    client = TestClient(app)

    started = _chat_debug(client, "J\u00e1tsszunk bark\u00f3b\u00e1t")
    conversation_id = str(started["conversation_id"])
    assert started["debug_secret"] == "elef\u00e1nt"

    expected = [
        ("Kisebb mint a tenyerem?", "Nem."),
        ("V\u00edzben \u00e9l?", "Nem."),
        ("Van orm\u00e1nya?", "Igen."),
    ]
    for message, reply in expected:
        response = _chat_debug(client, message, conversation_id)
        assert response["reply_text"] == reply
        assert response["answer_source"] == "barkoba_fact_sheet"
