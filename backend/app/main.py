from __future__ import annotations

import logging
import mimetypes
import os
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

from dotenv import load_dotenv
from fastapi import FastAPI, File, Form, Header, HTTPException, Query, Request, UploadFile
from starlette.requests import ClientDisconnect
from starlette.responses import FileResponse, JSONResponse
from starlette.staticfiles import StaticFiles

from .activities import handle_active_activity_turn, handle_activity, handle_activity_continuation, handle_activity_followup
from .activity_router import detect_activity
from .barkoba import (
    MAX_BARKOBA_TURNS,
    BarkobaGame,
    answer_barkoba_turn,
    barkoba_correct_guess_reply,
    barkoba_give_up_reply,
    barkoba_wrong_guess_reply,
    choose_secret,
    detect_direct_guess,
    excluded_secret_keys_from_message,
    game_secret_category,
    game_secret_name,
    is_barkoba_change_request,
    is_barkoba_give_up_request,
    log_secret_choice,
    max_turns_reply,
    start_barkoba_reply,
)
from .barkoba_gpt import (
    BARKOBA_MODEL_CONFIG_REPLY,
    GptBarkobaModelConfigError,
    choose_gpt_secret,
    handle_gpt_barkoba_turn,
    openai_chat_model,
    should_use_gpt_engine,
)
from .barkoba_facts import answer_from_fact_sheet, make_fact_sheet, validate_fact_sheet
from .conversation_state import append_turn, clear_barkoba_state, get_or_create_conversation, recent_context, remember_barkoba_secret
from .intent_router import IntentName, detect_intent
from .kids_safety import blocked_response, check_message_safe
from .mock_answers import UNKNOWN_SAFE_REPLY, get_mock_answer
from .openai_client import generate_openai_reply, openai_enabled
from .parent_settings import authorized as parent_authorized, settings_payload, update_settings
from .profiles import detect_profile_claim, get_profile, profile_claim_reply
from .schemas import (
    HealthResponse,
    RobotChatRequest,
    RobotChatResponse,
    TtsTestRequest,
    TtsTestResponse,
    VoiceUploadCancelRequest,
    VoiceUploadFinishRequest,
    VoiceUploadStartRequest,
    VoiceUploadStartResponse,
)
from .session_memory import session_memory_snapshot
from .stt import max_upload_bytes, stt_model, transcribe_wav
from .story_engine import generate_story_reply, story_gpt_enabled
from .tts import audio_dir, generate_tts_audio, tts_enabled, tts_model, tts_voice
from .weather import get_weather_reply, weather_mock_enabled

load_dotenv()
mimetypes.add_type("audio/mpeg", ".mp3")

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("robot_backend")
logger.info("OpenAI chat model=%s", openai_chat_model())


class Utf8JSONResponse(JSONResponse):
    media_type = "application/json; charset=utf-8"


app = FastAPI(
    title="Robot Assistant Backend",
    version="0.2.0",
    default_response_class=Utf8JSONResponse,
)
app.mount("/audio/generated", StaticFiles(directory=str(audio_dir())), name="generated_audio")
TEST_CHAT_HTML = Path(__file__).resolve().parent / "static" / "test_chat.html"
VOICE_UPLOAD_DIR = Path(__file__).resolve().parents[1] / "tmp_voice_uploads"
VOICE_UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
VOICE_UPLOAD_TTL_SECONDS = 20 * 60
VOICE_UPLOADS: dict[str, dict[str, Any]] = {}


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().casefold() in ("1", "true", "yes", "on")


def _model_json(model: Any) -> dict[str, Any]:
    if hasattr(model, "model_dump"):
        return model.model_dump()
    return model.dict()


def _json_response(model: Any) -> Utf8JSONResponse:
    return Utf8JSONResponse(content=_model_json(model))


def _chat_response(
    *,
    safe: bool,
    conversation_id: str,
    profile_id: str,
    reply_text: str,
    active_mode: str,
    reply_audio_url: str | None = None,
    robot_mood: str = "happy",
    robot_action: str = "speak",
    blocked_reason: str | None = None,
    activity: str | None = None,
    last_activity: str | None = None,
    last_activity_topic: str | None = None,
    activity_state: dict[str, Any] | None = None,
    awaiting_input_type: str | None = None,
    awaiting_input_for: str | None = None,
    suggested_replies: list[str] | None = None,
    session_memory: dict[str, str] | None = None,
    barkoba_reason: str | None = None,
    turn_count: int | None = None,
    debug_secret: str | None = None,
    barkoba_category: str | None = None,
    debug_barkoba_facts: dict[str, Any] | None = None,
    recent_barkoba_secrets: list[str] | None = None,
    barkoba_excluded_secrets: list[str] | None = None,
    barkoba_secret_provider: str | None = None,
    answer_source: str | None = None,
    backend_mock: bool | None = None,
) -> RobotChatResponse:
    return RobotChatResponse(
        ok=True,
        safe=safe,
        conversation_id=conversation_id,
        profile_id=profile_id,
        reply_text=reply_text,
        reply_audio_url=reply_audio_url,
        robot_mood=robot_mood,
        robot_action=robot_action,
        blocked_reason=blocked_reason,
        activity=activity,
        last_activity=last_activity,
        last_activity_topic=last_activity_topic,
        activity_state=activity_state,
        awaiting_input_type=awaiting_input_type,
        awaiting_input_for=awaiting_input_for,
        suggested_replies=suggested_replies,
        session_memory=session_memory,
        active_mode=active_mode,  # type: ignore[arg-type]
        barkoba_reason=barkoba_reason,
        turn_count=turn_count,
        debug_secret=debug_secret,
        barkoba_category=barkoba_category,
        debug_barkoba_facts=debug_barkoba_facts,
        recent_barkoba_secrets=recent_barkoba_secrets,
        barkoba_excluded_secrets=barkoba_excluded_secrets,
        barkoba_secret_provider=barkoba_secret_provider or _barkoba_secret_provider(),  # type: ignore[arg-type]
        answer_source=answer_source,
        openai_chat_model=openai_chat_model(),
        barkoba_engine=_barkoba_engine_name(),
        backend_mock=backend_mock,
    )


