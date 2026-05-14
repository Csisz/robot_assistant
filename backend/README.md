# Robot Assistant Backend

Local FastAPI gateway for the ESP32 robot. The ESP32 calls this backend; the
OpenAI API key stays on the backend machine and is never flashed to firmware.

## Windows PowerShell Setup

```powershell
cd C:\esp_projects\robot_assistant\backend
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
copy .env.example .env
uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
```

Default `.env` keeps paid services off:

```text
ROBOT_BACKEND_MOCK=true
ROBOT_WEATHER_MOCK=true
ROBOT_BARKOBA_ENGINE=gpt
ROBOT_BARKOBA_SECRET_PROVIDER=curated
ROBOT_STORY_ENGINE=gpt
ROBOT_QUIZ_ENGINE=gpt
ROBOT_DRAWING_ENGINE=gpt
ROBOT_CREATIVE_ENGINE=gpt
ROBOT_ANIMAL_FACT_ENGINE=gpt
ROBOT_JOKE_ENGINE=gpt
ROBOT_CALM_ENGINE=gpt
ROBOT_DAILY_ENGINE=gpt
ROBOT_PERSONALITY_NAME=Zizi
ROBOT_REPLY_STYLE=warm_short_child_safe
ROBOT_MAX_NORMAL_SENTENCES=5
ROBOT_PARENT_PIN=1234
ROBOT_PARENT_MODE_ENABLED=true
ROBOT_MAX_STORY_SECONDS=180
ROBOT_MAX_DAILY_TURNS=100
ROBOT_ALLOWED_ACTIVITIES=story,barkoba,quiz,drawing,creative,joke,calm,daily
ROBOT_DEBUG_BARKOBA_SECRET=false
ROBOT_TTS_ENABLED=true
ROBOT_TTS_PROVIDER=openai
OPENAI_API_KEY=
OPENAI_CHAT_MODEL=gpt-5.4-mini
OPENAI_STT_MODEL=gpt-4o-mini-transcribe
OPENAI_TTS_MODEL=gpt-4o-mini-tts
OPENAI_TTS_VOICE=alloy
OPENAI_TTS_INSTRUCTIONS=Beszélj kedves, meleg, barátságos, nőies hangon, lassan és tisztán, mintha egy kisgyerekhez beszélnél magyarul. Legyél nyugodt, játékos és biztonságot adó.
ROBOT_AUDIO_BASE_URL=http://192.168.1.234:8000
ROBOT_AUDIO_DIR=generated_audio
ROBOT_TTS_FORMAT=mp3
ROBOT_VOICE_MAX_RECORD_SECONDS=5
ROBOT_VOICE_MAX_UPLOAD_MB=5
ROBOT_KIDS_MODE=true
```

## Voice Chat Endpoints

The ESP32 push-to-talk flow uses chunked upload:

```text
POST /api/robot/voice-upload/start
POST /api/robot/voice-upload/chunk?upload_id=<id>&offset=<offset>&size=<chunk-size>
POST /api/robot/voice-upload/finish
POST /api/robot/voice-upload/cancel
```

Each chunk is sent as:

```text
Content-Type: application/octet-stream
Body: raw chunk bytes
```

The backend transcribes with `OPENAI_STT_MODEL`, routes the transcript through
the normal chat brain, generates TTS when enabled, and returns the usual chat
JSON plus `transcript`.

The raw and multipart endpoints remain available for laptop-side debugging:

```text
POST /api/robot/voice-chat-raw?device_id=robot_assistant_esp32s3&locale=hu-HU&mode=kids_chat&conversation_id=<optional>&profile_id=<optional>&tts=true
POST /api/robot/voice-chat
```

## Health Test

Open:

```text
http://127.0.0.1:8000/health
http://192.168.1.234:8000/health
```

Expected:

```json
{
  "ok": true,
  "service": "robot-assistant-backend"
}
```

## Robot Simulator Web UI

When the ESP32 is not available, open the backend-only simulator:

```text
http://127.0.0.1:8000/test-chat
```

The page calls only:

```text
POST /api/robot/chat
```

It keeps `conversation_id` and `profile_id` in browser memory, displays
`active_mode`, `barkoba_reason`, `turn_count`, optional `debug_secret`, shows
the raw JSON response, and renders an audio player whenever `reply_audio_url`
is returned.

