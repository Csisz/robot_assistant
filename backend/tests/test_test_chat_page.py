from __future__ import annotations

import sys
from pathlib import Path

from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.main import app
from app.tts import audio_dir


def test_test_chat_page_served():
    client = TestClient(app)
    response = client.get("/test-chat")

    assert response.status_code == 200
    assert "text/html" in response.headers["content-type"]
    assert "Robot Simulator Chat" in response.text
    assert 'fetch("/api/robot/chat"' in response.text
    assert "Latest robot reply" in response.text
    assert "CHILD" in response.text
    assert "ROBOT" in response.text
    assert "Newest first" in response.text
    assert 'id="activity"' in response.text
    assert 'id="lastActivity"' in response.text
    assert 'id="activityState"' in response.text
    assert 'id="awaitingInputType"' in response.text
    assert 'id="awaitingInputFor"' in response.text
    assert 'id="robotMood"' in response.text
    assert 'id="robotAction"' in response.text
    assert 'id="barkobaReason"' in response.text
    assert 'id="turnCount"' in response.text
    assert 'id="barkobaCategory"' in response.text
    assert 'id="debugSecret"' in response.text
    assert 'id="answerSource"' in response.text
    assert 'id="openaiChatModel"' in response.text
    assert 'id="barkobaEngine"' in response.text
    assert 'id="barkobaSecretProvider"' in response.text
    assert 'id="backendMock"' in response.text
    assert 'id="barkobaState"' in response.text
    assert 'id="recentBarkobaSecrets"' in response.text
    assert 'id="barkobaExcludedSecrets"' in response.text
    assert 'id="sessionMemory"' in response.text
    assert "message-text" in response.text
    assert "white-space: pre-wrap" in response.text
    assert "word-break: break-word" in response.text
    assert ".turn {" in response.text
    assert "overflow: visible" in response.text
    assert "Szabad a gazda." in response.text
    assert "Feladom." in response.text
    assert "Vidra?" in response.text
    assert "Te k\\u00e9rdezz t\\u0151lem valamit" in response.text
    assert "Mit rajzoljak?" in response.text
    assert "Jegyezd meg, hogy a kedvenc sz\\u00ednem a lila" in response.text
    assert "\\u00c1llatos t\\u00e9nyt k\\u00e9rek" in response.text
    assert "Lila" in response.text
    assert "K\\u00e9k" in response.text
    assert "S\\u00e1rga" in response.text


def test_test_chat_page_normalizes_absolute_audio_url_to_path():
    client = TestClient(app)
    response = client.get("/test-chat")

    assert "function normalizeAudioUrl(rawUrl)" in response.text
    assert "return url.pathname + url.search + url.hash;" in response.text
    assert "original audio URL:" in response.text
    assert "browser src:" in response.text
    assert "Download MP3" in response.text


def test_test_chat_conversation_cards_do_not_use_fixed_cropping_height():
    client = TestClient(app)
    response = client.get("/test-chat")

    assert "max-height: 58vh" not in response.text
    assert "overflow: hidden" not in response.text
    assert "height: auto" in response.text


def test_generated_audio_route_serves_mp3_as_audio_mpeg():
    path = audio_dir() / "pytest_audio_probe.mp3"
    path.write_bytes(b"ID3\x04\x00\x00\x00\x00\x00\x00")
    try:
        client = TestClient(app)
        response = client.get("/audio/generated/pytest_audio_probe.mp3")

        assert response.status_code == 200
        assert response.headers["content-type"].startswith("audio/mpeg")
    finally:
        path.unlink(missing_ok=True)
