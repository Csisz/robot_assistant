# Robot Assistant Backend

Camera-free ChatGPT companion backend for the ESP32-S3 MVP.

## Start

```powershell
cd C:\esp_projects\robot_assistant
python -m uvicorn backend.app.main:app --host 0.0.0.0 --port 8000
```

Useful environment:

```powershell
$env:OPENAI_API_KEY="..."
$env:ROBOT_AUDIO_BASE_URL="http://<backend-ip>:8000"
$env:ROBOT_TTS_ENABLED="true"
$env:OPENAI_CHAT_MODEL="gpt-5.4-mini"
$env:OPENAI_TTS_MODEL="gpt-4o-mini-tts"
$env:OPENAI_TTS_VOICE="nova"
```

Set `ROBOT_BACKEND_MOCK=true` for offline text-only mock replies.

## API

`POST /api/robot/chat`

```json
{
  "message": "Szia, Zita vagyok",
  "conversation_id": null,
  "tts": true
}
```

Always returns the MVP fields:

```json
{
  "ok": true,
  "conversation_id": "...",
  "reply_text": "...",
  "reply_audio_url": "http://<backend-ip>:8000/audio/generated/response_....mp3",
  "active_mode": "companion"
}
```

When `tts=true`, OpenAI TTS saves a temporary MP3 under `backend/generated_audio` and returns a downloadable URL.