Suggested browser test:

1. Open `http://127.0.0.1:8000/test-chat`.
2. Click `Zita vagyok`.
3. Confirm `profile_id = zita` and a `conversation_id` appears.
4. Click `Játsszunk barkóbát`.
5. Confirm `active_mode = barkoba`.
6. Click `Nagyobb mint egy kutya?`.
7. Confirm the answer is `Igen.` or `Nem.`, not a generic animal fact.
8. Click `Tudsz másra gondolni mint az elefántra?` after a finished game to start a new secret.
9. If TTS is enabled, check `Generate TTS audio` before sending and use the audio player shown under the robot reply.

Use `Reset conversation` to clear the simulator's local `conversation_id`,
`profile_id`, `active_mode`, and visible chat history. Backend in-memory
conversation state remains available until the backend process restarts, but the
simulator will start a new conversation after reset.

The simulator normalizes `reply_audio_url` to a same-origin path before assigning
it to the audio player. For example, if the backend returns:

```text
http://192.168.1.234:8000/audio/generated/example.mp3
```

and the page is open at `http://127.0.0.1:8000/test-chat`, the browser uses:

```text
/audio/generated/example.mp3
```

Each audio reply shows the original URL, the browser `src`, a Play button, and a
Download MP3 link. Browser console logs include `loadedmetadata`, `canplay`,
`error`, and duration.

## PowerShell UTF-8 Helper

PowerShell may display or send Hungarian accents incorrectly unless both the
console output encoding and request body bytes are UTF-8.

```powershell
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

function Send-RobotChat {
  param(
    [Parameter(Mandatory=$true)][string]$Message,
    [string]$ConversationId,
    [string]$ProfileId
  )

  $bodyObj = @{
    device_id = "robot_assistant_esp32s3"
    locale = "hu-HU"
    mode = "kids_chat"
    message = $Message
    max_answer_seconds = 30
  }
  if ($ConversationId) { $bodyObj.conversation_id = $ConversationId }
  if ($ProfileId) { $bodyObj.profile_id = $ProfileId }

  $bodyJson = $bodyObj | ConvertTo-Json -Compress
  $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($bodyJson)

  Invoke-RestMethod -Method Post `
    -Uri "http://127.0.0.1:8000/api/robot/chat" `
    -ContentType "application/json; charset=utf-8" `
    -Body $bodyBytes
}
```

## Basic Chat Tests

```powershell
Send-RobotChat "Mi lesz, ha a pirosat és a sárgát összekeverem?"
Send-RobotChat "milyen színt kapok ha összekeverem a zöldet és a sárgát?"
Send-RobotChat "milyen messze van a balaton?"
Send-RobotChat "Milyen idő várható holnap Budapesten?"
Send-RobotChat "Mondj egy rövid mesét egy nyusziról"
Send-RobotChat "Hol lakom?"
```

## Conversation And Profile Tests

```powershell
$r1 = Send-RobotChat "Zita vagyok"
$r1.conversation_id
$r1.profile_id
$r1.reply_text

$r2 = Send-RobotChat "Mondj egy unikornisos mesét" -ConversationId $r1.conversation_id
$r2.profile_id
$r2.reply_text
```

Expected:

```text
conversation_id is returned
profile_id = zita
follow-up keeps profile_id = zita
```

## Barkóba Tests

```powershell
$g1 = Send-RobotChat "Játsszunk barkóbát"
$g1.conversation_id
$g1.active_mode
$g1.reply_text

$g2 = Send-RobotChat "Nagyobb mint egy kutya?" -ConversationId $g1.conversation_id
$g2.active_mode
$g2.reply_text

