from __future__ import annotations

import json
import logging
import os
import urllib.parse
import urllib.request

from .kids_safety import normalize_hungarian

logger = logging.getLogger("robot_backend.weather")

MOCK_WEATHER_REPLY = (
    "Holnap Budapesten változékony időre számíthatsz. "
    "Érdemes rétegesen öltözni, és indulás előtt kérj meg egy felnőttet, "
    "hogy nézze meg az aktuális előrejelzést is."
)

WEATHER_FALLBACK_REPLY = (
    "Most nem tudom biztosan megnézni az időjárást, de indulás előtt kérjetek meg "
    "egy felnőttet, hogy ellenőrizze az előrejelzést."
)


def weather_mock_enabled() -> bool:
    return os.getenv("ROBOT_WEATHER_MOCK", "true").strip().casefold() == "true"


def get_weather_reply(message: str) -> tuple[str, str]:
    if weather_mock_enabled():
        return MOCK_WEATHER_REPLY, "weather_mock"

    try:
        return _get_open_meteo_reply(message), "weather"
    except Exception:
        logger.exception("Open-Meteo weather lookup failed")
        return WEATHER_FALLBACK_REPLY, "fallback"


def _get_open_meteo_reply(message: str) -> str:
    text = normalize_hungarian(message)
    day_index = 1 if "holnap" in text else 0
    label = "Holnap" if day_index == 1 else "Ma"
    params = urllib.parse.urlencode(
        {
            "latitude": "47.4979",
            "longitude": "19.0402",
            "daily": "temperature_2m_max,temperature_2m_min,precipitation_probability_max",
            "timezone": "Europe/Budapest",
            "forecast_days": "2",
        }
    )
    url = f"https://api.open-meteo.com/v1/forecast?{params}"
    with urllib.request.urlopen(url, timeout=8) as response:
        payload = json.loads(response.read().decode("utf-8"))

    daily = payload["daily"]
    temp_min = round(float(daily["temperature_2m_min"][day_index]))
    temp_max = round(float(daily["temperature_2m_max"][day_index]))
    rain = int(daily.get("precipitation_probability_max", [0, 0])[day_index] or 0)

    if rain >= 60:
        rain_text = "eső is lehet, ezért jól jöhet egy esőkabát"
    elif rain >= 30:
        rain_text = "néha lehetnek felhők, ezért érdemes indulás előtt ránézni az előrejelzésre"
    else:
        rain_text = "kevés eső valószínű"

    return (
        f"{label} Budapesten körülbelül {temp_min} és {temp_max} fok között lehet a hőmérséklet, "
        f"és {rain_text}. Kérj meg egy felnőttet, hogy indulás előtt ellenőrizze a friss előrejelzést is."
    )