def _answer_safe_request(
    req: RobotChatRequest,
    intent: IntentName,
    mock_mode: bool,
    profile_id: str,
    conversation_id: str,
) -> tuple[str, str]:
    if intent == "weather":
        return get_weather_reply(req.message)

    if intent == "short_story" and story_gpt_enabled():
        profile = get_profile(profile_id)
        story = generate_story_reply(
            message=req.message,
            locale=req.locale,
            max_answer_seconds=req.max_answer_seconds,
            profile=profile,
        )
        return story.reply_text, story.answer_source

    if not mock_mode and intent == "unknown_safe" and openai_enabled():
        profile = get_profile(profile_id)
        try:
            return (
                generate_openai_reply(
                    req.message,
                    req.locale,
                    req.max_answer_seconds,
                    profile_display_name=profile.display_name,
                    safe_interests=profile.safe_interests,
                    recent_messages=recent_context_by_id(conversation_id),
                ),
                "openai",
            )
        except Exception:
            logger.exception("OpenAI call failed, returning safe error fallback")
            return "Most kicsit lassan gondolkodom. Próbáljuk meg még egyszer.", "openai_error_fallback"

    return get_mock_answer(intent, req.message, profile_id)


def recent_context_by_id(conversation_id: str) -> list[tuple[str, str]]:
    state, _created = get_or_create_conversation(conversation_id, None)
    return recent_context(state)


def _start_barkoba_round(state: Any, *, changed: bool, excluded_secret_keys: list[str] | None = None) -> str:
    turn_engine = _barkoba_engine_name()
    secret_provider = _barkoba_secret_provider()
    current_key = state.barkoba.secret_key if state.barkoba else None
    excluded = _unique_secret_keys(excluded_secret_keys or [])
    if current_key:
        excluded.append(current_key)
        if changed:
            remember_barkoba_secret(state, current_key)
    excluded = _unique_secret_keys(excluded)
    all_excluded = _unique_secret_keys([*state.recent_barkoba_secret_keys, *excluded])
    state.last_barkoba_excluded_keys = all_excluded
    state.last_barkoba_secret_provider = secret_provider
    logger.info(
        "barkoba engine=%s secret_provider=%s conversation_id=%s profile_id=%s",
        turn_engine,
        secret_provider,
        state.conversation_id,
        state.profile_id,
    )

    selected_secret: str
    selected_category: str
    selected_key: str
    fact_sheet: dict[str, Any]
    fact_source: str

    if secret_provider == "gpt":
        try:
            gpt_secret = choose_gpt_secret(state.recent_barkoba_secret_keys, excluded)
        except GptBarkobaModelConfigError:
            raise
        except Exception:
            logger.exception("barkoba GPT secret selection failed, using curated fallback")
            secret = choose_secret(state.recent_barkoba_secret_keys, excluded)
            selected_secret = secret.name
            selected_category = secret.category
            selected_key = secret.key
        else:
            selected_secret = gpt_secret.secret
            selected_category = gpt_secret.category
            selected_key = _normalize_secret_key(selected_secret)
            if selected_key in set(all_excluded):
                logger.info("barkoba secret rejected because repeated key=%s", selected_key)
                secret = choose_secret(state.recent_barkoba_secret_keys, excluded)
                selected_secret = secret.name
                selected_category = secret.category
                selected_key = secret.key
    else:
        secret = choose_secret(state.recent_barkoba_secret_keys, excluded)
        selected_secret = secret.name
        selected_category = secret.category
        selected_key = secret.key

    fact_sheet, fact_source = make_fact_sheet(selected_secret, selected_category)
    if not validate_fact_sheet(fact_sheet):
        logger.info("barkoba fact sheet invalid for key=%s, using curated fallback", selected_key)
        secret = choose_secret(state.recent_barkoba_secret_keys, excluded)
        selected_secret = secret.name
        selected_category = secret.category
        selected_key = secret.key
        fact_sheet, fact_source = make_fact_sheet(selected_secret, selected_category)

    state.barkoba = BarkobaGame(
        selected_key,
        secret_name=selected_secret,
        category=selected_category,
        engine=turn_engine,
    )
    state.barkoba.fact_sheet = fact_sheet
    state.active_mode = "barkoba"
    logger.info(
        "barkoba started conversation_id=%s profile_id=%s secret_provider=%s excluded=%s recent=%s secret_category=%s secret_length=%d changed=%s",
        state.conversation_id,
        state.profile_id,
        secret_provider,
        all_excluded,
        state.recent_barkoba_secret_keys,
        selected_category,
        len(selected_secret),
        changed,
    )
    if os.getenv("ROBOT_DEBUG_BARKOBA_SECRET", "false").strip().casefold() == "true":
        logger.info("barkoba: provider=%s excluded=%s recent=%s selected=%s", secret_provider, all_excluded, state.recent_barkoba_secret_keys, selected_key)
    else:
        logger.info("barkoba: provider=%s excluded_count=%d recent_count=%d selected_category=%s", secret_provider, len(all_excluded), len(state.recent_barkoba_secret_keys), selected_category)
    logger.info("barkoba fact sheet source=%s category=%s", fact_source, selected_category)
    log_secret_choice(selected_category, len(selected_secret), selected_secret)
    return start_barkoba_reply(None, changed=changed)


def _handle_barkoba_start(state: Any, *, changed: bool, message: str = "") -> tuple[str, str, str]:
    try:
        excluded = excluded_secret_keys_from_message(message)
        reply = _start_barkoba_round(state, changed=changed, excluded_secret_keys=excluded)
        source = "barkoba_gpt" if getattr(state, "last_barkoba_secret_provider", None) == "gpt" else "barkoba_curated"
        return reply, source, "change_secret" if changed else "start_game"
    except GptBarkobaModelConfigError as exc:
        logger.error(
            "Configured OPENAI_CHAT_MODEL is invalid or unavailable model=%s answer_source=barkoba_gpt_error",
            exc.model,
        )
        clear_barkoba_state(state, remember_current=False)
        return BARKOBA_MODEL_CONFIG_REPLY, "barkoba_gpt_error", "model_config_error"


