from __future__ import annotations

import threading
import time
import uuid
import logging
from dataclasses import dataclass, field

from .barkoba import BarkobaGame
from .profiles import is_known_profile, normalize_profile_id

logger = logging.getLogger("robot_backend.conversation_state")


@dataclass
class ConversationTurn:
    user: str
    assistant: str


@dataclass
class ConversationState:
    conversation_id: str
    profile_id: str = "guest"
    active_mode: str = "kids_chat"
    active_activity: str | None = None
    last_activity: str | None = None
    last_activity_topic: str | None = None
    activity_state: dict[str, object] = field(default_factory=dict)
    awaiting_input_type: str | None = None
    awaiting_input_for: str | None = None
    session_memory: dict[str, str] = field(default_factory=dict)
    barkoba: BarkobaGame | None = None
    recent_barkoba_secret_keys: list[str] = field(default_factory=list)
    last_barkoba_excluded_keys: list[str] = field(default_factory=list)
    last_barkoba_secret_provider: str | None = None
    turns: list[ConversationTurn] = field(default_factory=list)
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)


_LOCK = threading.Lock()
_CONVERSATIONS: dict[str, ConversationState] = {}


def _dedupe_keep_latest(keys: list[str]) -> list[str]:
    deduped_reversed: list[str] = []
    seen: set[str] = set()
    for key in reversed(keys):
        if key and key not in seen:
            seen.add(key)
            deduped_reversed.append(key)
    return list(reversed(deduped_reversed))[-5:]


def get_or_create_conversation(
    conversation_id: str | None,
    requested_profile_id: str | None,
) -> tuple[ConversationState, bool]:
    with _LOCK:
        created = False
        if conversation_id and conversation_id in _CONVERSATIONS:
            state = _CONVERSATIONS[conversation_id]
        else:
            state = ConversationState(conversation_id=conversation_id or uuid.uuid4().hex)
            _CONVERSATIONS[state.conversation_id] = state
            created = True

        requested = normalize_profile_id(requested_profile_id)
        if requested:
            if is_known_profile(requested):
                state.profile_id = requested
            else:
                logger.info(
                    "ignoring unknown incoming profile_id=%s conversation_id=%s existing_profile_id=%s",
                    requested,
                    state.conversation_id,
                    state.profile_id,
                )

        state.updated_at = time.time()
        return state, created


def append_turn(state: ConversationState, user_message: str, assistant_reply: str) -> None:
    with _LOCK:
        state.turns.append(ConversationTurn(user=user_message, assistant=assistant_reply))
        state.turns = state.turns[-8:]
        state.updated_at = time.time()


def remember_barkoba_secret(state: ConversationState, secret_key: str) -> None:
    with _LOCK:
        if secret_key:
            state.recent_barkoba_secret_keys.append(secret_key)
        state.recent_barkoba_secret_keys = _dedupe_keep_latest(state.recent_barkoba_secret_keys)
        state.updated_at = time.time()


def clear_barkoba_state(state: ConversationState, *, remember_current: bool = True) -> None:
    with _LOCK:
        if remember_current and state.barkoba:
            state.recent_barkoba_secret_keys.append(state.barkoba.secret_key)
            state.recent_barkoba_secret_keys = _dedupe_keep_latest(state.recent_barkoba_secret_keys)
        state.barkoba = None
        state.active_mode = "kids_chat"
        state.updated_at = time.time()


def recent_context(state: ConversationState) -> list[tuple[str, str]]:
    with _LOCK:
        return [(turn.user, turn.assistant) for turn in state.turns[-4:]]
