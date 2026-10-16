import json
import struct
import wave
from dataclasses import replace

from fastapi.testclient import TestClient

from app.main import create_app
from app.protocol import build_session_update
from app.settings import Settings
from app.uplink_audio import uplink_matches_session, write_uplink_wav
from tests.fakes import FakeFactory
from tests.test_session import HELLO


def test_session_pcm_matches_and_odd_length_does_not(settings) -> None:
    session = build_session_update(settings)
    assert uplink_matches_session(session, 16000, b"\x00\x01\xff\xff") is None
    assert session["session"]["audio"]["input"]["format"] == {"type": "audio/pcm", "rate": 16000}
    session["session"]["audio"]["input"]["format"]["rate"] = 8000
    assert "8000" in (uplink_matches_session(session, 16000, b"\x00\x00") or "")
    session["session"]["audio"]["input"]["format"]["rate"] = 16000
    assert uplink_matches_session(session, 16000, b"\x00") == "odd length"


def test_wav_is_little_endian_mono_and_stays_in_the_directory(tmp_path) -> None:
    pcm = b"\x00\x01\x00\x00"
    path = write_uplink_wav(str(tmp_path), "../../etc/passwd", 4, pcm, 16000)
    assert path.parent == tmp_path
    blob = path.read_bytes()
    assert blob[:4] == b"RIFF" and blob[8:12] == b"WAVE"
    fmt_at = blob.find(b"fmt ")
    audio_format, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", blob, fmt_at + 8)
    assert (audio_format, channels, rate, byte_rate, align, bits) == (1, 1, 16000, 32000, 2, 16)
    data_at = blob.find(b"data")
    size = struct.unpack_from("<I", blob, data_at + 4)[0]
    assert blob[data_at + 8 : data_at + 8 + size] == pcm
    with wave.open(str(path), "rb") as handle:
        assert handle.getnchannels() == 1
        assert handle.getsampwidth() == 2
        assert handle.getframerate() == 16000
        assert handle.readframes(handle.getnframes()) == pcm


def test_dump_flag_is_off_unless_asked(monkeypatch) -> None:
    monkeypatch.setenv("XAI_API_KEY", "k")
    monkeypatch.setenv("DEVICE_TOKENS", "t")
    monkeypatch.setenv("XAI_REALTIME_URL", "wss://api.x.ai/v1/realtime")
    monkeypatch.setenv("XAI_MODEL", "grok-voice-latest")
    monkeypatch.setenv("XAI_VOICE", "eve")
    monkeypatch.setenv("XAI_INPUT_RATE", "16000")
    monkeypatch.setenv("XAI_OUTPUT_RATE", "24000")
    monkeypatch.setenv("XAI_REASONING_EFFORT", "none")
    monkeypatch.delenv("KOMM_DUMP_AUDIO", raising=False)
    monkeypatch.delenv("KOMM_DUMP_DIR", raising=False)
    assert Settings.from_env().dump_audio is False
    monkeypatch.setenv("KOMM_DUMP_AUDIO", "1")
    monkeypatch.setenv("KOMM_DUMP_DIR", "/tmp/hear")
    cfg = Settings.from_env()
    assert cfg.dump_audio is True
    assert cfg.dump_audio_dir == "/tmp/hear"


def _turn(client_settings, pcm: bytes, hello: dict | None = None) -> tuple[FakeFactory, list[dict]]:
    factory = FakeFactory()
    client = TestClient(create_app(client_settings, factory))
    with client.websocket_connect(
        "/ws",
        headers={"Authorization": "Bearer device-token-a"},
    ) as ws:
        ws.send_json(hello or HELLO)
        assert ws.receive_json()["type"] == "hello.ok"
        ws.send_json({"type": "ptt", "state": "down", "turn": 4})
        ws.send_bytes(pcm)
        ws.send_json({"type": "ptt", "state": "up", "turn": 4})
        for _ in range(16):
            message = ws.receive()
            text = message.get("text")
            if text and json.loads(text).get("state") == "idle":
                break
    return factory, [json.loads(item) for item in factory.links[0].sent if isinstance(item, str)]


def test_dump_writes_the_pcm_xai_received(tmp_path, settings) -> None:
    pcm = b"\x00\x01" * 160
    dumped = replace(settings, dump_audio=True, dump_audio_dir=str(tmp_path))
    factory, texts = _turn(dumped, pcm)
    sent = b"".join(item for item in factory.links[0].sent if isinstance(item, bytes))
    assert sent == pcm
    assert texts[0]["session"]["audio"]["input"]["format"] == {"type": "audio/pcm", "rate": 16000}
    files = list(tmp_path.glob("*.wav"))
    assert len(files) == 1
    assert files[0].name.startswith("stickc-plus2-")
    assert files[0].name.endswith("-turn4.wav")
    with wave.open(str(files[0]), "rb") as handle:
        assert handle.getnchannels() == 1
        assert handle.getsampwidth() == 2
        assert handle.getframerate() == 16000
        assert handle.readframes(handle.getnframes()) == sent


def test_dump_stays_off_without_the_flag(tmp_path, settings) -> None:
    quiet = replace(settings, dump_audio=False, dump_audio_dir=str(tmp_path))
    _turn(quiet, b"\x00\x01" * 160)
    assert list(tmp_path.glob("*.wav")) == []
