from __future__ import annotations

import json
import logging
import os
from dataclasses import dataclass, field
from typing import Any

from openai import OpenAI

from .activity_router import ActivityName
from .barkoba_gpt import openai_chat_model
from .kids_safety import check_message_safe, normalize_hungarian
from .personality import PERSONALITY_PROMPT
from .profiles import get_profile
from .session_memory import handle_session_memory, session_memory_snapshot

logger = logging.getLogger("robot_backend.activities")


@dataclass(frozen=True)
class ActivityReply:
    reply_text: str
    activity: str
    active_mode: str = "kids_chat"
    activity_state: dict[str, Any] = field(default_factory=dict)
    suggested_replies: list[str] = field(default_factory=list)
    robot_mood: str = "happy"
    robot_action: str = "speak"
    answer_source: str = "activity_mock"


COLORS: dict[str, str] = {
    "lila": "lila",
    "kek": "kék",
    "sarga": "sárga",
    "piros": "piros",
    "zold": "zöld",
    "rozsaszin": "rózsaszín",
}

ANSWER_SYNONYMS: dict[str, set[str]] = {
    "macska": {"macska", "cica", "cicus"},
    "cica": {"macska", "cica", "cicus"},
    "cicus": {"macska", "cica", "cicus"},
    "elefant": {"elefant"},
    "nap": {"nap", "napocska"},
    "napocska": {"nap", "napocska"},
    "kek": {"kek"},
    "sarga": {"sarga"},
}

RIDDLE_FALLBACKS: tuple[dict[str, Any], ...] = (
    {
        "riddle_text": "Kicsi, piros, fán terem, és finom. Mi az?",
        "expected_answers": ["alma"],
        "display_answer": "alma",
        "hint": "Gyümölcs.",
        "topic": "fruit",
        "wrong_options": ["eper"],
    },
    {
        "riddle_text": "Nappal világít, este eltűnik. Mi az?",
        "expected_answers": ["nap", "napocska"],
        "display_answer": "napocska",
        "hint": "Az égen látjuk.",
        "topic": "nature",
        "wrong_options": ["hold"],
    },
    {
        "riddle_text": "Puha, nyávog, és szereti, ha megsimogatják. Mi az?",
        "expected_answers": ["cica", "macska", "cicus"],
        "display_answer": "cica",
        "hint": "Kedves háziállat.",
        "topic": "animals",
        "wrong_options": ["kutya"],
    },
)

ANIMAL_FACT_FALLBACKS = (
    "A delfinek füttyökkel és kattogó hangokkal kommunikálnak egymással.",
    "Az elefántok az ormányukkal isznak, szagolnak, tárgyakat emelnek, és még köszönni is tudnak vele.",
    "A pingvinek madarak, de nem repülnek: inkább ügyesen úsznak a vízben.",
)


def handle_active_activity_turn(state: Any, message: str) -> ActivityReply | None:
    if state.active_mode == "quiz" and _quiz_expected_answers(getattr(state, "activity_state", {})):
        return _answer_quiz(state, message)
    if state.active_mode == "riddle" and getattr(state, "activity_state", {}).get("expected_answer"):
        return _answer_riddle(state, message)
    return None


def handle_activity_followup(state: Any, message: str) -> ActivityReply | None:
    if getattr(state, "awaiting_input_for", None) == "creative_task":
        return _creative_followup(state, message)
    return None


def handle_activity_continuation(state: Any, message: str, mock_mode: bool) -> ActivityReply | None:
    if not _is_generic_continuation(message):
        return None
    last_activity = getattr(state, "last_activity", None)
    if last_activity in {"animal_fact", "drawing_idea", "creative_task", "joke_or_riddle", "robot_asks_quiz"}:
        return handle_activity(
            activity=last_activity,  # type: ignore[arg-type]
            message=message,
            state=state,
            mock_mode=mock_mode,
        )
    return None


