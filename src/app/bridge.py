"""Bridge one device socket to one xAI realtime socket."""

from __future__ import annotations

import asyncio
import json
import logging

from fastapi import WebSocket, WebSocketDisconnect

from .memory import ConversationMemory
from .protocol import (
    ProtocolError,
    build_response_create,
    build_session_update,
    map_server_event,
    parse_hello,
    user_transcript,
)
from .resample import Pcm16Resampler
from .settings import Settings
from .xai import XaiFactory, XaiLink, dumps, realtime_url

log = logging.getLogger("kommunikator.bridge")

# The stick's WebSocket library drops the TCP connection when one frame is
# larger than 15 KiB, and that local close has no close code. Keep each
# downlink frame well under that limit.
_DEVICE_AUDIO_CHUNK = 3200


class _Turn:
    def __init__(self) -> None:
        self.capturing = False
        self.sent_audio = False
        self.response_active = False
        self.announced_speaking = False
        self.reply_pending = False
        self.user_transcript = ""
        self.transcript_ready = asyncio.Event()
        self.reply_task: asyncio.Task[None] | None = None


async def bridge(
    device: WebSocket,
    token: str,
    settings: Settings,
    factory: XaiFactory,
    memory: ConversationMemory,
) -> None:
    if not settings.xai_api_key:
        await device.send_json(
            {
                "type": "error",
                "code": "relay_not_configured",
                "message": "XAI_API_KEY saknas i relayn",
            }
        )
        await device.close(code=1011)
        return

    hello = await _read_hello(device)
    link = await factory.connect(
        realtime_url(settings, memory.get(token)),
        settings.xai_api_key,
    )
    send_lock = asyncio.Lock()

    async def send_json(payload: dict) -> None:
        async with send_lock:
            await device.send_json(payload)

    async def send_bytes(payload: bytes) -> None:
        if not payload:
            return
        async with send_lock:
            view = memoryview(payload)
            for start in range(0, len(view), _DEVICE_AUDIO_CHUNK):
                await device.send_bytes(bytes(view[start : start + _DEVICE_AUDIO_CHUNK]))

    try:
        await link.send(dumps(build_session_update(settings)))
        conversation_id = await _wait_session_updated(link, token, memory)
        await send_json(
            {
                "type": "hello.ok",
                "capture_rate": settings.xai_input_rate,
                "playback_rate": hello.playback_rate,
                "format": "pcm16le",
                "conversation_id": conversation_id,
            }
        )
        uplink = Pcm16Resampler(hello.capture_rate, settings.xai_input_rate)
        downlink = Pcm16Resampler(settings.xai_output_rate, hello.playback_rate)
        turn = _Turn()
        async with asyncio.TaskGroup() as group:
            group.create_task(_device_to_xai(device, link, uplink, turn, send_json, settings))
            group.create_task(_xai_to_device(link, downlink, turn, send_json, send_bytes))
    except ExceptionGroup as group:
        if not _expected_disconnect(group):
            log.warning("session ended: %s", type(group.exceptions[0]).__name__)
            raise
    except (WebSocketDisconnect, ConnectionError):
        return
    finally:
        await link.close()


async def _read_hello(device: WebSocket):
    while True:
        incoming = await asyncio.wait_for(device.receive(), timeout=10)
        if incoming.get("type") == "websocket.disconnect":
            raise WebSocketDisconnect(code=1000)
        if incoming.get("bytes") is not None:
            raise ProtocolError("binary before hello")
        text = incoming.get("text")
        if not text:
            continue
        payload = json.loads(text)
        if payload.get("type") == "ping":
            await device.send_json({"type": "pong"})
            continue
        return parse_hello(payload)