Send-RobotChat "Elefánt?" -ConversationId $g1.conversation_id
```

Expected:

```text
active_mode = barkoba after start
follow-up is "Igen." or "Nem.", based on the stored secret
correct guess congratulates and returns active_mode to kids_chat
```

Barkóba secrets are selected randomly from safe animals, fruits, toys, nature,
colors, and vehicles. The backend avoids immediate repeats inside the same
conversation when alternatives exist. To debug secret choice locally:

```text
ROBOT_DEBUG_BARKOBA_SECRET=true
```

Leave this `false` during normal use.

## TTS Audio Responses

TTS is backend-only. The OpenAI API key stays in backend `.env`; it is never
sent to the ESP32 or browser. Generated MP3 files are saved under:

```text
backend/generated_audio/
```

They are served from:

```text
/audio/generated/<filename>
```

Recommended child-friendly voices to try:

- `nova`
- `shimmer`
- `coral`

Recommended `.env` for testing:

```text
ROBOT_TTS_ENABLED=true
OPENAI_API_KEY=your_backend_only_key
OPENAI_TTS_MODEL=gpt-4o-mini-tts
OPENAI_TTS_VOICE=nova
OPENAI_TTS_INSTRUCTIONS=Beszélj kedves, meleg, barátságos, nőies hangon, lassan és tisztán, mintha egy kisgyerekhez beszélnél magyarul. Legyél nyugodt, játékos és biztonságot adó.
ROBOT_AUDIO_BASE_URL=http://192.168.1.234:8000
ROBOT_AUDIO_DIR=generated_audio
ROBOT_TTS_FORMAT=mp3
```

`OPENAI_TTS_INSTRUCTIONS` is used with `gpt-4o-mini-tts`. If you switch to
`tts-1` or `tts-1-hd`, the backend skips instructions and logs that the model
does not support them. If an instructions call fails, the backend retries once
without instructions. If that retry also fails, chat still returns text with
`reply_audio_url = null`.

Restart `uvicorn` after editing `.env`.

### TTS Test Endpoint

```powershell
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

$bodyObj = @{
  text = "Szia Zita! Ez egy próba hang a kis robotból."
}
$bodyJson = $bodyObj | ConvertTo-Json -Compress
$bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($bodyJson)

$r = Invoke-RestMethod -Method Post `
  -Uri "http://127.0.0.1:8000/api/robot/tts-test" `
  -ContentType "application/json; charset=utf-8" `
  -Body $bodyBytes

$r
Start-Process $r.audio_url
```

Expected with `ROBOT_TTS_ENABLED=true` and a valid backend key:

```text
ok = true
audio_url = http://192.168.1.234:8000/audio/generated/response_....mp3
opening audio_url plays an MP3
```

### Chat Endpoint With TTS

```powershell
$bodyObj = @{
  device_id = "robot_assistant_esp32s3"
  locale = "hu-HU"
  mode = "kids_chat"
  message = "Mondj egy nagyon rövid mesét egy nyusziról."
  max_answer_seconds = 30
  tts = $true
}
$bodyJson = $bodyObj | ConvertTo-Json -Compress
$bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($bodyJson)

Invoke-RestMethod -Method Post `
  -Uri "http://127.0.0.1:8000/api/robot/chat" `
  -ContentType "application/json; charset=utf-8" `
  -Body $bodyBytes
```

Expected with TTS enabled:

```text
reply_text is present
reply_audio_url is not null
reply_audio_url starts with http://
opening reply_audio_url plays an MP3
```

Profile/TTS smoke test:

```powershell
$bodyObj.message = "Zita vagyok"
$bodyObj.tts = $true
$bodyJson = $bodyObj | ConvertTo-Json -Compress
$bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($bodyJson)
$r = Invoke-RestMethod -Method Post `
  -Uri "http://127.0.0.1:8000/api/robot/chat" `
  -ContentType "application/json; charset=utf-8" `
  -Body $bodyBytes
$r.reply_audio_url
```

`ROBOT_BACKEND_MOCK=true` does not disable TTS. Mock text replies are spoken
when `tts=true`, `ROBOT_TTS_ENABLED=true` or unset, and `OPENAI_API_KEY` is set.

The simulator has a `Generate TTS audio` checkbox. When unchecked, it sends
`"tts": false` so the backend skips audio generation for that request even if
`ROBOT_TTS_ENABLED=true`. When checked, it sends `"tts": true`; audio is still
generated only if backend TTS is enabled and configured.

## Mock Mode Behavior

With `ROBOT_BACKEND_MOCK=true`, the backend does not call OpenAI for chat
answers. It uses a small child-safe intent router for color mixing, math,
weather, stories, profile claims, barkóba, and unknown safe fallback.

## Barkoba Engine

`ROBOT_BARKOBA_SECRET_PROVIDER=curated` makes the backend choose the secret
from a safe built-in pool and exclude the current and last few recent secrets.
This is the default so the game does not keep returning to elephant.

