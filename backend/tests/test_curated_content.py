from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.curated_content import (
    get_random_joke,
    get_random_riddle,
    is_correct_riddle_answer,
    is_joke_request,
    is_riddle_request,
    normalize_hu_answer,
)
from app.main import app

# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------

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
    resp = client.post("/api/robot/chat", json=body)
    assert resp.status_code == 200
    return resp.json()


# ---------------------------------------------------------------------------
# normalize_hu_answer
# ---------------------------------------------------------------------------

def test_normalize_strips_accent():
    assert normalize_hu_answer("gólya") == "golya"

def test_normalize_casefoldes():
    assert normalize_hu_answer("GÓLYA") == "golya"

def test_normalize_multiple_accents():
    assert normalize_hu_answer("Örülök") == "orulok"


# ---------------------------------------------------------------------------
# is_correct_riddle_answer
# ---------------------------------------------------------------------------

def test_correct_answer_exact():
    assert is_correct_riddle_answer("gólya", ["gólya"])

def test_correct_answer_unaccented():
    assert is_correct_riddle_answer("golya", ["gólya"])

def test_correct_answer_uppercase():
    assert is_correct_riddle_answer("GÓLYA", ["gólya"])

def test_correct_answer_wrong():
    assert not is_correct_riddle_answer("egér", ["gólya"])

def test_correct_answer_empty_list():
    assert not is_correct_riddle_answer("gólya", [])

def test_correct_answer_whitespace():
    assert is_correct_riddle_answer("  golya  ", ["gólya"])


# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------

def test_is_joke_request_plain():
    assert is_joke_request("Mondj egy viccet!")

def test_is_joke_request_accented():
    assert is_joke_request("Mondanál egy viccet?")

def test_is_joke_request_gyerekvicc():
    assert is_joke_request("Mondj egy gyerekviccet")

def test_is_riddle_request_talalos():
    assert is_riddle_request("Mondj egy találós kérdést!")

def test_is_riddle_request_rejtveny():
    assert is_riddle_request("Adj egy rejtvényt!")

def test_is_riddle_request_fejtoro():
    assert is_riddle_request("fejtörőt kérek")

def test_joke_not_riddle():
    assert not is_riddle_request("Mondj egy viccet!")

def test_riddle_not_joke():
    assert not is_joke_request("Mondj egy találós kérdést!")

def test_general_not_joke():
    assert not is_joke_request("Miért kék az ég?")

def test_general_not_riddle():
    assert not is_riddle_request("Miért kék az ég?")


# ---------------------------------------------------------------------------
# Data / get_random_*
# ---------------------------------------------------------------------------

def test_get_random_joke_returns_dict():
    j = get_random_joke()
    assert isinstance(j, dict)
    assert j.get("tts_text") or j.get("text")

def test_get_random_riddle_has_accepted_answers():
    r = get_random_riddle()
    assert isinstance(r, dict)
    assert r.get("accepted_answers")

def test_get_random_riddle_not_empty_accepted():
    for _ in range(10):
        r = get_random_riddle()
        assert len(r["accepted_answers"]) > 0


# ---------------------------------------------------------------------------
# Integration — joke
# ---------------------------------------------------------------------------

