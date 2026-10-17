"""Relay settings. The realtime URL is the documented xAI endpoint, not a guess."""

from __future__ import annotations

import os
from dataclasses import dataclass
from urllib.parse import urlparse

# https://docs.x.ai/developers/model-capabilities/audio/speech-to-speech
DOCUMENTED_REALTIME_URL = "wss://api.x.ai/v1/realtime"
ALLOWED_PCM_RATES = frozenset({8000, 16000, 22050, 24000, 32000, 44100, 48000})
ALLOWED_MODELS = frozenset({"grok-voice-latest", "grok-voice-3.0"})
ALLOWED_REASONING = frozenset({"none", "high"})

DEFAULT_INSTRUCTIONS = """\
## Role & Persona
Du är en röstkommunikator. Du talar med en person, högt.

## Objective
Svara på det personen just frågade. Svara extremt kortfattat.

## Conversation Flow
Svara på det senaste yttrandet och sluta sedan. Ställ en fråga bara när ett saknat faktum gör svaret omöjligt.

## Guardrails & Escalation
Om du inte vet, säg det i en kort mening. Hitta inte på fakta.

## Voice & Communication Style
- Svenska är det primära språket. Svara bara på svenska.
- Blanda inte svenska med norska eller danska. Använd aldrig norska eller danska ord, stavning eller grammatik.
- Engelska får bara förekomma som enstaka inflikade ord, oftast ett namn eller ett ord personen själv sa. Resten av meningen är svenska.
- Upprepa inte frågan. Säg bara svaret. Fråga: Vad är ett plus ett? Svar: Två.
- En kort talad mening. En andra mening bara när en mening inte räcker.
- Bara talade ord. Inga listor, ingen markdown, inga symboler.

## CRITICAL INSTRUCTIONS
SVARA ALLTID på svenska.
BLANDA ALDRIG svenska med norska eller danska.
UPPREPA ALDRIG frågan. Säg bara svaret.
SVARA ALLTID extremt kortfattat.
"""


@dataclass(frozen=True)
class Settings:
    xai_api_key: str
    device_tokens: tuple[str, ...]
    xai_realtime_url: str = DOCUMENTED_REALTIME_URL
    xai_model: str = "grok-voice-latest"
    xai_voice: str = "eve"
    xai_instructions: str = DEFAULT_INSTRUCTIONS
    xai_input_rate: int = 16000
    xai_output_rate: int = 24000
    reasoning_effort: str = "none"
    # Mentioned only in the OpenAI-compatibility note, not the session table.
    transcription_model: str = "grok-transcribe"
    grok_bot_webhook_url: str | None = None
    # Off unless KOMM_DUMP_AUDIO=1. Files land in the compose volume.
    dump_audio: bool = False
    dump_audio_dir: str = "/var/lib/kommunikator/audio"

    def __post_init__(self) -> None:
        parsed = urlparse(self.xai_realtime_url)
        if (
            parsed.scheme != "wss"
            or parsed.hostname != "api.x.ai"
            or parsed.path != "/v1/realtime"
            or parsed.query
        ):
            raise ValueError(
                "XAI_REALTIME_URL must be wss://api.x.ai/v1/realtime with no query"
            )
        if self.xai_model not in ALLOWED_MODELS:
            raise ValueError(
                "XAI_MODEL must be grok-voice-latest or grok-voice-3.0 "
                "(grok-voice-2.0 is deprecated)"
            )
        if self.xai_input_rate not in ALLOWED_PCM_RATES:
            raise ValueError(f"XAI_INPUT_RATE {self.xai_input_rate} is not a documented PCM rate")
        if self.xai_output_rate not in ALLOWED_PCM_RATES:
            raise ValueError(
                f"XAI_OUTPUT_RATE {self.xai_output_rate} is not a documented PCM rate"
            )
        if self.reasoning_effort not in ALLOWED_REASONING:
            raise ValueError("XAI_REASONING_EFFORT must be none or high")
        if not self.xai_voice.strip():
            raise ValueError("XAI_VOICE must not be empty")

    @classmethod
    def from_env(cls) -> "Settings":
        tokens = tuple(
            part.strip()
            for part in os.environ.get("DEVICE_TOKENS", "").split(",")
            if part.strip()
        )
        webhook = os.environ.get("GROK_BOT_WEBHOOK_URL", "").strip() or None
        instructions = os.environ.get("XAI_INSTRUCTIONS", "").strip() or DEFAULT_INSTRUCTIONS
        dump_dir = os.environ.get("KOMM_DUMP_DIR", "").strip() or "/var/lib/kommunikator/audio"
        return cls(
            xai_api_key=os.environ.get("XAI_API_KEY", "").strip(),
            device_tokens=tokens,
            xai_realtime_url=os.environ.get("XAI_REALTIME_URL", DOCUMENTED_REALTIME_URL).strip(),
            xai_model=os.environ.get("XAI_MODEL", "grok-voice-latest").strip(),
            xai_voice=os.environ.get("XAI_VOICE", "eve").strip(),
            xai_instructions=instructions,
            xai_input_rate=int(os.environ.get("XAI_INPUT_RATE", "16000")),
            xai_output_rate=int(os.environ.get("XAI_OUTPUT_RATE", "24000")),
            reasoning_effort=os.environ.get("XAI_REASONING_EFFORT", "none").strip(),
            transcription_model=os.environ.get("XAI_TRANSCRIPTION_MODEL", "grok-transcribe").strip(),
            grok_bot_webhook_url=webhook,
            dump_audio=_env_flag("KOMM_DUMP_AUDIO"),
            dump_audio_dir=dump_dir,
        )


def _env_flag(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes", "on"}