def _normalize_secret_key(secret: str) -> str:
    from .kids_safety import normalize_hungarian

    return normalize_hungarian(secret).replace(" ", "_")[:64] or "secret"


def _barkoba_engine_name() -> str:
    if should_use_gpt_engine():
        return "gpt"
    return "deterministic"


def _barkoba_secret_provider() -> str:
    value = os.getenv("ROBOT_BARKOBA_SECRET_PROVIDER", "curated").strip().casefold()
    if value in ("curated", "gpt"):
        return value
    logger.info("invalid ROBOT_BARKOBA_SECRET_PROVIDER=%s, using curated", value)
    return "curated"


def _unique_secret_keys(keys: list[str]) -> list[str]:
    unique: list[str] = []
    for key in keys:
        normalized = _normalize_secret_key(key)
        if normalized and normalized not in unique:
            unique.append(normalized)
    return unique


def _debug_secret_for_response(req: RobotChatRequest, state: Any) -> str | None:
    if req.device_id != "robot_assistant_simulator":
        return None
    if os.getenv("ROBOT_DEBUG_BARKOBA_SECRET", "false").strip().casefold() != "true":
        return None
    if not state.barkoba:
        return None
    return game_secret_name(state.barkoba)


def _debug_facts_for_response(req: RobotChatRequest, state: Any) -> dict[str, Any] | None:
    if req.device_id != "robot_assistant_simulator":
        return None
    if os.getenv("ROBOT_DEBUG_BARKOBA_SECRET", "false").strip().casefold() != "true":
        return None
    if not state.barkoba or not state.barkoba.fact_sheet:
        return None
    return state.barkoba.fact_sheet


def _barkoba_category_for_response(state: Any) -> str | None:
    if not state.barkoba:
        return None
    return game_secret_category(state.barkoba)


def _barkoba_turn_count(state: Any) -> int | None:
    if not state.barkoba:
        return None
    return state.barkoba.turns


def _recent_barkoba_for_response(state: Any) -> list[str]:
    return list(getattr(state, "recent_barkoba_secret_keys", []))


def _barkoba_excluded_for_response(state: Any) -> list[str]:
    return list(getattr(state, "last_barkoba_excluded_keys", []))


def _activity_state_for_response(state: Any) -> dict[str, Any] | None:
    activity_state = getattr(state, "activity_state", None)
    if isinstance(activity_state, dict) and activity_state:
        return dict(activity_state)
    return None


def _last_activity_for_response(state: Any) -> str | None:
    value = getattr(state, "last_activity", None)
    return str(value) if value else None


def _last_activity_topic_for_response(state: Any) -> str | None:
    value = getattr(state, "last_activity_topic", None)
    return str(value) if value else None


def _remember_activity_result(state: Any, activity: str | None, activity_state: dict[str, Any] | None) -> None:
    if not activity:
        return
    state.last_activity = activity
    topic = None
    if isinstance(activity_state, dict):
        topic = activity_state.get("topic") or activity_state.get("task_type")
    state.last_activity_topic = str(topic) if topic else _default_activity_topic(activity)


def _default_activity_topic(activity: str) -> str | None:
    topics = {
        "animal_fact": "animals",
        "robot_asks_quiz": "quiz",
        "drawing_idea": "drawing",
        "creative_task": "creative",
        "joke_or_riddle": "joke_or_riddle",
        "story": "story",
    }
    return topics.get(activity)


def _clear_pending_interaction(state: Any, *, clear_barkoba: bool = False) -> None:
    if clear_barkoba:
        clear_barkoba_state(state)
    else:
        state.active_mode = "kids_chat"
    state.active_activity = None
    state.activity_state = {}
    state.awaiting_input_type = None
    state.awaiting_input_for = None


def _awaiting_input_type_for_response(state: Any) -> str | None:
    value = getattr(state, "awaiting_input_type", None)
    return str(value) if value else None


def _awaiting_input_for_response(state: Any) -> str | None:
    value = getattr(state, "awaiting_input_for", None)
    return str(value) if value else None


def _should_generate_tts(req: RobotChatRequest) -> bool:
    return req.tts is not False


def _generate_reply_audio(req: RobotChatRequest, reply_text: str, conversation_id: str) -> str | None:
    requested = _should_generate_tts(req)
    enabled = tts_enabled()
    api_key_present = bool(os.getenv("OPENAI_API_KEY"))
    logger.info("tts requested=%s", str(requested).lower())
    logger.info("tts enabled=%s", str(enabled).lower())
    logger.info("openai api key present=%s", str(api_key_present).lower())
    logger.info("tts model=%s", tts_model())
    logger.info("tts voice=%s", tts_voice())

    if not requested:
        logger.info("tts skipped: request tts=false")
        return None
    if not enabled:
        logger.info("tts skipped: ROBOT_TTS_ENABLED=false")
        return None
    if not api_key_present:
        logger.info("tts skipped: OPENAI_API_KEY missing")
        return None
    if not reply_text:
        logger.info("tts skipped: empty reply_text")
        return None

    audio_url = generate_tts_audio(reply_text, conversation_id)
    if not audio_url:
        logger.warning("tts failed: generate_tts_audio returned no URL")
    else:
        logger.info("tts reply_audio_url=%s", audio_url)
    return audio_url


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


@app.get("/parent/settings", response_class=Utf8JSONResponse)
def parent_settings(x_parent_pin: str | None = Header(default=None, alias="X-Parent-PIN")) -> Utf8JSONResponse:
    if not parent_authorized(x_parent_pin):
        raise HTTPException(status_code=403, detail="parent pin required")
    return Utf8JSONResponse(content=settings_payload())


