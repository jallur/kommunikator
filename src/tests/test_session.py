import json
import logging

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
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer wrong"},
    ) as ws:
        body = ws.receive_json()
        assert body["code"] == "unauthorized"
        with pytest.raises(WebSocketDisconnect) as caught:
            ws.receive_json()
    assert caught.value.code == 1008
    assert factory.links == []


def test_ws_reject_logs_missing_or_mismatch_without_the_token(caplog, settings) -> None:
    client = TestClient(create_app(settings, FakeFactory()))
    with caplog.at_level(logging.INFO, logger="uvicorn.error"):
        with client.websocket_connect("/ws") as ws:
            assert ws.receive_json()["code"] == "unauthorized"
            with pytest.raises(WebSocketDisconnect):
                ws.receive_json()
        with client.websocket_connect(
            "/ws",
            headers={"Authorization": "Bearer wrong-token"},
        ) as ws:
            assert ws.receive_json()["code"] == "unauthorized"
            with pytest.raises(WebSocketDisconnect):
                ws.receive_json()
    messages = [
        record.getMessage()
        for record in caplog.records
        if record.name == "uvicorn.error"
    ]
    assert messages.count("ws reject missing") == 1
    assert messages.count("ws reject mismatch") == 1
    for message in messages:
        assert "wrong-token" not in message
        assert "device-token" not in message


def test_query_token_is_ignored(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect("/ws?token=device-token-a") as ws:
        body = ws.receive_json()
        assert body["code"] == "unauthorized"
        with pytest.raises(WebSocketDisconnect):
            ws.receive_json()
    assert factory.links == []


def test_header_token_streams_audio_and_text(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    pcm = (b"\x00\x01" * 160)
    with client.websocket_connect(
        "/ws",
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
    created = [item for item in texts if item.get("type") == "response.create"]
    assert created
    assert texts.index({"type": "input_audio_buffer.commit"}) < texts.index(created[0])
    assert "Vad är 5 plus 5?" in created[0]["response"]["instructions"]
    texts = [message["text"] for message in messages if message.get("text")]
    assert any("Hej" in text for text in texts)
    assert any(message.get("bytes") for message in messages)


def test_downlink_audio_is_split_below_the_stick_limit(settings) -> None:
    # 24 kHz in, 16 kHz out: 9000 samples become 6000 samples, 12000 bytes.
    factory = FakeFactory(downlink_pcm=b"\x10\x00" * 9000)
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down"})
        ws.send_bytes(b"\x00\x00" * 320)
        ws.send_json({"type": "ptt", "state": "up"})
        frames: list[bytes] = []
        saw_idle = False
        for _ in range(40):
            message = ws.receive()
            payload = message.get("bytes")
            if payload:
                frames.append(payload)
            text = message.get("text")
            if text and json.loads(text).get("state") == "idle":
                saw_idle = True
                break
    assert saw_idle
    assert len(frames) >= 4
    assert sum(len(item) for item in frames) > 10000
    assert all(0 < len(item) <= 3200 for item in frames)
    assert all(len(item) % 2 == 0 for item in frames)


def test_empty_ptt_does_not_commit(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect(
        "/ws",
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
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        body = ws.receive_json()
    assert body["code"] == "relay_not_configured"
    assert factory.links == []