async def _wait_session_updated(link: XaiLink, token: str, memory: ConversationMemory) -> str | None:
    conversation_id: str | None = None
    while True:
        raw = await asyncio.wait_for(link.recv(), timeout=10)
        if isinstance(raw, bytes):
            continue
        event = json.loads(raw)
        kind = event.get("type")
        if kind == "conversation.created":
            conversation = event.get("conversation")
            if isinstance(conversation, dict) and isinstance(conversation.get("id"), str):
                conversation_id = conversation["id"]
                memory.put(token, conversation_id)
        elif kind == "session.updated":
            return conversation_id
        elif kind == "error":
            raise ConnectionError("xAI session.update failed")


async def _answer_after_transcript(link, settings, turn) -> None:
    try:
        await asyncio.wait_for(turn.transcript_ready.wait(), timeout=2.5)
    except TimeoutError:
        log.info("user transcript missing, answering the audio alone")
    if not turn.reply_pending:
        return
    turn.reply_pending = False
    quote = turn.user_transcript.strip()
    if quote:
        log.info("response follows transcript (%d chars)", len(quote))
    await link.send(dumps(build_response_create(settings, quote)))


async def _device_to_xai(device, link, uplink, turn, send_json, settings) -> None:
    try:
        while True:
            try:
                incoming = await device.receive()
            except WebSocketDisconnect:
                raise
            if incoming.get("type") == "websocket.disconnect":
                raise WebSocketDisconnect(code=incoming.get("code", 1000))
            data = incoming.get("bytes")
            if data is not None:
                if turn.capturing:
                    pcm = uplink.process(data)
                    if pcm:
                        await link.send(pcm)
                        turn.sent_audio = True
                continue
            text = incoming.get("text")
            if not text:
                continue
            payload = json.loads(text)
            kind = payload.get("type")
            if kind == "ping":
                await send_json({"type": "pong"})
            elif kind == "ptt" and payload.get("state") == "down":
                turn.reply_pending = False
                if turn.reply_task is not None and not turn.reply_task.done():
                    turn.reply_task.cancel()
                turn.capturing = True
                turn.sent_audio = False
                turn.user_transcript = ""
                turn.transcript_ready.clear()
                if turn.response_active:
                    await link.send(dumps({"type": "response.cancel"}))
                    turn.response_active = False
                await link.send(dumps({"type": "input_audio_buffer.clear"}))
            elif kind == "ptt" and payload.get("state") == "up":
                turn.capturing = False
                if turn.sent_audio:
                    turn.reply_pending = True
                    turn.user_transcript = ""
                    turn.transcript_ready.clear()
                    await link.send(dumps({"type": "input_audio_buffer.commit"}))
                    turn.reply_task = asyncio.create_task(
                        _answer_after_transcript(link, settings, turn)
                    )
                    turn.sent_audio = False
    finally:
        turn.reply_pending = False
        if turn.reply_task is not None and not turn.reply_task.done():
            turn.reply_task.cancel()


async def _xai_to_device(link, downlink, turn, send_json, send_bytes) -> None:
    while True:
        raw = await link.recv()
        if isinstance(raw, bytes):
            pcm = downlink.process(raw)
            if pcm:
                await send_bytes(pcm)
            if not turn.announced_speaking:
                turn.announced_speaking = True
                turn.response_active = True
                await send_json({"type": "status", "state": "speaking"})
            continue
        event = json.loads(raw)
        heard = user_transcript(event)
        if heard is not None:
            text, final = heard
            if text:
                turn.user_transcript = text
            if final:
                turn.transcript_ready.set()
        if event.get("type") == "response.created":
            turn.response_active = True
            turn.announced_speaking = False
        elif event.get("type") == "response.done":
            turn.response_active = False
            turn.announced_speaking = False
        elif event.get("type") == "conversation.created":
            continue
        for message in map_server_event(event):
            await send_json(message)


def _expected_disconnect(group: ExceptionGroup) -> bool:
    return all(
        isinstance(exc, (WebSocketDisconnect, ConnectionError, asyncio.CancelledError))
        for exc in group.exceptions
    )
