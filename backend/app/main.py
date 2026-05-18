from __future__ import annotations

import logging
import mimetypes
import os
import time
import uuid
from pathlib import Path
from typing import Any

from dotenv import load_dotenv
from fastapi import FastAPI, File, Form, UploadFile
from starlette.responses import FileResponse, JSONResponse
from starlette.staticfiles import StaticFiles

from .curated_content import (
    get_random_joke,
    get_random_riddle,
    is_correct_riddle_answer,
    is_joke_request,
    is_riddle_request,
)
from .kids_safety import BLOCKED_REPLY, check_message_safe, normalize_hungarian
from .openai_client import chat_model, generate_openai_reply, openai_enabled, transcribe_audio
from .schemas import HealthResponse, RobotChatRequest, RobotChatResponse, TtsTestRequest, TtsTestResponse
from .tts import audio_dir, generate_tts_audio, tts_enabled, tts_model, tts_voice

load_dotenv()
mimetypes.add_type("audio/mpeg", ".mp3")

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("robot_backend")
logger.info("OpenAI chat model=%s", chat_model())


class Utf8JSONResponse(JSONResponse):
    media_type = "application/json; charset=utf-8"


app = FastAPI(
    title="Robot Assistant Backend",
    version="0.3.0",
    default_response_class=Utf8JSONResponse,
)
app.mount("/audio/generated", StaticFiles(directory=str(audio_dir())), name="generated_audio")

TEST_CHAT_HTML = Path(__file__).resolve().parent / "static" / "test_chat.html"
_DEBUG_AUDIO_DIR = Path(__file__).resolve().parent.parent / "debug_audio"
CONVERSATIONS: dict[str, list[tuple[str, str]]] = {}
PENDING_RIDDLES: dict[str, dict] = {}
CHAT_ERROR_FALLBACK_REPLY = "Most kicsit lassan gondolkodom. Próbáld meg újra pár másodperc múlva."


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().casefold() in ("1", "true", "yes", "on")


def _default_mode() -> str:
    return os.getenv("ROBOT_DEFAULT_MODE", "kids_chat").strip() or "kids_chat"


def _model_json(model: Any) -> dict[str, Any]:
    if hasattr(model, "model_dump"):
        return model.model_dump()
    return model.dict()


def _json_response(model: Any) -> Utf8JSONResponse:
    return Utf8JSONResponse(content=_model_json(model))


def _elapsed_ms(start: float) -> int:
    return int((time.perf_counter() - start) * 1000)


def _conversation_id(value: str | None) -> str:
    return value.strip() if value and value.strip() else uuid.uuid4().hex


def _should_generate_tts(req: RobotChatRequest) -> bool:
    return req.tts is True


def _generate_reply_audio(req: RobotChatRequest, reply_text: str, conversation_id: str) -> str | None:
    tts_start = time.perf_counter()
    requested = _should_generate_tts(req)
    enabled = tts_enabled()
    api_key_present = bool(os.getenv("OPENAI_API_KEY"))
    logger.info("tts requested=%s enabled=%s api_key=%s model=%s voice=%s",
                str(requested).lower(),
                str(enabled).lower(),
                str(api_key_present).lower(),
                tts_model(),
                tts_voice())
    if not requested or not enabled or not api_key_present or not reply_text:
        logger.info("tts skipped tts_ms=%d", _elapsed_ms(tts_start))
        return None
    audio_url = generate_tts_audio(reply_text, conversation_id)
    logger.info("tts result=%s tts_ms=%d", "ok" if audio_url else "missing", _elapsed_ms(tts_start))
    return audio_url


def _mock_reply(message: str) -> str:
    text = normalize_hungarian(message)
    if "piros" in text and "sarga" in text:
        return "Narancssárgát kapsz! A piros és a sárga együtt meleg, vidám narancs színt ad."
    if "kek az eg" in text:
        return "Az ég azért kék, mert a levegő a Nap fényéből a kék fényt szórja szét legerősebben. Ez jut el sok irányból a szemünkbe."
    if "zsoli" in text:
        return "Szia, Zsoli! Örülök, hogy itt vagy. Miben segíthetek?"
    if "zita" in text:
        return "Szia, Zita! Örülök, hogy itt vagy. Miben segíthetek?"
    if "ida" in text:
        return "Szia, Ida! Örülök, hogy itt vagy. Miben segíthetek?"
    return "Szia! Itt vagyok, és röviden válaszolok. Kérdésedre szívesen segítek."


def _trim_reply(text: str) -> str:
    max_sent = max(1, int(os.getenv("ROBOT_MAX_REPLY_SENTENCES", "5")))
    cleaned = " ".join((text or "").strip().split())
    if not cleaned:
        return CHAT_ERROR_FALLBACK_REPLY
    sentences: list[str] = []
    current = []
    for ch in cleaned:
        current.append(ch)
        if ch in ".!?":
            sentence = "".join(current).strip()
            if sentence:
                sentences.append(sentence)
            current = []
            if len(sentences) >= max_sent:
                break
    if not sentences and current:
        sentences.append("".join(current).strip())
    reply = " ".join(sentences[:max_sent]).strip()
    return reply[:700].strip() or CHAT_ERROR_FALLBACK_REPLY


