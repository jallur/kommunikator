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
    assert texts[0]["session"]["turn_detection"] is None
    assert texts[0]["session"]["audio"]["output"]["format"]["rate"] == 16000
    assert any(isinstance(item, bytes) and item for item in sent)
    assert {"type": "input_audio_buffer.commit"} in texts
    created = [item for item in texts if item.get("type") == "response.create"]
    assert created
    assert texts.index({"type": "input_audio_buffer.commit"}) < texts.index(created[0])
    assert "Vad är 5 plus 5?" in created[0]["response"]["instructions"]
    texts = [message["text"] for message in messages if message.get("text")]
    assert any("Hej" in text for text in texts)
    assert any(message.get("bytes") for message in messages)


def test_playback_rate_is_what_xai_is_asked_for(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    hello = dict(HELLO)
    hello["playback_rate"] = 44100
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(hello)
        ready = ws.receive_json()
        assert ready["playback_rate"] == 44100
    texts = [json.loads(item) for item in factory.links[0].sent if isinstance(item, str)]
    assert texts[0]["session"]["audio"]["output"]["format"]["rate"] == 44100


def test_downlink_audio_is_split_below_the_stick_limit(settings) -> None:
    # Hello asks for 16 kHz, so the relay copies the 9000 samples (18000 bytes).
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


def test_turn_is_echoed_and_speaking_precedes_audio(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down", "turn": 7})
        ws.send_bytes(b"\x00\x01" * 160)
        ws.send_json({"type": "ptt", "state": "up", "turn": 7})
        ordered: list[dict] = []
        for _ in range(12):
            message = ws.receive()
            ordered.append(message)
            text = message.get("text")
            if text and json.loads(text).get("state") == "idle":
                break
    states = []
    saw_audio = False
    speaking_before_audio = False
    for message in ordered:
        text = message.get("text")
        if message.get("bytes"):
            saw_audio = True
        if not text:
            continue
        body = json.loads(text)
        if body.get("type") != "status":
            continue
        assert body["turn"] == 7
        states.append(body["state"])
        if body["state"] == "speaking" and not saw_audio:
            speaking_before_audio = True
    assert states == ["thinking", "speaking", "idle"]
    assert speaking_before_audio
    assert saw_audio


def test_relay_reports_the_commit_without_secrets(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    pcm = b"\x00\x01" * 160
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down", "turn": 7})
        ws.send_bytes(pcm)
        ws.send_json({"type": "ptt", "state": "up", "turn": 7})
        bodies: list[dict] = []
        for _ in range(16):
            message = ws.receive()
            text = message.get("text")
            if not text:
                continue
            body = json.loads(text)
            bodies.append(body)
            if body.get("type") == "status" and body.get("state") == "idle":
                break
    commit = [body for body in bodies if body.get("event") == "commit"]
    answer = [body for body in bodies if body.get("event") == "answer"]
    assert commit == [{"type": "relay", "event": "commit", "turn": 7, "bytes": len(pcm)}]
    assert answer == [{"type": "relay", "event": "answer", "turn": 7, "chars": len("Vad är 5 plus 5?")}]
    blob = json.dumps(bodies)
    assert "xai-test-key" not in blob
    assert "device-token" not in blob


def test_ptt_up_for_another_turn_does_not_commit(settings) -> None:
    factory = FakeFactory()
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down", "turn": 7})
        ws.send_bytes(b"\x00\x01" * 160)
        ws.send_json({"type": "ptt", "state": "up", "turn": 8})

    texts = [json.loads(item) for item in factory.links[0].sent if isinstance(item, str)]
    assert not any(item.get("type") == "input_audio_buffer.commit" for item in texts)
    assert not any(item.get("type") == "response.create" for item in texts)


def test_server_opens_the_reply_without_a_second_create(settings) -> None:
    factory = FakeFactory(vad_reply=True)
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down", "turn": 3})
        ws.send_bytes(b"\x00\x01" * 160)
        ws.send_json({"type": "ptt", "state": "up", "turn": 3})
        saw_audio = False
        states: list[str] = []
        for _ in range(16):
            message = ws.receive()
            if message.get("bytes"):
                saw_audio = True
            text = message.get("text")
            if not text:
                continue
            body = json.loads(text)
            if body.get("type") == "status":
                states.append(body["state"])
            if body.get("type") == "status" and body.get("state") == "idle":
                break
    texts = [json.loads(item) for item in factory.links[0].sent if isinstance(item, str)]
    assert any(item.get("type") == "input_audio_buffer.commit" for item in texts)
    assert not any(item.get("type") == "response.create" for item in texts)
    assert saw_audio
    assert "speaking" in states


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


def _texts(factory: FakeFactory) -> list[dict]:
    return [json.loads(item) for item in factory.links[0].sent if isinstance(item, str)]


def _read_until(ws, pred, limit: int = 24) -> list[dict]:
    found = []
    for _ in range(limit):
        message = ws.receive()
        text = message.get("text")
        body = json.loads(text) if text else {}
        found.append(body if text else {"bytes": len(message.get("bytes") or b"")})
        if pred(body):
            return found
    raise AssertionError(found)


def test_late_transcript_from_the_previous_press_is_not_quoted(settings) -> None:
    factory = FakeFactory(mode="race")
    client = TestClient(create_app(settings, factory))
    pcm = b"\x00\x01" * 160
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down", "turn": 1})
        ws.send_bytes(pcm)
        ws.send_json({"type": "ptt", "state": "up", "turn": 1})
        ws.send_json({"type": "ptt", "state": "down", "turn": 2})
        ws.send_bytes(pcm)
        ws.send_json({"type": "ptt", "state": "up", "turn": 2})
        seen = _read_until(ws, lambda body: body.get("text") == "andra frågan")
    created = [item for item in _texts(factory) if item.get("type") == "response.create"]
    assert len(created) == 1
    instructions = created[0]["response"]["instructions"]
    assert "andra frågan" in instructions
    assert "första frågan" not in instructions
    texts = [body.get("text") for body in seen]
    assert "andra frågan" in texts
    assert "första frågan" not in texts


def test_partial_transcript_is_quoted_when_completed_does_not_arrive(monkeypatch, settings) -> None:
    monkeypatch.setattr("app.bridge._TRANSCRIPT_WAIT_S", 0.2)
    factory = FakeFactory(mode="partial")
    client = TestClient(create_app(settings, factory))
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down", "turn": 4})
        ws.send_bytes(b"\x00\x01" * 160)
        ws.send_json({"type": "ptt", "state": "up", "turn": 4})
        _read_until(
            ws,
            lambda body: body.get("type") == "relay" and body.get("event") == "answer",
        )
    created = [item for item in _texts(factory) if item.get("type") == "response.create"]
    assert len(created) == 1
    assert "delvis fråga" in created[0]["response"]["instructions"]


def test_late_response_done_does_not_drop_the_next_response(settings) -> None:
    factory = FakeFactory(mode="late_done")
    client = TestClient(create_app(settings, factory))
    pcm = b"\x00\x01" * 160
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down", "turn": 1})
        ws.send_bytes(pcm)
        ws.send_json({"type": "ptt", "state": "up", "turn": 1})
        first = _read_until(ws, lambda body: body.get("text") == "Hej" or body.get("delta") == "Hej" or (
            body.get("type") == "transcript.assistant" and body.get("text") == "Hej"
        ))
        assert not any(body.get("state") == "idle" for body in first)
        ws.send_json({"type": "ptt", "state": "down", "turn": 2})
        ws.send_bytes(pcm)
        ws.send_json({"type": "ptt", "state": "up", "turn": 2})
        second = _read_until(
            ws,
            lambda body: body.get("type") == "transcript.assistant" and body.get("text") == "Hej",
        )
        assert not any(body.get("state") == "idle" for body in second)
        factory.links[0].push(
            {"type": "error", "error": {"code": "synk", "message": "synk"}}
        )
        synced = _read_until(ws, lambda body: body.get("code") == "synk")
        assert not any(body.get("state") == "idle" for body in synced)
        ws.send_json({"type": "ptt", "state": "down", "turn": 3})
        ws.send_json({"type": "ping"})
        pong = ws.receive_json()
        assert pong["type"] == "pong"
    cancels = [item for item in _texts(factory) if item.get("type") == "response.cancel"]
    assert len(cancels) == 2


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