def handle_activity(
    *,
    activity: ActivityName,
    message: str,
    state: Any,
    mock_mode: bool,
) -> ActivityReply:
    if activity == "teach_me_session_memory":
        return _memory_activity(message, state)
    if activity == "robot_asks_quiz":
        return _quiz_activity(state, message, mock_mode)
    if activity == "calm_sleep_mode":
        return _simple_activity(
            activity,
            message,
            state,
            mock_mode,
            mock_reply=(
                "Vegyünk együtt három lassú levegőt. Beszívjuk... kifújjuk... Ügyes vagy. "
                "Most képzelj el egy puha felhőt, amin csendben pihen egy kis csillag."
            ),
            mood="calm",
            suggested=["Kérek nyugtató mesét", "Még egy levegőt", "Jó éjszakát"],
        )
    if activity == "daily_routine":
        return _daily_activity(message, state, mock_mode)
    if activity == "drawing_idea":
        return _simple_activity(
            activity,
            message,
            state,
            mock_mode,
            mock_reply="Rajzolj egy lila unikornist, aki csillagos hídon sétál, mellette pedig mosolyog egy kis hold.",
            suggested=["Kérek könnyebbet", "Kérek viccesebbet", "Kérek unikornisosat"],
        )
    if activity == "creative_task":
        return _creative_task_start(state, message, mock_mode)
    if activity == "joke_or_riddle":
        return _joke_or_riddle_activity(message, state, mock_mode)
    if activity == "animal_fact":
        return _animal_fact_activity(message, state, mock_mode)
    if activity == "parent_mode_basic":
        return ActivityReply(
            reply_text="A szülői beállításokhoz kérj meg egy felnőttet, hogy használja a védett parent endpointot.",
            activity=activity,
            robot_mood="gentle",
            suggested_replies=["Mondj mesét", "Játsszunk barkóbát", "Mit rajzoljak?"],
        )
    return ActivityReply(
        reply_text="Válasszunk egy kedves játékot: kérdezhetek, adhatok rajzötletet, vagy mondhatok egy találóst.",
        activity=activity,
    )


def _quiz_activity(state: Any, message: str, mock_mode: bool) -> ActivityReply:
    quiz_state = _maybe_gpt_quiz(state, message) or _fallback_quiz_state()
    source = "quiz_gpt" if quiz_state.get("_source") == "gpt" else "activity_mock"
    quiz_state.pop("_source", None)

    state.active_mode = "quiz"
    state.active_activity = "robot_asks_quiz"
    state.activity_state = dict(quiz_state)
    _clear_awaiting(state)
    return ActivityReply(
        reply_text=str(quiz_state["question"]),
        activity="robot_asks_quiz",
        active_mode="quiz",
        activity_state=dict(state.activity_state),
        suggested_replies=_quiz_suggested_replies(quiz_state),
        answer_source=source,
    )


def _maybe_gpt_quiz(state: Any, message: str) -> dict[str, Any] | None:
    payload = _maybe_gpt_activity(
        "ROBOT_QUIZ_ENGINE",
        message,
        state,
        False,
        (
            "Create one easy playful Hungarian quiz question for a young child. "
            "Return strict JSON only with keys: question, expected_answers, display_answer, "
            "wrong_options, explanation, topic, difficulty. expected_answers must include synonyms where useful."
        ),
    )
    if not payload:
        return None
    try:
        quiz_state = _validate_quiz_payload(payload)
        quiz_state["_source"] = "gpt"
        return quiz_state
    except ValueError as exc:
        logger.info("quiz GPT payload invalid reason=%s", exc)
        return None


def _answer_quiz(state: Any, message: str) -> ActivityReply:
    quiz_state = dict(state.activity_state)
    expected_answers = _quiz_expected_answers(quiz_state)
    answer = _normalize_answer(message)
    correct = any(_answer_matches(answer, expected) for expected in expected_answers)
    display_answer = str(quiz_state.get("display_answer") or (expected_answers[0] if expected_answers else ""))
    explanation = str(quiz_state.get("explanation") or f"A jó válasz: {display_answer}.")

    state.active_mode = "kids_chat"
    state.active_activity = None
    state.activity_state = {
        "correct": correct,
        "expected_answers": expected_answers,
        "display_answer": display_answer,
        "explanation": explanation,
        "topic": quiz_state.get("topic", "general"),
    }
    _clear_awaiting(state)
    reply = f"Ügyes vagy! {explanation}" if correct else f"Majdnem! {explanation}"
    return ActivityReply(
        reply_text=reply,
        activity="robot_asks_quiz",
        activity_state=dict(state.activity_state),
        suggested_replies=["Kérek még egy kérdést", "Játsszunk barkóbát", "Mondj találóst"],
        answer_source="quiz_eval",
    )