def handle_robot_chat_request(req: RobotChatRequest) -> RobotChatResponse:
    start = time.perf_counter()
    conversation_id = _conversation_id(req.conversation_id)
    history = CONVERSATIONS.setdefault(conversation_id, [])
    backend_mock = not openai_enabled()
    pending_riddle = PENDING_RIDDLES.get(conversation_id)
    do_trim = True
    is_safe = True

    try:
        # A) Pending riddle answer — evaluated before safety filter so the answer
        #    word itself (e.g. "vér") does not get incorrectly blocked
        if pending_riddle and not is_joke_request(req.message) and not is_riddle_request(req.message):
            accepted = pending_riddle.get("accepted_answers", [])
            answer_display = pending_riddle.get("answer", "")
            if accepted and is_correct_riddle_answer(req.message, accepted):
                reply_text = f"Ügyes vagy, pontosan! A megfejtés: {answer_display}."
            else:
                reply_text = f"Majdnem! A megfejtés: {answer_display}. Kérsz még egy találós kérdést?"
            PENDING_RIDDLES.pop(conversation_id, None)
            answer_source = "curated_riddle_check"
            do_trim = False

        # B) Safety filter
        elif _env_bool("ROBOT_CHILD_SAFE_MODE", True) and not check_message_safe(req.message)[0]:
            reply_text = BLOCKED_REPLY
            answer_source = "safety_block"
            is_safe = False
            do_trim = False

        # C) Joke request
        elif is_joke_request(req.message):
            joke = get_random_joke(conversation_id)
            reply_text = joke.get("tts_text") or joke.get("text", CHAT_ERROR_FALLBACK_REPLY)
            answer_source = "curated_joke"
            do_trim = False

        # D) Riddle request
        elif is_riddle_request(req.message):
            riddle = get_random_riddle(conversation_id)
            reply_text = riddle.get("tts_text", CHAT_ERROR_FALLBACK_REPLY)
            PENDING_RIDDLES[conversation_id] = riddle
            answer_source = "curated_riddle"
            do_trim = False

        # E) OpenAI kids chat
        elif openai_enabled():
            reply_text = generate_openai_reply(req.message, req.locale, recent_messages=history)
            answer_source = "openai"

        # F) Offline mock
        else:
            reply_text = _mock_reply(req.message)
            answer_source = "mock"

    except Exception:
        logger.exception("chat failed")
        reply_text = CHAT_ERROR_FALLBACK_REPLY
        answer_source = "openai_error_fallback"
        do_trim = False

    if do_trim:
        reply_text = _trim_reply(reply_text)
    else:
        reply_text = " ".join(reply_text.strip().split()) or CHAT_ERROR_FALLBACK_REPLY
    history.append((req.message, reply_text))
    del history[:-8]
    reply_audio_url = _generate_reply_audio(req, reply_text, conversation_id)

    logger.info(
        "chat result conversation_id=%s active_mode=%s source=%s safe=%s reply_audio_url=%s chat_ms=%d",
        conversation_id,
        req.mode,
        answer_source,
        is_safe,
        "ok" if reply_audio_url else "missing",
        _elapsed_ms(start),
    )
    return RobotChatResponse(
        ok=True,
        safe=is_safe,
        conversation_id=conversation_id,
        profile_id=req.profile_id,
        reply_text=reply_text,
        reply_audio_url=reply_audio_url,
        active_mode=req.mode,
        answer_source=answer_source,
        openai_chat_model=chat_model(),
        backend_mock=backend_mock,
    )


@app.get("/health", response_model=HealthResponse, response_class=Utf8JSONResponse)
def health() -> Utf8JSONResponse:
    return _json_response(HealthResponse())


@app.get("/test-chat", response_class=FileResponse)
def test_chat() -> FileResponse:
    return FileResponse(TEST_CHAT_HTML, media_type="text/html; charset=utf-8")


@app.post("/api/robot/tts-test", response_model=TtsTestResponse, response_class=Utf8JSONResponse)
def tts_test(req: TtsTestRequest) -> Utf8JSONResponse:
    audio_url = generate_tts_audio(req.text, "tts_test")
    if not audio_url:
        return _json_response(TtsTestResponse(ok=False, audio_url=None, error="tts unavailable or disabled"))
    return _json_response(TtsTestResponse(ok=True, audio_url=audio_url, error=None))