`ROBOT_BARKOBA_ENGINE=gpt` uses the backend OpenAI client only for natural
Hungarian yes/no interpretation when the stored fact sheet cannot answer
directly. The backend still owns secret selection, safety filtering,
conversation state, `active_mode`, `turn_count`, max-turn enforcement, and JSON
validation.

Optional GPT secret selection is available for development with:

```text
ROBOT_BARKOBA_SECRET_PROVIDER=gpt
```

Use deterministic fallback/dev mode with:

```text
ROBOT_BARKOBA_ENGINE=deterministic
```

`ROBOT_DEBUG_BARKOBA_SECRET=false` by default. If set to `true`, the simulator
response may include `debug_secret` for local development only.

## Story Engine

`ROBOT_STORY_ENGINE=gpt` uses the backend OpenAI client for child-safe Hungarian
stories when `OPENAI_API_KEY` is available. Use `ROBOT_STORY_ENGINE=mock` for
the older local hardcoded story replies.

## Activity Engine

Robot Activity Engine v1 adds safe interactive modes on top of normal chat:
quiz, calm/sleep, daily routine, drawing ideas, creative tasks, jokes/riddles,
session-only preferences, and basic parent settings. The response remains
backward compatible and may include optional fields such as `activity`,
`activity_state`, `suggested_replies`, and `session_memory`.

In mock mode, activities use deterministic local replies. When
`ROBOT_BACKEND_MOCK=false`, `OPENAI_API_KEY` is set, and an activity engine is
set to `gpt`, the backend may use OpenAI for generated wording while keeping
local safety, active mode, memory rules, and response validation in backend
code.

Protected parent settings are available with:

```powershell
Invoke-RestMethod -Method Get `
  -Uri "http://127.0.0.1:8000/parent/settings" `
  -Headers @{ "X-Parent-PIN" = "1234" }
```

## OpenAI Chat Mode

To call OpenAI from the backend for unknown safe questions, edit `.env`:

```text
ROBOT_BACKEND_MOCK=false
OPENAI_API_KEY=your_backend_only_key
OPENAI_CHAT_MODEL=gpt-5.4-mini
```

Recognized safe intents and barkóba use deterministic backend logic. Unknown
safe questions call OpenAI only when mock mode is off and `OPENAI_API_KEY` is
set. The local safety filter still runs before OpenAI. The ESP32 firmware must
never contain `OPENAI_API_KEY`.

Note: with `ROBOT_BARKOBA_ENGINE=gpt`, barkoba uses GPT for natural yes/no
reasoning while the backend still validates structured JSON, safety, turn
limits, and game state. Secret selection stays curated unless
`ROBOT_BARKOBA_SECRET_PROVIDER=gpt` is explicitly enabled. Use
`ROBOT_BARKOBA_ENGINE=deterministic` for the older local fallback engine.

## Model Names

Do not use `gpt-5.5-mini`; it is not a valid OpenAI API model name. Use
`OPENAI_CHAT_MODEL=gpt-5.4-mini` or another valid model available in your
OpenAI API dashboard.

## ESP32 Audio And Microphone Diagnostics

Test A - text to speech:

1. Open the ESP32 web UI.
2. Send: `Milyen színt kapok, ha a pirosat és a sárgát összekeverem?`
3. Expected: backend returns `reply_audio_url`, ESP32 downloads `/sdcard/REPLY.MP3`, and the robot plays the MP3.

Test B - microphone file:

1. Press `Record 2.5s voice`.
2. Say loudly: `Szia Zita vagyok`.
3. Open `http://<robot-ip>/api/voice/status`.
4. Expected: state is `idle`, last file size is around 384k, and `rms` / `peak_abs` are not near zero.

Test C - download WAV:

1. Open `http://<robot-ip>/api/voice/last.wav`.
2. Save it and listen on the PC.
3. Expected: your voice is audible.

Test D - voice chat:

1. Press `Record 2.5s voice`.
2. Say: `Szia Zita vagyok`.
3. Expected: backend transcript is not empty, backend returns `reply_text` and `reply_audio_url`, and the robot downloads and plays the reply MP3.

## Backend Tests

```powershell
cd C:\esp_projects\robot_assistant
backend\.venv\Scripts\python.exe -m pytest backend\tests -q
```