@app.post("/parent/settings", response_class=Utf8JSONResponse)
async def parent_settings_update(
    request: Request,
    x_parent_pin: str | None = Header(default=None, alias="X-Parent-PIN"),
) -> Utf8JSONResponse:
    if not parent_authorized(x_parent_pin):
        raise HTTPException(status_code=403, detail="parent pin required")
    payload = await request.json()
    if not isinstance(payload, dict):
        payload = {}
    return Utf8JSONResponse(content=update_settings(payload))


def handle_robot_chat_request(req: RobotChatRequest) -> RobotChatResponse:
    state, created = get_or_create_conversation(req.conversation_id, req.profile_id)
    mock_mode = _env_bool("ROBOT_BACKEND_MOCK", True)
    weather_mock = weather_mock_enabled()
    active_mode_before = state.active_mode

    safe, reason = check_message_safe(req.message)
    route = detect_intent(req.message, safe=safe)
    activity_route = detect_activity(req.message) if safe else None

    logger.info(
        "chat request incoming_conversation_id=%s resolved_conversation_id=%s new=%s profile_id=%s active_mode_before=%s intent=%s activity=%s mock=%s weather_mock=%s barkoba_engine=%s turn_count=%s message_length=%d",
        req.conversation_id or "(missing)",
        state.conversation_id,
        created,
        state.profile_id,
        active_mode_before,
        route.intent,
        activity_route.activity if activity_route else None,
        mock_mode,
        weather_mock,
        _barkoba_engine_name(),
        _barkoba_turn_count(state),
        len(req.message),
    )

    if not safe:
        blocked = blocked_response()
        response = _chat_response(
            safe=False,
            conversation_id=state.conversation_id,
            profile_id=state.profile_id,
            reply_text=str(blocked["reply_text"]),
            active_mode=state.active_mode,
            robot_mood=str(blocked["robot_mood"]),
            robot_action=str(blocked["robot_action"]),
            blocked_reason=str(blocked["blocked_reason"]),
            activity=activity_route.activity if activity_route else None,
            last_activity=_last_activity_for_response(state),
            last_activity_topic=_last_activity_topic_for_response(state),
            activity_state=_activity_state_for_response(state),
            awaiting_input_type=_awaiting_input_type_for_response(state),
            awaiting_input_for=_awaiting_input_for_response(state),
            session_memory=session_memory_snapshot(state),
            turn_count=_barkoba_turn_count(state),
            debug_secret=_debug_secret_for_response(req, state),
            barkoba_category=_barkoba_category_for_response(state),
            debug_barkoba_facts=_debug_facts_for_response(req, state),
            recent_barkoba_secrets=_recent_barkoba_for_response(state),
            barkoba_excluded_secrets=_barkoba_excluded_for_response(state),
            answer_source="blocked",
            backend_mock=mock_mode,
        )
        append_turn(state, req.message, response.reply_text)
        logger.info(
            "chat result returned_conversation_id=%s profile_id=%s safe=false intent=%s active_mode_before=%s active_mode_after=%s answer_source=blocked turn_count=%s reply_length=%d",
            state.conversation_id,
            state.profile_id,
            route.intent,
            active_mode_before,
            response.active_mode,
            response.turn_count,
            len(response.reply_text),
        )
        return response

    reply: str
    answer_source: str
    barkoba_reason: str | None = None
    barkoba_turn_count: int | None = None
    barkoba_category: str | None = None
    robot_mood = "happy"
    robot_action = "speak"
    activity: str | None = None
    activity_state: dict[str, Any] | None = None
    suggested_replies: list[str] | None = None

    claimed_profile_id, profile_known = detect_profile_claim(req.message)
    if route.intent == "profile_claim" or claimed_profile_id:
        if profile_known and claimed_profile_id:
            state.profile_id = claimed_profile_id
            _clear_pending_interaction(state, clear_barkoba=state.barkoba is not None)
        else:
            logger.info(
                "ignoring unknown profile claim conversation_id=%s existing_profile_id=%s",
                state.conversation_id,
                state.profile_id,
            )
            _clear_pending_interaction(state, clear_barkoba=state.barkoba is not None)
        reply = profile_claim_reply(get_profile(state.profile_id), known=profile_known)
        answer_source = "profile"
        state.active_mode = "kids_chat"
    elif state.active_mode == "barkoba" and state.barkoba is not None and is_barkoba_change_request(req.message):
        reply, answer_source, barkoba_reason = _handle_barkoba_start(state, changed=True, message=req.message)
        barkoba_turn_count = _barkoba_turn_count(state)
    elif state.active_mode == "barkoba" and state.barkoba is not None:
        game = state.barkoba
        barkoba_category = game_secret_category(game)
        if game.turns >= MAX_BARKOBA_TURNS:
            reply = max_turns_reply(game)
            answer_source = "barkoba_max_turns"
            barkoba_reason = "max_turns"
            barkoba_turn_count = game.turns
            logger.info(
                "barkoba ended conversation_id=%s profile_id=%s reason=%s turns=%d",
                state.conversation_id,
                state.profile_id,
                barkoba_reason,
                game.turns,
            )
            clear_barkoba_state(state)
        elif is_barkoba_give_up_request(req.message):
            reply = barkoba_give_up_reply(game)
            answer_source = "barkoba_give_up"
            barkoba_reason = "give_up"
            barkoba_turn_count = game.turns
            logger.info(
                "barkoba ended conversation_id=%s profile_id=%s reason=%s turns=%d",
                state.conversation_id,
                state.profile_id,
                barkoba_reason,
                game.turns,
            )
            clear_barkoba_state(state)
        elif (direct_guess := detect_direct_guess(game, req.message))[0]:
            _is_guess, correct = direct_guess
            if correct:
                reply = barkoba_correct_guess_reply(game)
                answer_source = "barkoba_direct_guess"
                barkoba_reason = "correct_guess"
                barkoba_turn_count = game.turns
                logger.info(
                    "barkoba ended conversation_id=%s profile_id=%s reason=%s turns=%d",
                    state.conversation_id,
                    state.profile_id,
                    barkoba_reason,
                    game.turns,
                )
                clear_barkoba_state(state)
            else:
                reply = barkoba_wrong_guess_reply()
                answer_source = "barkoba_direct_guess"
                barkoba_reason = "wrong_guess"
                barkoba_turn_count = game.turns
                game.last_reason = barkoba_reason
                state.active_mode = "barkoba"
        elif (fact_match := answer_from_fact_sheet(game.fact_sheet, req.message))[0]:
            _matched, fact_value, fact_key = fact_match
            game.turns += 1
            reply = "Igen." if fact_value else "Nem."
            answer_source = "barkoba_fact_sheet"
            barkoba_reason = "yes_no_fact"
            barkoba_turn_count = game.turns
            game.last_reason = barkoba_reason
            logger.info(
                "barkoba fact answer conversation_id=%s profile_id=%s fact_key=%s value=%s turns=%d",
                state.conversation_id,
                state.profile_id,
                fact_key,
                fact_value,
                game.turns,
            )
        elif game.engine == "gpt":
            profile = get_profile(state.profile_id)
            try:
                result = handle_gpt_barkoba_turn(
                    secret=game_secret_name(game),
                    category=game_secret_category(game),
                    fact_sheet=game.fact_sheet,
                    turn_count=game.turns,
                    profile_display_name=profile.display_name,
                    message=req.message,
                )
            except GptBarkobaModelConfigError as exc:
                logger.error(
                    "Configured OPENAI_CHAT_MODEL is invalid or unavailable model=%s answer_source=barkoba_gpt_error",
                    exc.model,
                )
                reply = BARKOBA_MODEL_CONFIG_REPLY
                answer_source = "barkoba_gpt_error"
                barkoba_reason = "model_config_error"
                barkoba_turn_count = game.turns
                logger.info(
                    "barkoba ended conversation_id=%s profile_id=%s reason=%s turns=%d",
                    state.conversation_id,
                    state.profile_id,
                    barkoba_reason,
                    game.turns,
                )
                clear_barkoba_state(state)
            else:
                game.turns += result.turn_count_increment
                game.last_reason = result.reason
                reply = result.reply_text
                answer_source = "barkoba_gpt"
                barkoba_reason = result.reason
                barkoba_turn_count = game.turns
                if result.game_over or result.active_mode == "kids_chat":
                    logger.info(
                        "barkoba ended conversation_id=%s profile_id=%s reason=%s turns=%d",
                        state.conversation_id,
                        state.profile_id,
                        barkoba_reason,
                        game.turns,
                    )
                    clear_barkoba_state(state)
                elif game.turns >= MAX_BARKOBA_TURNS:
                    reply = max_turns_reply(game)
                    answer_source = "barkoba_max_turns"
                    barkoba_reason = "max_turns"
                    barkoba_turn_count = game.turns
                    logger.info(
                        "barkoba ended conversation_id=%s profile_id=%s reason=%s turns=%d",
                        state.conversation_id,
                        state.profile_id,
                        barkoba_reason,
                        game.turns,
                    )
                    clear_barkoba_state(state)
                else:
                    state.active_mode = "barkoba"
        else:
            reply, ended, deterministic_reason = answer_barkoba_turn(game, req.message)
            answer_source = "deterministic"
            barkoba_reason = deterministic_reason
            barkoba_turn_count = game.turns
            game.last_reason = barkoba_reason
            if ended:
                logger.info(
                    "barkoba ended conversation_id=%s profile_id=%s reason=%s turns=%d",
                    state.conversation_id,
                    state.profile_id,
                    barkoba_reason,
                    game.turns,
                )
                clear_barkoba_state(state)
    elif (activity_reply := handle_active_activity_turn(state, req.message)) is not None:
        reply = activity_reply.reply_text
        answer_source = activity_reply.answer_source
        activity = activity_reply.activity
        activity_state = activity_reply.activity_state or _activity_state_for_response(state)
        suggested_replies = activity_reply.suggested_replies
        robot_mood = activity_reply.robot_mood
        robot_action = activity_reply.robot_action
    elif (activity_reply := handle_activity_followup(state, req.message)) is not None:
        reply = activity_reply.reply_text
        answer_source = activity_reply.answer_source
        activity = activity_reply.activity
        activity_state = activity_reply.activity_state or _activity_state_for_response(state)
        suggested_replies = activity_reply.suggested_replies
        robot_mood = activity_reply.robot_mood
        robot_action = activity_reply.robot_action
    elif (activity_reply := handle_activity_continuation(state, req.message, mock_mode)) is not None:
        reply = activity_reply.reply_text
        answer_source = activity_reply.answer_source
        activity = activity_reply.activity
        activity_state = activity_reply.activity_state or _activity_state_for_response(state)
        suggested_replies = activity_reply.suggested_replies
        robot_mood = activity_reply.robot_mood
        robot_action = activity_reply.robot_action
    elif is_barkoba_give_up_request(req.message):
        reply = "Most épp nem barkóbázunk. Ha szeretnéd, kezdhetünk egy új barkóbát."
        answer_source = "barkoba_not_active"
        barkoba_reason = "not_active"
        state.active_mode = "kids_chat"
    elif route.intent == "barkoba_change" and state.recent_barkoba_secret_keys:
        reply, answer_source, barkoba_reason = _handle_barkoba_start(state, changed=True, message=req.message)
        barkoba_turn_count = _barkoba_turn_count(state)
    elif route.intent == "barkoba_start":
        reply, answer_source, barkoba_reason = _handle_barkoba_start(state, changed=False, message=req.message)
        barkoba_turn_count = _barkoba_turn_count(state)
    elif activity_route and activity_route.activity:
        activity_reply = handle_activity(
            activity=activity_route.activity,
            message=req.message,
            state=state,
            mock_mode=mock_mode,
        )
        reply = activity_reply.reply_text
        answer_source = activity_reply.answer_source
        activity = activity_reply.activity
        activity_state = activity_reply.activity_state or _activity_state_for_response(state)
        suggested_replies = activity_reply.suggested_replies
        robot_mood = activity_reply.robot_mood
        robot_action = activity_reply.robot_action
    else:
        reply, answer_source = _answer_safe_request(
            req,
            route.intent,
            mock_mode,
            state.profile_id,
            state.conversation_id,
        )
        if answer_source.startswith("story_"):
            robot_mood = "story"
            activity = "story"

    _remember_activity_result(state, activity, activity_state if activity_state is not None else _activity_state_for_response(state))

    reply_audio_url = _generate_reply_audio(req, reply, state.conversation_id)

    response = _chat_response(
        safe=True,
        conversation_id=state.conversation_id,
        profile_id=state.profile_id,
        reply_text=reply,
        reply_audio_url=reply_audio_url,
        active_mode=state.active_mode,
        robot_mood=robot_mood,
        robot_action=robot_action,
        activity=activity,
        last_activity=_last_activity_for_response(state),
        last_activity_topic=_last_activity_topic_for_response(state),
        activity_state=activity_state if activity_state is not None else _activity_state_for_response(state),
        awaiting_input_type=_awaiting_input_type_for_response(state),
        awaiting_input_for=_awaiting_input_for_response(state),
        suggested_replies=suggested_replies,
        session_memory=session_memory_snapshot(state),
        barkoba_reason=barkoba_reason,
        turn_count=barkoba_turn_count if barkoba_turn_count is not None else _barkoba_turn_count(state),
        debug_secret=_debug_secret_for_response(req, state),
        barkoba_category=barkoba_category if barkoba_category is not None else _barkoba_category_for_response(state),
        debug_barkoba_facts=_debug_facts_for_response(req, state),
        recent_barkoba_secrets=_recent_barkoba_for_response(state),
        barkoba_excluded_secrets=_barkoba_excluded_for_response(state),
        answer_source=answer_source,
        backend_mock=mock_mode,
    )
    append_turn(state, req.message, response.reply_text)
    logger.info(
        "chat result returned_conversation_id=%s profile_id=%s safe=true intent=%s active_mode_before=%s active_mode_after=%s answer_source=%s barkoba_engine=%s turn_count=%s reason=%s reply_length=%d",
        state.conversation_id,
        state.profile_id,
        route.intent,
        active_mode_before,
        state.active_mode,
        answer_source,
        _barkoba_engine_name(),
        response.turn_count,
        barkoba_reason,
        len(response.reply_text),
    )
    return response


