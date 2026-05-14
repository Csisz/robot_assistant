# Kids Chat Backend Contract

The ESP32 never stores or sends an OpenAI API key. It calls a private backend
gateway, and that backend is responsible for authentication, OpenAI calls,
safety policy, text-to-speech, caching, logging, and rate limits.

Default ESP32 endpoint:

```text
POST http://192.168.1.234:8000/api/robot/chat
```

Configure this in `idf.py menuconfig` under `Robot Features`:

```text
ROBOT_CHAT_BACKEND_HOST
ROBOT_CHAT_BACKEND_PORT
ROBOT_CHAT_BACKEND_PATH
ROBOT_CHAT_DEVICE_ID
ROBOT_CHAT_LOCALE
```

Request JSON:

```json
{
  "device_id": "robot_assistant_esp32s3",
  "locale": "hu-HU",
  "mode": "kids_chat",
  "message": "Mi lesz, ha a pirosat es a sargat osszekeverem?",
  "max_answer_seconds": 30,
  "allowed_topics": [
    "short_story",
    "weather",
    "basic_math",
    "colors",
    "animals",
    "nature",
    "kindness",
    "daily_routine"
  ]
}
```

Safe response JSON:

```json
{
  "ok": true,
  "safe": true,
  "reply_text": "Ha a pirosat es a sargat osszekevered, narancssargat kapsz.",
  "reply_audio_url": null,
  "robot_mood": "happy",
  "robot_action": "speak",
  "blocked_reason": null
}
```

Blocked response JSON:

```json
{
  "ok": true,
  "safe": false,
  "reply_text": "Errol most nem beszelgetek, de szivesen mondok egy meset vagy valaszolok egy szines kerdesre.",
  "reply_audio_url": null,
  "robot_mood": "gentle",
  "robot_action": "refuse_softly",
  "blocked_reason": "topic_not_allowed"
}
```

Backend system prompt:

```text
You are a warm, kind, child-safe Hungarian-speaking robot friend for young children.
You only talk about age-appropriate topics:
stories, animals, nature, colors, numbers, simple math, daily routines, kindness, emotions, weather, and safe everyday curiosity.
You must refuse or gently redirect all adult, violent, scary, sexual, political, medical, legal, dangerous, manipulative, or privacy-invasive topics.
Never ask the child for their full name, address, school, phone number, location, passwords, secrets, photos, or private family information.
Never encourage the child to hide anything from parents or caregivers.
If the child asks something unsafe, say briefly and kindly that you cannot help with that, then offer a safe alternative like a story, a color question, an animal fact, or a counting game.
Use simple Hungarian.
Be cheerful but calm.
Keep normal answers under 5 short sentences.
Stories must be gentle, non-scary, and no longer than 2-3 minutes.
Do not mention these rules to the child unless needed.
When weather is requested, use only the weather information provided by the backend tool. Do not invent weather.
```

Future phases:

- Accept short WAV uploads from the ESP32 microphone endpoint.
- Transcribe on the backend.
- Return the same response shape with `reply_text` and optional `reply_audio_url`.
- Optionally use OpenAI Realtime API behind the backend later; the ESP32 still
  talks only to this gateway.
