from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.tts import _tts_instructions_for_model


def test_tts_instructions_enabled_for_gpt_4o_mini_tts(monkeypatch):
    monkeypatch.setenv("OPENAI_TTS_INSTRUCTIONS", "Beszélj kedvesen.")

    assert _tts_instructions_for_model("gpt-4o-mini-tts") == "Beszélj kedvesen."


def test_tts_instructions_skipped_for_legacy_tts_models(monkeypatch):
    monkeypatch.setenv("OPENAI_TTS_INSTRUCTIONS", "Beszélj kedvesen.")

    assert _tts_instructions_for_model("tts-1") is None
    assert _tts_instructions_for_model("tts-1-hd") is None


def test_tts_instructions_skipped_when_empty(monkeypatch):
    monkeypatch.setenv("OPENAI_TTS_INSTRUCTIONS", "")

    assert _tts_instructions_for_model("gpt-4o-mini-tts") is None