@app.post("/api/robot/chat", response_model=RobotChatResponse, response_class=Utf8JSONResponse)
def robot_chat(req: RobotChatRequest) -> Utf8JSONResponse:
    try:
        return _json_response(handle_robot_chat_request(req))
    except Exception:
        logger.exception("chat route failed, returning complete safe fallback")
        state, _created = get_or_create_conversation(req.conversation_id, req.profile_id)
        return _json_response(
            RobotChatResponse(
                ok=False,
                safe=True,
                conversation_id=state.conversation_id,
                profile_id=state.profile_id,
                reply_text="Most kicsit lassan gondolkodom. Próbáljuk meg még egyszer.",
                reply_audio_url=None,
                robot_mood="confused",
                robot_action="speak",
                blocked_reason=None,
                active_mode=state.active_mode if state.active_mode else "kids_chat",  # type: ignore[arg-type]
                answer_source="openai_error_fallback",
                backend_mock=_env_bool("ROBOT_BACKEND_MOCK", True),
            )
        )


@app.post("/api/robot/voice-chat", response_model=RobotChatResponse, response_class=Utf8JSONResponse)
async def robot_voice_chat(
    audio: UploadFile = File(...),
    device_id: str = Form(default="robot_assistant_esp32s3"),
    locale: str = Form(default="hu-HU"),
    mode: str = Form(default="kids_chat"),
    conversation_id: str | None = Form(default=None),
    profile_id: str | None = Form(default=None),
    max_answer_seconds: int = Form(default=30),
    tts: bool | None = Form(default=True),
) -> Utf8JSONResponse:
    content = await audio.read()
    size = len(content)
    max_bytes = max_upload_bytes()
    logger.info(
        "voice: received audio filename=%s size=%d content_type=%s max_bytes=%d",
        audio.filename,
        size,
        audio.content_type,
        max_bytes,
    )
    if size <= 0 or size > max_bytes:
        logger.warning("voice: rejected audio size=%d", size)
        return _json_response(_voice_failure_response(conversation_id, profile_id, tts=tts is not False))

    try:
        transcript = transcribe_wav(content, audio.filename or "VOICE.WAV", audio.content_type or "audio/wav")
    except Exception:
        logger.exception("voice: transcription failed model=%s", stt_model())
        return _json_response(_voice_failure_response(conversation_id, profile_id, tts=tts is not False))

    logger.info(
        "voice: transcript length=%d conversation_id=%s profile_id=%s",
        len(transcript),
        conversation_id or "(missing)",
        profile_id or "(missing)",
    )
    if not transcript:
        return _json_response(_voice_failure_response(conversation_id, profile_id, tts=tts is not False))

    chat_req = RobotChatRequest(
        conversation_id=conversation_id,
        profile_id=profile_id,
        device_id=device_id,
        locale=locale,
        mode="kids_chat",
        message=transcript,
        max_answer_seconds=max_answer_seconds,
        tts=tts is not False,
    )
    response = handle_robot_chat_request(chat_req)
    response.transcript = transcript
    logger.info(
        "voice: chat complete ok=%s safe=%s transcript_length=%d reply_audio=%s conversation_id=%s profile_id=%s",
        response.ok,
        response.safe,
        len(transcript),
        "yes" if response.reply_audio_url else "no",
        response.conversation_id,
        response.profile_id,
    )
    return _json_response(response)