def test_joke_request_answer_source(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy viccet!")
    assert data["answer_source"] == "curated_joke"

def test_joke_reply_contains_accented_text(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy viccet!")
    reply = data["reply_text"]
    assert len(reply) > 10
    assert any(ord(c) > 127 for c in reply), f"Joke missing accents: {reply!r}"

def test_joke_does_not_call_openai(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-unused")
    client = TestClient(app)

    import app.main as main_mod
    called: list[bool] = []

    def _guard(*a, **kw) -> str:
        called.append(True)
        return "should not be called"

    monkeypatch.setattr(main_mod, "generate_openai_reply", _guard)

    data = _chat(client, "Mondj még egy viccet!")
    assert data["answer_source"] == "curated_joke"
    assert not called, "generate_openai_reply was called for a joke request"

def test_mondanal_viccet_variant(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondanál egy viccet?")
    assert data["answer_source"] == "curated_joke"


# ---------------------------------------------------------------------------
# Integration — riddle
# ---------------------------------------------------------------------------

def test_riddle_request_answer_source(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy találós kérdést!")
    assert data["answer_source"] == "curated_riddle"

def test_riddle_reply_contains_question_mark(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy találós kérdést!")
    assert "?" in data["reply_text"], "Riddle reply should contain a question mark"

def test_riddle_reply_accented(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy találós kérdést!")
    reply = data["reply_text"]
    assert any(ord(c) > 127 for c in reply), f"Riddle missing accents: {reply!r}"

def test_riddle_not_generic_reply(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    data = _chat(client, "Mondj egy találós kérdést!")
    assert "itt vagyok" not in data["reply_text"].casefold()


# ---------------------------------------------------------------------------
# Integration — riddle answer check
# ---------------------------------------------------------------------------

def test_riddle_correct_answer_cleared(monkeypatch):
    """Correct answer → curated_riddle_check, positive wording."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "get_random_riddle", lambda *a, **kw: {
        "id": "riddle_001",
        "tts_text": "Háló nélkül halászik, kéményeken tanyázik. Elköltözik, ha fázik. Mi az?",
        "answer": "gólya",
        "accepted_answers": ["gólya"],
        "answer_tts_text": "A megfejtés: gólya.",
    })

    r1 = _chat(client, "Mondj egy találós kérdést!")
    assert r1["answer_source"] == "curated_riddle"
    conv_id = r1["conversation_id"]

    r2 = _chat(client, "gólya", conversation_id=conv_id)
    assert r2["answer_source"] == "curated_riddle_check"
    assert "ügyes" in r2["reply_text"].casefold() or "pontosan" in r2["reply_text"].casefold()

def test_riddle_accent_insensitive_answer(monkeypatch):
    """'golya' (no accent) matches 'gólya' accepted answer."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "get_random_riddle", lambda *a, **kw: {
        "id": "riddle_001",
        "tts_text": "Háló nélkül halászik, kéményeken tanyázik. Elköltözik, ha fázik. Mi az?",
        "answer": "gólya",
        "accepted_answers": ["gólya"],
        "answer_tts_text": "A megfejtés: gólya.",
    })

    r1 = _chat(client, "Mondj egy találós kérdést!")
    conv_id = r1["conversation_id"]

    r2 = _chat(client, "golya", conversation_id=conv_id)
    assert r2["answer_source"] == "curated_riddle_check"
    assert "ügyes" in r2["reply_text"].casefold() or "pontosan" in r2["reply_text"].casefold()

def test_riddle_wrong_answer(monkeypatch):
    """Wrong answer → negative wording, pending cleared."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "get_random_riddle", lambda *a, **kw: {
        "id": "riddle_001",
        "tts_text": "Háló nélkül halászik, kéményeken tanyázik. Elköltözik, ha fázik. Mi az?",
        "answer": "gólya",
        "accepted_answers": ["gólya"],
        "answer_tts_text": "A megfejtés: gólya.",
    })

    r1 = _chat(client, "Mondj egy találós kérdést!")
    conv_id = r1["conversation_id"]

    r2 = _chat(client, "egér", conversation_id=conv_id)
    assert r2["answer_source"] == "curated_riddle_check"
    assert "majdnem" in r2["reply_text"].casefold()

def test_riddle_pending_cleared_after_answer(monkeypatch):
    """After answer (right or wrong), next general message is not riddle_check."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "get_random_riddle", lambda *a, **kw: {
        "id": "riddle_001",
        "tts_text": "Háló nélkül halászik, kéményeken tanyázik. Elköltözik, ha fázik. Mi az?",
        "answer": "gólya",
        "accepted_answers": ["gólya"],
        "answer_tts_text": "A megfejtés: gólya.",
    })

    r1 = _chat(client, "Mondj egy találós kérdést!")
    conv_id = r1["conversation_id"]

    _chat(client, "nem tudom", conversation_id=conv_id)  # clears pending

    r3 = _chat(client, "Szia!", conversation_id=conv_id)
    assert r3["answer_source"] != "curated_riddle_check"

def test_new_riddle_during_pending_replaces_it(monkeypatch):
    """Asking for a new riddle while one is pending should start a fresh riddle."""
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "true")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(main_mod, "get_random_riddle", lambda *a, **kw: {
        "id": "riddle_001",
        "tts_text": "Háló nélkül halászik, kéményeken tanyázik. Elköltözik, ha fázik. Mi az?",
        "answer": "gólya",
        "accepted_answers": ["gólya"],
        "answer_tts_text": "A megfejtés: gólya.",
    })

    r1 = _chat(client, "Mondj egy találós kérdést!")
    conv_id = r1["conversation_id"]

    r2 = _chat(client, "Mondj egy találós kérdést!", conversation_id=conv_id)
    assert r2["answer_source"] == "curated_riddle"


# ---------------------------------------------------------------------------
# Integration — general question still goes to OpenAI when live
# ---------------------------------------------------------------------------

def test_general_question_uses_openai_when_enabled(monkeypatch):
    monkeypatch.setenv("ROBOT_BACKEND_MOCK", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key-mocked")
    client = TestClient(app)

    import app.main as main_mod
    monkeypatch.setattr(
        main_mod,
        "generate_openai_reply",
        lambda *a, **kw: "Az ég kék, mert a levegő szórja a fényt.",
    )

    data = _chat(client, "Miért kék az ég?")
    assert data["answer_source"] == "openai"
    assert "kék" in data["reply_text"].casefold()
