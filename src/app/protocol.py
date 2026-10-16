"""Device WebSocket messages and the xAI session.update payload."""

from __future__ import annotations

import json
from dataclasses import dataclass

from .settings import ALLOWED_PCM_RATES, Settings


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class Hello:
    capture_rate: int
    playback_rate: int
    device_id: str | None


def parse_hello(payload: dict) -> Hello:
    if payload.get("type") != "hello":
        raise ProtocolError("expected hello")
    if payload.get("format") != "pcm16le":
        raise ProtocolError("format must be pcm16le")
    try:
        capture = int(payload["capture_rate"])
        playback = int(payload["playback_rate"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ProtocolError("capture_rate and playback_rate are required") from exc
    if capture not in ALLOWED_PCM_RATES or playback not in ALLOWED_PCM_RATES:
        raise ProtocolError("sample rate is not a documented PCM rate")
    device_id = payload.get("device_id")
    if device_id is not None and not isinstance(device_id, str):
        raise ProtocolError("device_id must be a string")
    return Hello(capture_rate=capture, playback_rate=playback, device_id=device_id)


def build_session_update(settings: Settings) -> dict:
    """Session shape from the Speech to Speech guide, fetched 2026-10-15.

    Swedish is not in the supported-language table. language_hint is still
    "sv": an unknown code is documented as ignored, and a rejected
    session.update comes back as an error event. turn_detection.type is null
    so PTT can commit the buffer.
    """
    return {
        "type": "session.update",
        "session": {
            "voice": settings.xai_voice,
            "instructions": settings.xai_instructions,
            "reasoning": {"effort": settings.reasoning_effort},
            "turn_detection": {"type": None},
            "resumption": {"enabled": True},
            "audio": {
                "input": {
                    "format": {"type": "audio/pcm", "rate": settings.xai_input_rate},
                    "transport": "binary",
                    "transcription": {
                        "model": settings.transcription_model,
                        "language_hint": "sv",
                    },
                },
                "output": {
                    "format": {"type": "audio/pcm", "rate": settings.xai_output_rate},
                    "transport": "binary",
                },
            },
        },
    }


def user_transcript(event: dict) -> tuple[str, bool] | None:
    """Text of the user's audio, and whether this event is the final one.

    The voice model does not read this text. grok-transcribe produces it on
    the side, which is why the screen can show one question and the spoken
    answer can be another.
    """
    kind = event.get("type")
    if kind == "conversation.item.input_audio_transcription.updated":
        return _text(event, "transcript", "text"), False
    if kind == "conversation.item.input_audio_transcription.completed":
        return _text(event, "transcript", "text"), True
    return None


def build_response_create(settings: Settings, transcript: str) -> dict:
    """Ask for a reply to the transcribed utterance, not an earlier one.

    response.create instructions replace the session prompt for this turn
    only, so the session text is repeated and the quote is appended.
    """
    instructions = settings.xai_instructions
    quote = transcript.strip()
    if quote:
        instructions += (
            "\n\n## Latest utterance\n"
            "The user just said, verbatim: "
            + json.dumps(quote, ensure_ascii=False)
            + "\nAnswer only that utterance. Do not answer an earlier question.\n"
        )
    return {"type": "response.create", "response": {"instructions": instructions}}


def map_server_event(event: dict) -> list[dict]:
    """Map documented xAI server events onto device messages.

    Transcript property names are not spelled out in the fetched reference,
    so both `delta` and `transcript` are accepted. See DESIGN.md TODO.
    """
    kind = event.get("type")
    if kind == "conversation.item.input_audio_transcription.updated":
        return [
            {
                "type": "transcript.user",
                "text": _text(event, "transcript", "text"),
                "final": False,
                "cumulative": True,
            }
        ]
    if kind == "conversation.item.input_audio_transcription.completed":
        return [
            {
                "type": "transcript.user",
                "text": _text(event, "transcript", "text"),
                "final": True,
                "cumulative": True,
            }
        ]
    if kind == "response.output_audio_transcript.delta":
        return [
            {
                "type": "transcript.assistant",
                "text": _text(event, "delta", "transcript", "text"),
                "final": False,
                "cumulative": False,
            }
        ]
    if kind == "response.output_audio_transcript.done":
        return [
            {
                "type": "transcript.assistant",
                "text": _text(event, "transcript", "text"),
                "final": True,
                "cumulative": True,
            }
        ]
    if kind == "response.created":
        return [{"type": "status", "state": "thinking"}]
    if kind == "response.done":
        return [{"type": "status", "state": "idle"}]
    if kind == "error":
        err = event.get("error") if isinstance(event.get("error"), dict) else {}
        return [
            {
                "type": "error",
                "code": str(err.get("code", "error")),
                "message": str(err.get("message", "")),
            }
        ]
    return []


def _text(event: dict, *keys: str) -> str:
    for key in keys:
        value = event.get(key)
        if isinstance(value, str):
            return value
    return ""