@app.post("/api/robot/voice-chat-raw", response_model=RobotChatResponse, response_class=Utf8JSONResponse)
async def robot_voice_chat_raw(
    request: Request,
    device_id: str = "robot_assistant_esp32s3",
    locale: str = "hu-HU",
    mode: str = "kids_chat",
    conversation_id: str | None = None,
    profile_id: str | None = None,
    max_answer_seconds: int = 30,
    tts: bool | None = True,
) -> Utf8JSONResponse:
    logger.info(
        "voice-chat-raw: method=%s content-type=%s content-length=%s",
        request.method,
        request.headers.get("content-type", ""),
        request.headers.get("content-length", ""),
    )
    try:
        content = await request.body()
    except ClientDisconnect:
        logger.warning("voice-chat-raw: client disconnected while uploading")
        return Utf8JSONResponse(
            status_code=499,
            content={
                "ok": False,
                "safe": True,
                "transcript": "",
                "conversation_id": conversation_id or "",
                "profile_id": profile_id or "guest",
                "reply_text": "A hangfeltöltés megszakadt. Próbáld meg újra.",
                "reply_audio_url": None,
                "robot_mood": "confused",
                "robot_action": "speak",
                "active_mode": mode or "kids_chat",
                "blocked_reason": None,
            },
        )
    size = len(content)
    max_bytes = max_upload_bytes()
    logger.info("voice-chat-raw: received bytes=%d max_bytes=%d", size, max_bytes)
    if size <= 0 or size > max_bytes:
        logger.warning("voice-chat-raw: rejected audio size=%d", size)
        return _json_response(_voice_failure_response(conversation_id, profile_id, tts=tts is not False))

    temp_path: str | None = None
    try:
        with tempfile.NamedTemporaryFile(prefix="robot_voice_", suffix=".wav", delete=False) as tmp:
            tmp.write(content)
            temp_path = tmp.name
        logger.info("voice-chat-raw: saved temp wav=%s bytes=%d", temp_path, size)
        logger.info("voice-chat-raw: STT model=%s", stt_model())
        transcript = transcribe_wav(content, "VOICE.WAV", request.headers.get("content-type") or "audio/wav")
    except Exception:
        logger.exception("voice-chat-raw: transcription failed model=%s", stt_model())
        return _json_response(_voice_failure_response(conversation_id, profile_id, tts=tts is not False))
    finally:
        if temp_path:
            try:
                os.unlink(temp_path)
            except OSError:
                logger.warning("voice-chat-raw: failed to remove temp wav=%s", temp_path)

    logger.info(
        "voice-chat-raw: transcript length=%d conversation_id=%s profile_id=%s",
        len(transcript),
        conversation_id or "(missing)",
        profile_id or "(missing)",
    )
    if not transcript:
        return _json_response(_voice_failure_response(conversation_id, profile_id, tts=tts is not False))

    chat_req = RobotChatRequest(
        conversation_id=conversation_id,
        profile_id=profile_id,
        device_id=device_id,
        locale=locale,
        mode=mode or "kids_chat",
        message=transcript,
        max_answer_seconds=max_answer_seconds,
        tts=tts is not False,
    )
    response = handle_robot_chat_request(chat_req)
    response.transcript = transcript
    logger.info(
        "voice-chat-raw: chat ok=%s safe=%s tts=%s conversation_id=%s profile_id=%s",
        response.ok,
        response.safe,
        "ok" if response.reply_audio_url else "missing",
        response.conversation_id,
        response.profile_id,
    )
    return _json_response(response)


