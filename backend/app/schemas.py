from typing import Literal
from typing import Any

from pydantic import BaseModel, Field


class RobotChatRequest(BaseModel):
    conversation_id: str | None = Field(default=None, max_length=80)
    profile_id: str | None = Field(default=None, max_length=40)
    device_id: str = Field(default="robot_assistant_esp32s3", max_length=64)
    locale: str = Field(default="hu-HU", max_length=16)
    mode: Literal["kids_chat"] = "kids_chat"
    message: str = Field(min_length=1, max_length=1200)
    max_answer_seconds: int = Field(default=30, ge=3, le=180)
    allowed_topics: list[str] = Field(default_factory=list)
    tts: bool | None = None


class RobotChatResponse(BaseModel):
    ok: bool = True
    safe: bool
    transcript: str | None = None
    conversation_id: str | None = None
    profile_id: str | None = None
    reply_text: str
    reply_audio_url: str | None = None
    robot_mood: str
    robot_action: str
    blocked_reason: str | None = None
    active_mode: Literal["kids_chat", "barkoba", "quiz", "riddle", "creative_task"] = "kids_chat"
    activity: str | None = None
    last_activity: str | None = None
    last_activity_topic: str | None = None
    activity_state: dict[str, Any] | None = None
    awaiting_input_type: str | None = None
    awaiting_input_for: str | None = None
    suggested_replies: list[str] | None = None
    session_memory: dict[str, str] | None = None
    barkoba_reason: str | None = None
    turn_count: int | None = None
    debug_secret: str | None = None
    barkoba_category: str | None = None
    debug_barkoba_facts: dict[str, Any] | None = None
    recent_barkoba_secrets: list[str] | None = None
    barkoba_excluded_secrets: list[str] | None = None
    barkoba_secret_provider: Literal["curated", "gpt"] | None = None
    answer_source: str | None = None
    openai_chat_model: str | None = None
    barkoba_engine: Literal["gpt", "deterministic"] | None = None
    backend_mock: bool | None = None


class HealthResponse(BaseModel):
    ok: bool = True
    service: str = "robot-assistant-backend"


class TtsTestRequest(BaseModel):
    text: str = Field(min_length=1, max_length=2000)


class TtsTestResponse(BaseModel):
    ok: bool
    audio_url: str | None = None
    error: str | None = None


class VoiceUploadStartRequest(BaseModel):
    device_id: str = Field(default="robot_assistant_esp32s3", max_length=64)
    locale: str = Field(default="hu-HU", max_length=16)
    mode: str = Field(default="kids_chat", max_length=32)
    conversation_id: str | None = Field(default=None, max_length=80)
    profile_id: str | None = Field(default=None, max_length=40)
    tts: bool | None = True
    file_size: int = Field(gt=44, le=25 * 1024 * 1024)


class VoiceUploadStartResponse(BaseModel):
    ok: bool
    upload_id: str


class VoiceUploadFinishRequest(BaseModel):
    upload_id: str = Field(min_length=1, max_length=80)


class VoiceUploadCancelRequest(BaseModel):
    upload_id: str = Field(min_length=1, max_length=80)