def _creative_task_start(state: Any, message: str, mock_mode: bool) -> ActivityReply:
    generated = _maybe_gpt_activity(
        "ROBOT_CREATIVE_ENGINE",
        message,
        state,
        mock_mode,
        "Create one small interactive Hungarian creative task for a young child. Prefer asking for a color as the next step.",
    )
    reply = (
        str(generated.get("reply_text"))
        if generated
        else "Találjunk ki egy varázsigét, ami virágokat ébreszt. Milyen színű legyen az első virág?"
    )
    state.active_mode = "creative_task"
    state.active_activity = "creative_task"
    state.awaiting_input_type = "color"
    state.awaiting_input_for = "creative_task"
    state.activity_state = {
        "task_type": "magic_flower_spell",
        "step": "awaiting_color",
        "prompt": "Milyen színű legyen az első virág?",
    }
    return ActivityReply(
        reply_text=reply,
        activity="creative_task",
        active_mode="creative_task",
        activity_state=dict(state.activity_state),
        suggested_replies=["Lila", "Sárga", "Kék"],
        answer_source="creative_gpt" if generated else "activity_mock",
    )


def _creative_followup(state: Any, message: str) -> ActivityReply:
    color = _detect_color(message)
    if not color:
        return ActivityReply(
            reply_text="Válasszunk egy színt az első virágnak: lila, kék, sárga, piros, zöld vagy rózsaszín?",
            activity="creative_task",
            active_mode="creative_task",
            activity_state=dict(state.activity_state),
            suggested_replies=["Lila", "Kék", "Sárga"],
            answer_source="creative_task_followup",
        )

    reply = (
        f"Szuper, legyen az első virág {color}! Mondhatjuk ezt a varázsigét: "
        f"{_spell_for_color(color)}, puha fény, ébredj virág, nyílj ma még!"
    )
    state.active_mode = "kids_chat"
    state.active_activity = None
    state.activity_state = {"task_type": "magic_flower_spell", "chosen_color": color, "step": "completed"}
    _clear_awaiting(state)
    return ActivityReply(
        reply_text=reply,
        activity="creative_task",
        active_mode="kids_chat",
        activity_state=dict(state.activity_state),
        suggested_replies=["Legyen még egy virág", "Találjunk ki másik varázsigét", "Kérek új kreatív feladatot"],
        answer_source="creative_task_followup",
    )


def _joke_or_riddle_activity(message: str, state: Any, mock_mode: bool) -> ActivityReply:
    text = normalize_hungarian(message)
    if "talalos" in text:
        riddle_state = _maybe_gpt_riddle(state, message) or _fallback_riddle_state()
        source = "riddle_gpt" if riddle_state.get("_source") == "gpt" else "riddle_fallback"
        riddle_state.pop("_source", None)
        state.active_mode = "riddle"
        state.active_activity = "joke_or_riddle"
        state.activity_state = dict(riddle_state)
        _clear_awaiting(state)
        return ActivityReply(
            reply_text=str(riddle_state["riddle_text"]),
            activity="joke_or_riddle",
            active_mode="riddle",
            activity_state=dict(state.activity_state),
            suggested_replies=_riddle_suggested_replies(riddle_state),
            answer_source=source,
        )

    generated = _maybe_gpt_activity(
        "ROBOT_JOKE_ENGINE",
        message,
        state,
        mock_mode,
        "Create one short, kind, child-safe Hungarian joke or riddle. No rude, scary, political, or adult content.",
    )
    reply = str(generated.get("reply_text")) if generated else "Mit mond a felhő, amikor boldog? Ma jó kedvem van, és cseppnyi mosolyt hozok!"
    return ActivityReply(
        reply_text=reply,
        activity="joke_or_riddle",
        suggested_replies=["Mondj még egyet", "Mondj találóst", "Mit rajzoljak?"],
        answer_source="activity_gpt" if generated else "activity_mock",
    )