def _voice_failure_response(
    conversation_id: str | None,
    profile_id: str | None,
    *,
    tts: bool = True,
) -> RobotChatResponse:
    state, _created = get_or_create_conversation(conversation_id, profile_id)
    reply_text = "Most nem hallottalak jól. Megpróbálod még egyszer?"
    tts_req = RobotChatRequest(
        conversation_id=state.conversation_id,
        profile_id=state.profile_id,
        device_id="robot_assistant_esp32s3",
        locale="hu-HU",
        mode="kids_chat",
        message="voice fallback",
        max_answer_seconds=30,
        tts=tts,
    )
    reply_audio_url = _generate_reply_audio(tts_req, reply_text, state.conversation_id)
    logger.info(
        "voice failure response: tts=%s reply_audio=%s conversation_id=%s profile_id=%s",
        str(tts).lower(),
        "yes" if reply_audio_url else "missing",
        state.conversation_id,
        state.profile_id,
    )
    return RobotChatResponse(
        ok=True,
        safe=True,
        transcript="",
        conversation_id=state.conversation_id,
        profile_id=state.profile_id,
        reply_text=reply_text,
        reply_audio_url=reply_audio_url,
        robot_mood="confused",
        robot_action="speak",
        blocked_reason=None,
        active_mode=state.active_mode,  # type: ignore[arg-type]
        answer_source="voice_stt_fallback",
    )


def _cleanup_expired_voice_uploads() -> None:
    now = time.time()
    expired = [
        upload_id
        for upload_id, meta in VOICE_UPLOADS.items()
        if now - float(meta.get("created_at", now)) > VOICE_UPLOAD_TTL_SECONDS
    ]
    for upload_id in expired:
        meta = VOICE_UPLOADS.pop(upload_id, None)
        if not meta:
            continue
        try:
            Path(str(meta["path"])).unlink(missing_ok=True)
        except OSError:
            logger.warning("voice-upload-cleanup: failed to remove upload_id=%s", upload_id)


def _voice_upload_not_found(upload_id: str) -> Utf8JSONResponse:
    return Utf8JSONResponse(status_code=404, content={"ok": False, "error": "invalid upload_id", "upload_id": upload_id})


