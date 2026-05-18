from pydantic import BaseModel, Field


class RobotChatRequest(BaseModel):
    conversation_id: str | None = Field(default=None, max_length=80)
    profile_id: str | None = Field(default=None, max_length=40)
    device_id: str = Field(default="robot_assistant_esp32s3", max_length=64)
    locale: str = Field(default="hu-HU", max_length=16)
    mode: str = Field(default="kids_chat", max_length=32)
    message: str = Field(min_length=1, max_length=1200)
    max_answer_seconds: int = Field(default=30, ge=3, le=180)
    tts: bool | None = True


class RobotChatResponse(BaseModel):
    ok: bool = True
    safe: bool = True
    transcript: str | None = None
    conversation_id: str
    profile_id: str | None = None
    reply_text: str
    reply_audio_url: str | None = None
    robot_mood: str = "friendly"
    robot_action: str = "speak"
    blocked_reason: str | None = None
    active_mode: str = "kids_chat"
    answer_source: str | None = None
    openai_chat_model: str | None = None
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