def _answer_riddle(state: Any, message: str) -> ActivityReply:
    riddle_state = dict(state.activity_state)
    expected_answers = _quiz_expected_answers(riddle_state)
    answer = _normalize_answer(message)
    correct = any(_answer_matches(answer, expected) for expected in expected_answers)
    display_answer = str(riddle_state.get("display_answer") or (expected_answers[0] if expected_answers else ""))
    state.active_mode = "kids_chat"
    state.active_activity = None
    state.activity_state = {}
    _clear_awaiting(state)
    reply = (
        f"Ügyes vagy! Igen, a megfejtés: {display_answer}."
        if correct
        else f"Majdnem! A megfejtés: {display_answer}. Kérsz még egy találós kérdést?"
    )
    return ActivityReply(
        reply_text=reply,
        activity="joke_or_riddle",
        activity_state={
            "correct": correct,
            "expected_answers": expected_answers,
            "display_answer": display_answer,
            "topic": riddle_state.get("topic", "general"),
        },
        suggested_replies=["Mondj még egy találós kérdést", "Mondj egy viccet", "Játsszunk barkóbát"],
        answer_source="riddle_eval",
    )


def _animal_fact_activity(message: str, state: Any, mock_mode: bool) -> ActivityReply:
    generated = _maybe_gpt_activity(
        "ROBOT_ANIMAL_FACT_ENGINE",
        message,
        state,
        mock_mode,
        "Create one short Hungarian child-safe animal fact. Maximum 2 sentences. No scary, violent, adult, medical, or private content.",
    )
    if generated:
        reply = _limit_sentences(str(generated.get("reply_text", "")), 3)
        source = "animal_fact_gpt"
    else:
        reply = ANIMAL_FACT_FALLBACKS[0]
        source = "animal_fact_fallback"
    return ActivityReply(
        reply_text=reply,
        activity="animal_fact",
        robot_mood="curious",
        suggested_replies=["Mondj még egyet", "Kérdezz tőlem állatosat", "Játsszunk állatos barkóbát"],
        answer_source=source,
    )


def _daily_activity(message: str, state: Any, mock_mode: bool) -> ActivityReply:
    text = normalize_hungarian(message)
    if "ejszaka" in text or "esti" in text:
        mock_reply = "Jó éjszakát! Most jöhet egy puha, nyugodt mese, vagy három lassú levegő."
        mood = "sleepy"
        suggested = ["Altató mód", "Mondj nyugtató mesét", "Jó éjszakát"]
    else:
        mock_reply = "Jó reggelt! Választhatsz: mondjak egy vidám állatos tényt, egy rövid mesét, vagy játsszunk egy gyors találósat?"
        mood = "happy"
        suggested = ["Állatos tényt kérek", "Rövid mesét kérek", "Találóst kérek"]
    generated = _maybe_gpt_activity(
        "ROBOT_DAILY_ENGINE",
        message,
        state,
        mock_mode,
        "Create a short, warm Hungarian daily routine reply for a young child.",
    )
    reply = str(generated.get("reply_text")) if generated else mock_reply
    return ActivityReply(
        reply_text=reply,
        activity="daily_routine",
        robot_mood=mood,
        suggested_replies=suggested,
        answer_source="activity_gpt" if generated else "activity_mock",
    )


def _memory_activity(message: str, state: Any) -> ActivityReply:
    allowed, reply, _memory = handle_session_memory(message, state)
    return ActivityReply(
        reply_text=reply,
        activity="teach_me_session_memory",
        activity_state={"stored": allowed},
        suggested_replies=["Mit jegyeztél meg?", "Mondj mesét", "Mit rajzoljak?"],
        robot_mood="happy" if allowed else "gentle",
        answer_source="session_memory",
    )