@app.post("/api/robot/voice-upload/start", response_model=VoiceUploadStartResponse, response_class=Utf8JSONResponse)
def voice_upload_start(req: VoiceUploadStartRequest) -> Utf8JSONResponse:
    _cleanup_expired_voice_uploads()
    if req.file_size <= 44 or req.file_size > max_upload_bytes():
        return Utf8JSONResponse(status_code=400, content={"ok": False, "error": "invalid file_size"})

    upload_id = uuid.uuid4().hex
    path = VOICE_UPLOAD_DIR / f"{upload_id}.wav"
    path.write_bytes(b"")
    VOICE_UPLOADS[upload_id] = {
        "path": str(path),
        "expected_size": req.file_size,
        "received": 0,
        "device_id": req.device_id,
        "locale": req.locale,
        "mode": req.mode or "kids_chat",
        "conversation_id": req.conversation_id,
        "profile_id": req.profile_id,
        "tts": req.tts is not False,
        "created_at": time.time(),
    }
    logger.info(
        "voice-upload-start: upload_id=%s expected_size=%d profile_id=%s",
        upload_id,
        req.file_size,
        req.profile_id or "(missing)",
    )
    return _json_response(VoiceUploadStartResponse(ok=True, upload_id=upload_id))


@app.post("/api/robot/voice-upload/chunk", response_class=Utf8JSONResponse)
async def voice_upload_chunk(
    request: Request,
    upload_id: str = Query(...),
    offset: int = Query(..., ge=0),
    size: int = Query(..., gt=0, le=64 * 1024),
) -> Utf8JSONResponse:
    meta = VOICE_UPLOADS.get(upload_id)
    if not meta:
        return _voice_upload_not_found(upload_id)

    try:
        chunk = await request.body()
    except ClientDisconnect:
        logger.warning("voice-upload-chunk: client disconnected upload_id=%s offset=%d", upload_id, offset)
        return Utf8JSONResponse(status_code=499, content={"ok": False, "error": "client disconnected"})

    if len(chunk) != size:
        return Utf8JSONResponse(
            status_code=400,
            content={"ok": False, "error": "chunk size mismatch", "expected": size, "actual": len(chunk)},
        )
    expected_size = int(meta["expected_size"])
    if offset + len(chunk) > expected_size:
        return Utf8JSONResponse(status_code=400, content={"ok": False, "error": "chunk exceeds expected file size"})

    path = Path(str(meta["path"]))
    current_size = path.stat().st_size if path.exists() else 0
    if offset > current_size:
        return Utf8JSONResponse(
            status_code=409,
            content={"ok": False, "error": "chunk offset gap", "received": current_size},
        )

    with path.open("r+b") as f:
        f.seek(offset)
        f.write(chunk)
        f.flush()
    received = path.stat().st_size
    meta["received"] = received
    logger.info(
        "voice-upload-chunk: upload_id=%s offset=%d size=%d received=%d",
        upload_id,
        offset,
        len(chunk),
        received,
    )
    return Utf8JSONResponse(content={"ok": True, "received": received})


@app.post("/api/robot/voice-upload/finish", response_model=RobotChatResponse, response_class=Utf8JSONResponse)
def voice_upload_finish(req: VoiceUploadFinishRequest) -> Utf8JSONResponse:
    meta = VOICE_UPLOADS.get(req.upload_id)
    if not meta:
        return _voice_upload_not_found(req.upload_id)

    path = Path(str(meta["path"]))
    if not path.exists():
        VOICE_UPLOADS.pop(req.upload_id, None)
        return Utf8JSONResponse(status_code=404, content={"ok": False, "error": "upload file missing"})

    final_size = path.stat().st_size
    expected_size = int(meta["expected_size"])
    logger.info(
        "voice-upload-finish: upload_id=%s final_size=%d expected_size=%d",
        req.upload_id,
        final_size,
        expected_size,
    )
    if final_size != expected_size:
        return Utf8JSONResponse(
            status_code=400,
            content={"ok": False, "error": "final size mismatch", "final_size": final_size, "expected_size": expected_size},
        )

    try:
        audio = path.read_bytes()
        logger.info("voice-upload-finish: transcribing")
        transcript = transcribe_wav(audio, "VOICE.WAV", "audio/wav")
    except Exception:
        logger.exception("voice-upload-finish: transcription failed model=%s", stt_model())
        return _json_response(
            _voice_failure_response(
                meta.get("conversation_id"),
                meta.get("profile_id"),
                tts=bool(meta.get("tts", True)),
            )
        )
    finally:
        try:
            path.unlink(missing_ok=True)
        except OSError:
            logger.warning("voice-upload-finish: failed to remove upload file upload_id=%s", req.upload_id)
        VOICE_UPLOADS.pop(req.upload_id, None)

    logger.info("voice-upload-finish: transcript length=%d", len(transcript))
    if not transcript:
        return _json_response(
            _voice_failure_response(
                meta.get("conversation_id"),
                meta.get("profile_id"),
                tts=bool(meta.get("tts", True)),
            )
        )

    chat_req = RobotChatRequest(
        conversation_id=meta.get("conversation_id"),
        profile_id=meta.get("profile_id"),
        device_id=str(meta.get("device_id") or "robot_assistant_esp32s3"),
        locale=str(meta.get("locale") or "hu-HU"),
        mode="kids_chat",
        message=transcript,
        max_answer_seconds=30,
        tts=bool(meta.get("tts", True)),
    )
    response = handle_robot_chat_request(chat_req)
    response.transcript = transcript
    logger.info(
        "voice-upload-finish: chat ok safe=%s tts=%s",
        response.safe,
        "ok" if response.reply_audio_url else "missing",
    )
    return _json_response(response)


@app.post("/api/robot/voice-upload/cancel", response_class=Utf8JSONResponse)
def voice_upload_cancel(req: VoiceUploadCancelRequest) -> Utf8JSONResponse:
    meta = VOICE_UPLOADS.pop(req.upload_id, None)
    if not meta:
        return _voice_upload_not_found(req.upload_id)
    try:
        Path(str(meta["path"])).unlink(missing_ok=True)
    except OSError:
        logger.warning("voice-upload-cancel: failed to remove upload_id=%s", req.upload_id)
    logger.info("voice-upload-cancel: upload_id=%s", req.upload_id)
    return Utf8JSONResponse(content={"ok": True})
