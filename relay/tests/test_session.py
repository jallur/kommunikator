import json

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.main import create_app
from tests.fakes import FakeFactory

HELLO = {
    "type": "hello",
    "capture_rate": 16000,
    "playback_rate": 16000,
    "format": "pcm16le",
    "device_id": "stickc-plus2",
}


def _collect(ws, limit: int = 12) -> list[dict]:
    found = []
    for _ in range(limit):
        message = ws.receive()
        found.append(message)
        text = message.get("text")
        if text and json.loads(text).get("type") == "status":
            if json.loads(text).get("state") == "idle":
                break
    return found


def test_bad_token_never_opens_xai(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect("/v1/session?token=wrong") as ws:
        body = ws.receive_json()
        assert body["code"] == "unauthorized"
        with pytest.raises(WebSocketDisconnect) as caught:
            ws.receive_json()
    assert caught.value.code == 1008
    assert factory.links == []


def test_header_token_streams_audio_and_text(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    pcm = (b"\x00\x01" * 160)
    with client.websocket_connect(
        "/v1/session",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        ready = ws.receive_json()
        assert ready["type"] == "hello.ok"
        assert ready["playback_rate"] == 16000
        assert ready["conversation_id"] == "conv_test"
        ws.send_json({"type": "ptt", "state": "down"})
        ws.send_bytes(pcm)
        ws.send_json({"type": "ptt", "state": "up"})
        messages = _collect(ws)

    assert factory.urls == [
        "wss://api.x.ai/v1/realtime?model=grok-voice-latest&reasoning.effort=none"
    ]
    assert factory.keys == ["xai-test-key"]
    sent = factory.links[0].sent
    texts = [json.loads(item) for item in sent if isinstance(item, str)]
    assert texts[0]["type"] == "session.update"
    assert any(isinstance(item, bytes) and item for item in sent)
    assert {"type": "input_audio_buffer.commit"} in texts
    assert {"type": "response.create"} in texts
    texts = [message["text"] for message in messages if message.get("text")]
    assert any("Hej" in text for text in texts)
    assert any(message.get("bytes") for message in messages)


def test_empty_ptt_does_not_commit(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect(
        "/v1/session",
        headers={"Authorization": "Bearer device-token-b"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down"})
        ws.send_json({"type": "ptt", "state": "up"})

    texts = [json.loads(item) for item in factory.links[0].sent if isinstance(item, str)]
    assert not any(item.get("type") == "input_audio_buffer.commit" for item in texts)


def test_missing_api_key_fails_closed(settings) -> None:
    from dataclasses import replace

    factory = FakeFactory()
    client = TestClient(create_app(replace(settings, xai_api_key=""), factory))
    with client.websocket_connect(
        "/v1/session",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        body = ws.receive_json()
    assert body["code"] == "relay_not_configured"
    assert factory.links == []