def _simple_activity(
    activity: ActivityName,
    message: str,
    state: Any,
    mock_mode: bool,
    *,
    mock_reply: str,
    suggested: list[str],
    mood: str = "happy",
) -> ActivityReply:
    generated = _maybe_gpt_activity(f"ROBOT_{_engine_prefix(activity)}_ENGINE", message, state, mock_mode, _prompt_for(activity))
    reply = str(generated.get("reply_text")) if generated else mock_reply
    safe, _reason = check_message_safe(reply)
    if not safe:
        reply = mock_reply
        generated = None
    return ActivityReply(
        reply_text=reply,
        activity=activity,
        suggested_replies=suggested,
        robot_mood=mood,
        answer_source="activity_gpt" if generated else "activity_mock",
    )


def _maybe_gpt_activity(env_name: str, message: str, state: Any, mock_mode: bool, task: str) -> dict[str, Any] | None:
    if os.getenv(env_name, "gpt").strip().casefold() != "gpt":
        return None
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        return None
    profile = get_profile(getattr(state, "profile_id", None))
    try:
        client = OpenAI(api_key=api_key, timeout=20)
        response = client.responses.create(
            model=openai_chat_model(),
            instructions=PERSONALITY_PROMPT,
            input=(
                f"{task}\n"
                "Return strict JSON. Use keys reply_text, suggested_replies, expected_answer, topic when useful.\n"
                f"Profile display name: {profile.display_name}\n"
                f"Session memory: {json.dumps(session_memory_snapshot(state), ensure_ascii=False)}\n"
                f"Child message: {message}"
            ),
        )
        payload = json.loads((getattr(response, "output_text", "") or "").strip())
        if not _payload_has_activity_text(payload):
            return None
        safe, _reason = check_message_safe(_payload_safety_text(payload))
        return payload if safe else None
    except Exception as exc:
        logger.exception("activity GPT failed env=%s reason=%s", env_name, exc)
        return None


def _maybe_gpt_riddle(state: Any, message: str) -> dict[str, Any] | None:
    if os.getenv("ROBOT_JOKE_ENGINE", "gpt").strip().casefold() != "gpt":
        return None
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        return None
    profile = get_profile(getattr(state, "profile_id", None))
    try:
        client = OpenAI(api_key=api_key, timeout=20)
        response = client.responses.create(
            model=openai_chat_model(),
            instructions=PERSONALITY_PROMPT,
            input=(
                "Create one short, child-safe Hungarian riddle for a young child. "
                "No scary, violent, rude, political, adult, medical, or private content. "
                "Return strict JSON only with keys: riddle_text, expected_answers, display_answer, hint, topic. "
                "expected_answers must include common synonyms where useful.\n"
                f"Profile display name: {profile.display_name}\n"
                f"Child message: {message}"
            ),
        )
        payload = json.loads((getattr(response, "output_text", "") or "").strip())
        riddle_state = _validate_riddle_payload(payload)
        riddle_state["_source"] = "gpt"
        return riddle_state
    except Exception as exc:
        logger.exception("riddle GPT failed reason=%s", exc)
        return None


def _fallback_riddle_state() -> dict[str, Any]:
    return _validate_riddle_payload(dict(RIDDLE_FALLBACKS[0]))


def _validate_riddle_payload(payload: dict[str, Any]) -> dict[str, Any]:
    riddle_text = str(payload.get("riddle_text") or payload.get("reply_text") or "").strip()
    raw_answers = payload.get("expected_answers") or payload.get("expected_answer")
    if isinstance(raw_answers, str):
        expected_answers = [raw_answers]
    elif isinstance(raw_answers, list):
        expected_answers = [str(answer).strip() for answer in raw_answers if str(answer).strip()]
    else:
        expected_answers = []
    display_answer = str(payload.get("display_answer") or (expected_answers[0] if expected_answers else "")).strip()
    hint = str(payload.get("hint") or "").strip()
    topic = str(payload.get("topic") or "general").strip()
    wrong_options_raw = payload.get("wrong_options")
    wrong_options = [str(option).strip() for option in wrong_options_raw if str(option).strip()] if isinstance(wrong_options_raw, list) else []

    if not riddle_text:
        raise ValueError("missing riddle_text")
    if not expected_answers:
        raise ValueError("missing expected_answers")
    if not display_answer:
        raise ValueError("missing display_answer")
    for value in (riddle_text, display_answer, hint):
        if value:
            safe, reason = check_message_safe(value)
            if not safe:
                raise ValueError(f"unsafe riddle text: {reason}")

    normalized_answers = sorted({_normalize_answer(answer) for answer in [*expected_answers, display_answer] if answer})
    return {
        "riddle_text": riddle_text,
        "expected_answers": expected_answers,
        "expected_answer": normalized_answers[0],
        "normalized_expected_answers": normalized_answers,
        "display_answer": display_answer,
        "hint": hint,
        "topic": topic,
        "wrong_options": wrong_options,
    }


