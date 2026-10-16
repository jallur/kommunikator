import json

import pytest

from app.memory import ConversationMemory
from app.protocol import (
    ProtocolError,
    build_response_create,
    build_session_update,
    map_server_event,
    parse_hello,
)
from app.settings import Settings
from app.xai import realtime_url


def test_hello_requires_documented_rates() -> None:
    hello = parse_hello(
        {
            "type": "hello",
            "capture_rate": 16000,
            "playback_rate": 16000,
            "format": "pcm16le",
            "device_id": "stick",
        }
    )
    assert hello.capture_rate == 16000
    with pytest.raises(ProtocolError):
        parse_hello(
            {
                "type": "hello",
                "capture_rate": 11025,
                "playback_rate": 16000,
                "format": "pcm16le",
            }
        )


def test_session_update_matches_documented_shape() -> None:
    settings = Settings(xai_api_key="secret-key", device_tokens=("t",))
    payload = build_session_update(settings)
    assert payload["type"] == "session.update"
    session = payload["session"]
    assert session["voice"] == "eve"
    assert session["turn_detection"] == {"type": None}
    assert session["reasoning"] == {"effort": "none"}
    assert session["resumption"] == {"enabled": True}
    assert session["audio"]["input"]["format"] == {"type": "audio/pcm", "rate": 16000}
    assert session["audio"]["input"]["transport"] == "binary"
    assert session["audio"]["output"]["format"] == {"type": "audio/pcm", "rate": 24000}
    assert session["audio"]["output"]["transport"] == "binary"
    assert session["audio"]["input"]["transcription"]["model"] == "grok-transcribe"
    assert session["audio"]["input"]["transcription"]["language_hint"] == "sv"
    blob = json.dumps(payload)
    assert "secret-key" not in blob
    assert "svenska" in session["instructions"].lower()
    assert "norska" in session["instructions"]
    assert "danska" in session["instructions"]
    assert "extremt kortfattat" in session["instructions"]
    assert "Upprepa inte frågan" in session["instructions"]
    assert "Svar: Två." in session["instructions"]


def test_response_create_quotes_the_user_transcript() -> None:
    settings = Settings(xai_api_key="secret-key", device_tokens=("t",))
    payload = build_response_create(settings, 'Vad är 5 plus 5?')
    text = payload["response"]["instructions"]
    assert "Vad är 5 plus 5?" in text
    assert "Svara bara på det" in text
    assert "Upprepa inte frågan" in text
    assert "extremt kortfattat" in text
    bare = build_response_create(settings, "  ")
    assert "Senaste yttrandet" not in bare["response"]["instructions"]


def test_realtime_url_is_the_documented_endpoint() -> None:
    settings = Settings(xai_api_key="k", device_tokens=("t",))
    assert (
        realtime_url(settings)
        == "wss://api.x.ai/v1/realtime?model=grok-voice-latest&reasoning.effort=none"
    )
    assert realtime_url(settings, "conv_1").endswith("&conversation_id=conv_1")


def test_event_mapping() -> None:
    user = map_server_event(
        {
            "type": "conversation.item.input_audio_transcription.updated",
            "transcript": "hej",
        }
    )
    assert user[0]["cumulative"] is True
    assert user[0]["final"] is False
    assistant = map_server_event(
        {"type": "response.output_audio_transcript.delta", "delta": "ok"}
    )
    assert assistant[0]["text"] == "ok"
    assert assistant[0]["cumulative"] is False
    assert map_server_event({"type": "response.done"}) == [{"type": "status", "state": "idle"}]
    err = map_server_event({"type": "error", "error": {"code": "bad", "message": "nej"}})
    assert err[0]["code"] == "bad"


def test_conversation_memory_expires() -> None:
    now = {"t": 0.0}
    memory = ConversationMemory(ttl_s=30, clock=lambda: now["t"])
    memory.put("token", "conv_1")
    assert memory.get("token") == "conv_1"
    now["t"] = 31
    assert memory.get("token") is None