@app.post("/api/robot/chat", response_model=RobotChatResponse, response_class=Utf8JSONResponse)
def robot_chat(req: RobotChatRequest) -> Utf8JSONResponse:
    try:
        return _json_response(handle_robot_chat_request(req))
    except Exception:
        logger.exception("chat route failed")
        conversation_id = _conversation_id(req.conversation_id)
        reply_audio_url = _generate_reply_audio(req, CHAT_ERROR_FALLBACK_REPLY, conversation_id)
        return _json_response(
            RobotChatResponse(
                ok=False,
                safe=True,
                conversation_id=conversation_id,
                profile_id=req.profile_id,
                reply_text=CHAT_ERROR_FALLBACK_REPLY,
                reply_audio_url=reply_audio_url,
                active_mode=req.mode,
                answer_source="route_error_fallback",
                openai_chat_model=chat_model(),
                backend_mock=_env_bool("ROBOT_BACKEND_MOCK", False),
            )
        )


@app.post("/api/robot/voice-chat", response_model=RobotChatResponse, response_class=Utf8JSONResponse)
async def robot_voice_chat(
    audio: UploadFile = File(...),
    device_id: str = Form("robot_assistant_esp32s3"),
    locale: str = Form("hu-HU"),
    conversation_id: str | None = Form(None),
    profile_id: str | None = Form(None),
    tts: bool = Form(True),
) -> Utf8JSONResponse:
    conv_id = _conversation_id(conversation_id)
    transcript = ""
    try:
        audio_bytes = await audio.read()
        if not audio_bytes:
            return _json_response(RobotChatResponse(
                ok=False,
                safe=True,
                conversation_id=conv_id,
                profile_id=profile_id,
                reply_text=CHAT_ERROR_FALLBACK_REPLY,
                active_mode=_default_mode(),
                answer_source="voice_error",
                openai_chat_model=chat_model(),
                backend_mock=not openai_enabled(),
            ))

        try:
            _DEBUG_AUDIO_DIR.mkdir(parents=True, exist_ok=True)
            last_path = _DEBUG_AUDIO_DIR / "last_voice_upload.wav"
            last_path.write_bytes(audio_bytes)
            ts_name = f"voice_{int(time.time())}_{conv_id}.wav"
            (_DEBUG_AUDIO_DIR / ts_name).write_bytes(audio_bytes)
            logger.info("voice upload saved path=%s bytes=%d content_type=%s",
                        last_path, len(audio_bytes), audio.content_type)
        except Exception:
            logger.exception("voice debug save failed")

        if openai_enabled():
            transcript = transcribe_audio(audio_bytes, filename=audio.filename or "voice.wav")
            if transcript:
                logger.info("voice transcript=%r len=%d conv=%s",
                            transcript, len(transcript), conv_id)
            else:
                logger.warning("voice transcript is empty conv=%s", conv_id)
                empty_reply = "Nem hallottalak jól. Megpróbálod még egyszer?"
                reply_audio_url = None
                if tts and tts_enabled() and os.getenv("OPENAI_API_KEY"):
                    try:
                        reply_audio_url = generate_tts_audio(empty_reply, conv_id)
                    except Exception:
                        logger.exception("tts failed for empty transcript fallback")
                empty_resp = RobotChatResponse(
                    ok=True,
                    safe=True,
                    conversation_id=conv_id,
                    profile_id=profile_id,
                    reply_text=empty_reply,
                    reply_audio_url=reply_audio_url,
                    active_mode=_default_mode(),
                    answer_source="empty_transcript_fallback",
                    openai_chat_model=chat_model(),
                    backend_mock=False,
                )
                resp_dict = _model_json(empty_resp)
                resp_dict["transcript"] = ""
                return Utf8JSONResponse(content=resp_dict)
        else:
            transcript = "[mock transkript — hangbemenet nem feldolgozva]"
            logger.info("voice transcript mock conv=%s", conv_id)

        req = RobotChatRequest(
            device_id=device_id,
            locale=locale,
            conversation_id=conversation_id,
            profile_id=profile_id,
            message=transcript or "...",
            tts=tts,
        )
        resp = handle_robot_chat_request(req)
        resp_dict = _model_json(resp)
        resp_dict["transcript"] = transcript
        return Utf8JSONResponse(content=resp_dict)

    except Exception:
        logger.exception("voice-chat route failed")
        return _json_response(RobotChatResponse(
            ok=False,
            safe=True,
            conversation_id=conv_id,
            profile_id=profile_id,
            reply_text=CHAT_ERROR_FALLBACK_REPLY,
            active_mode="companion",
            answer_source="voice_error",
            openai_chat_model=chat_model(),
            backend_mock=not openai_enabled(),
        ))


@app.get("/debug/last-voice.wav")
def debug_last_voice_wav():
    wav_path = _DEBUG_AUDIO_DIR / "last_voice_upload.wav"
    if not wav_path.exists():
        return Utf8JSONResponse(status_code=404, content={"error": "no debug wav available"})
    return FileResponse(str(wav_path), media_type="audio/wav", filename="last_voice_upload.wav")