def _riddle_suggested_replies(riddle_state: dict[str, Any]) -> list[str]:
    display_answer = str(riddle_state.get("display_answer") or "").strip()
    wrong_options = [str(option).strip() for option in riddle_state.get("wrong_options", []) if str(option).strip()]
    suggestions: list[str] = []
    if display_answer:
        suggestions.append(display_answer[:1].upper() + display_answer[1:])
    suggestions.extend(option[:1].upper() + option[1:] for option in wrong_options[:1])
    suggestions.append("Kérek másikat")
    return suggestions


def _fallback_quiz_state() -> dict[str, Any]:
    return {
        "question": "Melyik nagyobb: az elefánt vagy a nyuszi?",
        "expected_answers": ["elefánt", "elefant"],
        "expected_answer": "elefant",
        "normalized_expected_answers": ["elefant"],
        "display_answer": "elefánt",
        "wrong_options": ["nyuszi", "egér"],
        "explanation": "Az elefánt nagyobb, mint a nyuszi.",
        "topic": "animals",
        "difficulty": "easy",
    }


def _validate_quiz_payload(payload: dict[str, Any]) -> dict[str, Any]:
    question = str(payload.get("question") or payload.get("reply_text") or "").strip()
    raw_answers = payload.get("expected_answers") or payload.get("expected_answer")
    if isinstance(raw_answers, str):
        expected_answers = [raw_answers]
    elif isinstance(raw_answers, list):
        expected_answers = [str(answer).strip() for answer in raw_answers if str(answer).strip()]
    else:
        expected_answers = []
    display_answer = str(payload.get("display_answer") or (expected_answers[0] if expected_answers else "")).strip()
    explanation = str(payload.get("explanation") or "").strip()
    wrong_options_raw = payload.get("wrong_options")
    wrong_options = [str(option).strip() for option in wrong_options_raw if str(option).strip()] if isinstance(wrong_options_raw, list) else []
    topic = str(payload.get("topic") or "general").strip()
    difficulty = str(payload.get("difficulty") or "easy").strip()

    if not question:
        raise ValueError("missing question")
    if not expected_answers:
        raise ValueError("missing expected_answers")
    if not display_answer:
        raise ValueError("missing display_answer")
    if not explanation:
        raise ValueError("missing explanation")
    for value in (question, display_answer, explanation):
        safe, reason = check_message_safe(value)
        if not safe:
            raise ValueError(f"unsafe quiz text: {reason}")

    normalized_answers = sorted({_normalize_answer(answer) for answer in [*expected_answers, display_answer] if answer})
    return {
        "question": question,
        "expected_answers": expected_answers,
        "expected_answer": normalized_answers[0],
        "normalized_expected_answers": normalized_answers,
        "display_answer": display_answer,
        "wrong_options": wrong_options,
        "explanation": explanation,
        "topic": topic,
        "difficulty": difficulty,
    }


def _quiz_expected_answers(activity_state: dict[str, Any]) -> list[str]:
    raw = activity_state.get("normalized_expected_answers") or activity_state.get("expected_answers") or activity_state.get("expected_answer")
    if isinstance(raw, str):
        answers = [raw]
    elif isinstance(raw, list):
        answers = [str(answer) for answer in raw]
    else:
        answers = []
    return sorted({_normalize_answer(answer) for answer in answers if answer})


