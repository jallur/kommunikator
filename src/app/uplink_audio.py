"""Uplink PCM that xAI actually receives, checked and optionally stored.

The bytes are one mono PCM16 little-endian frame per sample, at the rate in
session.update audio.input.format. KOMM_DUMP_AUDIO writes one WAV per turn.
"""

from __future__ import annotations

import wave
from datetime import datetime, timezone
from pathlib import Path


def uplink_matches_session(session_update: dict, rate: int, pcm: bytes) -> str | None:
    """None when the buffer is mono PCM16 and the session asks for that rate.

    audio/pcm in the realtime session is one little-endian int16 per sample.
    An odd length is not that framing. The rate has to be the one declared
    in session.update, because that is the clock xAI will use.
    """
    audio = (session_update.get("session") or {}).get("audio") or {}
    incoming = audio.get("input") or {}
    fmt = incoming.get("format") or {}
    problems: list[str] = []
    if fmt.get("type") != "audio/pcm":
        problems.append(f"type {fmt.get('type')!r}")
    if fmt.get("rate") != rate:
        problems.append(f"rate {fmt.get('rate')!r} != {rate}")
    if incoming.get("transport") != "binary":
        problems.append(f"transport {incoming.get('transport')!r}")
    if len(pcm) % 2:
        problems.append("odd length")
    return ", ".join(problems) or None


def write_uplink_wav(directory: str, device_id: str, turn_id: int, pcm: bytes, rate: int) -> Path:
    """Write mono 16-bit little-endian WAV. The header rate is the session rate."""
    if rate <= 0:
        raise ValueError("sample rate must be positive")
    if len(pcm) % 2:
        raise ValueError("pcm is not 16-bit frames")
    folder = Path(directory)
    folder.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    name = f"{_safe(device_id)}-{stamp}-turn{turn_id}.wav"
    path = folder / name
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(pcm)
    return path


def _safe(part: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in part)[:40]
    return cleaned or "device"