def _quiz_suggested_replies(quiz_state: dict[str, Any]) -> list[str]:
    display_answer = str(quiz_state.get("display_answer") or "").strip()
    wrong_options = [str(option).strip() for option in quiz_state.get("wrong_options", []) if str(option).strip()]
    suggestions = []
    if display_answer:
        suggestions.append(display_answer[:1].upper() + display_answer[1:])
    suggestions.extend(option[:1].upper() + option[1:] for option in wrong_options[:1])
    suggestions.append("Kérek másikat")
    return suggestions


def _normalize_answer(answer: str) -> str:
    text = normalize_hungarian(answer)
    for char in "?!.,;:\"'„”()[]{}":
        text = text.replace(char, " ")
    return " ".join(text.split())


def _answer_matches(answer: str, expected: str) -> bool:
    normalized_expected = _normalize_answer(expected)
    if not answer or not normalized_expected:
        return False
    if answer == normalized_expected or normalized_expected in answer.split():
        return True
    answer_synonyms = ANSWER_SYNONYMS.get(answer, {answer})
    expected_synonyms = ANSWER_SYNONYMS.get(normalized_expected, {normalized_expected})
    return bool(answer_synonyms & expected_synonyms)


def _is_generic_continuation(message: str) -> bool:
    text = normalize_hungarian(message)
    phrases = (
        "mondj meg egyet",
        "meg egyet",
        "kerek meg",
        "johet meg",
        "masikat kerek",
        "kerek masikat",
    )
    return any(phrase in text for phrase in phrases)


def _payload_has_activity_text(payload: dict[str, Any]) -> bool:
    return bool(
        str(payload.get("reply_text") or "").strip()
        or str(payload.get("question") or "").strip()
        or str(payload.get("explanation") or "").strip()
    )


def _payload_safety_text(payload: dict[str, Any]) -> str:
    values: list[str] = []
    for key in ("reply_text", "question", "display_answer", "explanation"):
        value = payload.get(key)
        if value:
            values.append(str(value))
    return "\n".join(values)


def _engine_prefix(activity: ActivityName) -> str:
    mapping = {
        "drawing_idea": "DRAWING",
        "creative_task": "CREATIVE",
        "joke_or_riddle": "JOKE",
        "calm_sleep_mode": "CALM",
        "daily_routine": "DAILY",
        "robot_asks_quiz": "QUIZ",
        "animal_fact": "ANIMAL_FACT",
    }
    return mapping.get(activity, "CREATIVE")


def _prompt_for(activity: ActivityName) -> str:
    prompts = {
        "drawing_idea": "Create one safe, imaginative Hungarian drawing idea for a young child.",
        "creative_task": "Create one small interactive Hungarian creative task for a young child. Ask one short follow-up question.",
        "calm_sleep_mode": "Create a calm Hungarian sleep or breathing reply for a young child. No scary imagery.",
        "daily_routine": "Create a short Hungarian daily routine reply for a young child.",
        "animal_fact": "Create one short Hungarian child-safe animal fact.",
    }
    return prompts.get(activity, "Create a short child-safe Hungarian activity reply.")


def _detect_color(message: str) -> str | None:
    text = normalize_hungarian(message)
    for normalized, display in COLORS.items():
        if normalized in text.split() or normalized in text:
            return display
    return None


def _spell_for_color(color: str) -> str:
    roots = {
        "lila": "Lila-luma",
        "kék": "Kéki-kóka",
        "sárga": "Sárga-szirma",
        "piros": "Piros-pille",
        "zöld": "Zöldi-zengő",
        "rózsaszín": "Rózsa-ragyog",
    }
    return roots.get(color, "Virág-világ")


def _clear_awaiting(state: Any) -> None:
    state.awaiting_input_type = None
    state.awaiting_input_for = None


def _limit_sentences(text: str, max_sentences: int) -> str:
    parts = [part.strip() for part in text.replace("!", ".").replace("?", ".").split(".") if part.strip()]
    if not parts:
        return text.strip()
    return ". ".join(parts[:max_sentences]) + "."
