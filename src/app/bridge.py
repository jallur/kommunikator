"""Bridge one device socket to one xAI realtime socket."""

from __future__ import annotations

import array
import asyncio
import json
import logging
from collections import deque

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
from .uplink_audio import uplink_matches_session, write_uplink_wav
from .xai import XaiFactory, XaiLink, dumps, realtime_url

log = logging.getLogger("kommunikator.bridge")

# The stick's WebSocket library drops the TCP connection when one frame is
# larger than 15 KiB, and that local close has no close code. Keep each
# downlink frame well under that limit.
_DEVICE_AUDIO_CHUNK = 3200
# grok-transcribe can finish after the commit. Past this, quote the latest
# partial that belongs to the committed item.
_TRANSCRIPT_WAIT_S = 2.5


class _Turn:
    def __init__(self) -> None:
        self.id = 0
        # None means "do not play". 0 is a real turn for a client that omits the field.
        self.playing_id: int | None = None
        self.capturing = False
        self.sent_audio = False
        self.response_active = False
        self.announced_speaking = False
        self.reply_pending = False
        self.user_transcript = ""
        # item_id from input_audio_buffer.committed for the open turn.
        # Transcript events for any other item stay off this quote.
        self.item_id: str | None = None
        self.provisional: dict[str, str] = {}
        self.ready_items: set[str] = set()
        self.pending_commits: deque[int] = deque()
        self.response_id: str | None = None
        self.ignored_responses: set[str] = set()
        self.device_bytes = 0
        self.answered_id: int | None = None
        self.level_peak = 0
        self.level_sq = 0
        self.level_n = 0
        self.uplink = bytearray()
        self.format_error: str | None = None
        self.dumped = False
        self.level_logged = False
        self.transcript_ready = asyncio.Event()
        self.reply_task: asyncio.Task[None] | None = None


def _note_level(turn: _Turn, pcm: bytes) -> None:
    """Peak and energy of the PCM we hand to xAI. Samples are not logged."""
    count = len(pcm) // 2
    if count == 0:
        return
    samples = array.array("h")
    samples.frombytes(pcm[: count * 2])
    peak = turn.level_peak
    total = turn.level_sq
    for sample in samples:
        magnitude = -sample if sample < 0 else sample
        if magnitude > peak:
            peak = magnitude
        total += sample * sample
    turn.level_peak = peak
    turn.level_sq = total
    turn.level_n += count


def _turn_id(payload: dict) -> int:
    raw = payload.get("turn", 0)
    if isinstance(raw, bool) or not isinstance(raw, int) or raw < 0:
        return 0
    return raw


def _brief(event: dict) -> str:
    kind = str(event.get("type") or "unknown")
    if kind != "error":
        return kind
    err = event.get("error") if isinstance(event.get("error"), dict) else {}
    message = str(err.get("message", "")).replace("\n", " ")[:160]
    return f"error {err.get('code', '')} {message}".rstrip()


def _stamp(message: dict, turn_id: int) -> dict:
    stamped = dict(message)
    stamped["turn"] = turn_id
    return stamped


def _item_id(event: dict) -> str | None:
    raw = event.get("item_id")
    if isinstance(raw, str) and raw:
        return raw
    return None


def _response_id(event: dict) -> str | None:
    raw = event.get("response_id")
    if isinstance(raw, str) and raw:
        return raw
    response = event.get("response")
    if isinstance(response, dict):
        raw = response.get("id")
        if isinstance(raw, str) and raw:
            return raw
    return None


def _user_line(turn: _Turn, text: str, final: bool) -> dict:
    return {
        "type": "transcript.user",
        "text": text,
        "final": final,
        "cumulative": True,
        "turn": turn.id,
    }


def _bind_committed(turn: _Turn, event: dict) -> dict | None:
    """Attach this commit's item to the turn that sent it.

    Completed events from two presses can pass each other. The item id on
    input_audio_buffer.committed is what makes the quote belong to one press.
    """
    item_id = _item_id(event)
    if not turn.pending_commits:
        log.info("committed without a pending turn")
        return None
    owner = turn.pending_commits.popleft()
    if not item_id:
        log.info("committed without item_id turn %s", owner)
        return None
    if owner != turn.id:
        log.info("committed for turn %s ignored, current is %s", owner, turn.id)
        turn.provisional.pop(item_id, None)
        turn.ready_items.discard(item_id)
        return None
    turn.item_id = item_id
    stashed = turn.provisional.pop(item_id, "")
    final = item_id in turn.ready_items
    if stashed:
        turn.user_transcript = stashed
    if final:
        turn.transcript_ready.set()
    log.info("committed turn %s item %s", owner, item_id)
    if not stashed:
        return None
    return _user_line(turn, stashed, final)


def _take_user_transcript(turn: _Turn, event: dict) -> dict | None:
    """Keep a transcript only when its item is the one this turn committed."""
    heard = user_transcript(event)
    if heard is None:
        return None
    text, final = heard
    item_id = _item_id(event)
    if not item_id:
        log.info("user transcript without item_id turn %s", turn.id)
        return None
    if turn.item_id == item_id:
        if text:
            turn.user_transcript = text
        if final:
            turn.transcript_ready.set()
        if not text:
            return None
        return _user_line(turn, text, final)
    if text and (turn.capturing or turn.pending_commits):
        turn.provisional[item_id] = text
    if final:
        turn.ready_items.add(item_id)
    return None


def _done_applies(turn: _Turn, event: dict) -> bool:
    """A late done from a cancelled response must not clear the next one."""
    rid = _response_id(event)
    if rid and rid in turn.ignored_responses:
        return False
    if turn.response_id:
        return rid == turn.response_id
    return rid is None


def _assistant_belongs(turn: _Turn, event: dict) -> bool:
    rid = _response_id(event)
    if rid and rid in turn.ignored_responses:
        return False
    if rid and turn.response_id:
        return rid == turn.response_id
    return True


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
    log.info(
        "session %s capture %s playback %s",
        hello.device_id or "-",
        hello.capture_rate,
        hello.playback_rate,
    )
    link = await factory.connect(
        realtime_url(settings, memory.get(token)),
        settings.xai_api_key,
    )
    send_lock = asyncio.Lock()
    xai_lock = asyncio.Lock()

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

    session = build_session_update(settings, hello.playback_rate)
    declared = session["session"]["audio"]["input"]["format"]
    log.info(
        "uplink format pcm16le mono %s session %s %s dump %s",
        settings.xai_input_rate,
        declared.get("type"),
        declared.get("rate"),
        settings.dump_audio_dir if settings.dump_audio else "off",
    )
    try:
        async with xai_lock:
            await link.send(dumps(session))
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
        downlink = Pcm16Resampler(hello.playback_rate, hello.playback_rate)
        turn = _Turn()
        async with asyncio.TaskGroup() as group:
            group.create_task(
                _device_to_xai(
                    device,
                    link,
                    uplink,
                    turn,
                    send_json,
                    settings,
                    xai_lock,
                    hello.device_id,
                    session,
                )
            )
            group.create_task(_xai_to_device(link, downlink, turn, send_json, send_bytes, xai_lock))
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


async def _answer_after_transcript(link, settings, turn, xai_lock, turn_id: int, send_json) -> None:
    try:
        await asyncio.wait_for(turn.transcript_ready.wait(), timeout=_TRANSCRIPT_WAIT_S)
    except TimeoutError:
        log.info(
            "user transcript missing turn %s partial %s",
            turn_id,
            len(turn.user_transcript.strip()),
        )
    skipped = False
    adopted = False
    quote = ""
    try:
        async with xai_lock:
            # Same lock as clear/cancel. A late response.create must not land
            # after the next turn has wiped the buffer. The live API still
            # opens a response for real speech even with turn_detection null;
            # that audio is the reply, so a second create is not sent.
            if turn.answered_id == turn_id:
                adopted = True
                quote = turn.user_transcript.strip()
            elif not turn.reply_pending or turn.id != turn_id:
                skipped = True
            elif turn.response_active:
                turn.reply_pending = False
                turn.playing_id = turn_id
                turn.answered_id = turn_id
                adopted = True
                quote = turn.user_transcript.strip()
            else:
                turn.reply_pending = False
                turn.playing_id = turn_id
                turn.answered_id = turn_id
                quote = turn.user_transcript.strip()
                await link.send(dumps(build_response_create(settings, quote)))
    except asyncio.CancelledError:
        raise
    except Exception:
        log.exception("response.create failed turn %s", turn_id)
        await send_json({"type": "relay", "event": "answer_failed", "turn": turn_id})
        return
    if skipped:
        log.info("answer skipped turn %s", turn_id)
        await send_json({"type": "relay", "event": "skip", "turn": turn_id})
        return
    if adopted:
        log.info("response adopted turn %s chars %s", turn_id, len(quote))
    else:
        log.info("response.create turn %s chars %s", turn_id, len(quote))
    await send_json({"type": "relay", "event": "answer", "turn": turn_id, "chars": len(quote)})


def _log_level(
    turn: _Turn, turn_id: int, settings: Settings, session: dict, force: bool = False
) -> None:
    if turn.level_logged:
        return
    if not force and turn.level_n == 0 and not turn.uplink and not turn.format_error:
        return
    turn.level_logged = True
    rms = int((turn.level_sq / turn.level_n) ** 0.5) if turn.level_n else 0
    declared = (session.get("session") or {}).get("audio", {}).get("input", {}).get("format", {})
    log.info(
        "uplink level turn %s peak %s rms %s samples %s pcm16le mono %s session %s %s",
        turn_id,
        turn.level_peak,
        rms,
        turn.level_n,
        settings.xai_input_rate,
        declared.get("type"),
        declared.get("rate"),
    )
    if turn.format_error:
        log.error("uplink format mismatch turn %s %s", turn_id, turn.format_error)


def _save_uplink(settings: Settings, turn: _Turn, device_id: str | None) -> None:
    if not settings.dump_audio or turn.dumped or not turn.uplink:
        return
    pcm = bytes(turn.uplink)
    turn.uplink.clear()
    turn.dumped = True
    try:
        path = write_uplink_wav(
            settings.dump_audio_dir,
            device_id or "device",
            turn.id,
            pcm,
            settings.xai_input_rate,
        )
    except Exception:
        log.exception("uplink wav failed turn %s", turn.id)
        return
    log.info("uplink wav turn %s %s", turn.id, path)


async def _device_to_xai(
    device, link, uplink, turn, send_json, settings, xai_lock, device_id, session
) -> None:
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
                    if turn.device_bytes == 0:
                        log.info("uplink first %s bytes turn %s", len(data), turn.id)
                    turn.device_bytes += len(data)
                    pcm = uplink.process(data)
                    if pcm:
                        problem = uplink_matches_session(session, settings.xai_input_rate, pcm)
                        if problem:
                            turn.format_error = problem
                        _note_level(turn, pcm)
                        async with xai_lock:
                            if turn.capturing:
                                await link.send(pcm)
                                turn.sent_audio = True
                                if settings.dump_audio:
                                    turn.uplink.extend(pcm)
                continue
            text = incoming.get("text")
            if not text:
                continue
            payload = json.loads(text)
            kind = payload.get("type")
            if kind == "ping":
                await send_json({"type": "pong"})
            elif kind == "ptt" and payload.get("state") == "down":
                _log_level(turn, turn.id, settings, session)
                _save_uplink(settings, turn, device_id)
                turn_id = _turn_id(payload)
                log.info("ptt down turn %s", turn_id)
                turn.capturing = True
                turn.sent_audio = False
                turn.device_bytes = 0
                turn.level_peak = 0
                turn.level_sq = 0
                turn.level_n = 0
                turn.uplink.clear()
                turn.format_error = None
                turn.dumped = False
                turn.level_logged = False
                async with xai_lock:
                    turn.user_transcript = ""
                    turn.item_id = None
                    turn.provisional.clear()
                    turn.ready_items.clear()
                    turn.transcript_ready.clear()
                    turn.reply_pending = False
                    turn.answered_id = None
                    turn.id = turn_id
                    turn.playing_id = None
                    turn.announced_speaking = False
                    if turn.response_id:
                        turn.ignored_responses.add(turn.response_id)
                    if turn.response_active:
                        await link.send(dumps({"type": "response.cancel"}))
                        turn.response_active = False
                    turn.response_id = None
                    await link.send(dumps({"type": "input_audio_buffer.clear"}))
                if turn.reply_task is not None and not turn.reply_task.done():
                    turn.reply_task.cancel()
            elif kind == "ptt" and payload.get("state") == "up":
                up_id = _turn_id(payload)
                if up_id != turn.id:
                    log.info("ptt up ignored for turn %s, current is %s", up_id, turn.id)
                    continue
                turn.capturing = False
                audio_bytes = turn.device_bytes
                if not turn.sent_audio:
                    log.info("ptt up turn %s bytes %s skip", up_id, audio_bytes)
                    await send_json(
                        {"type": "relay", "event": "empty", "turn": up_id, "bytes": audio_bytes}
                    )
                    continue
                committed = False
                async with xai_lock:
                    if turn.id == up_id and turn.sent_audio:
                        # Drop a quote that belongs to an earlier item. Partials
                        # captured while the button was down stay in provisional
                        # until this commit's item_id arrives.
                        turn.user_transcript = ""
                        turn.item_id = None
                        turn.transcript_ready.clear()
                        turn.pending_commits.append(up_id)
                        await link.send(dumps({"type": "input_audio_buffer.commit"}))
                        turn.reply_pending = True
                        turn.sent_audio = False
                        committed = True
                if not committed:
                    log.info("ptt up turn %s bytes %s skip", up_id, audio_bytes)
                    await send_json(
                        {"type": "relay", "event": "empty", "turn": up_id, "bytes": audio_bytes}
                    )
                    continue
                _log_level(turn, up_id, settings, session, force=True)
                _save_uplink(settings, turn, device_id)
                log.info("ptt up turn %s bytes %s commit", up_id, audio_bytes)
                await send_json(
                    {"type": "relay", "event": "commit", "turn": up_id, "bytes": audio_bytes}
                )
                turn.reply_task = asyncio.create_task(
                    _answer_after_transcript(link, settings, turn, xai_lock, up_id, send_json)
                )
    finally:
        _log_level(turn, turn.id, settings, session)
        _save_uplink(settings, turn, device_id)
        turn.reply_pending = False
        if turn.reply_task is not None and not turn.reply_task.done():
            turn.reply_task.cancel()


async def _xai_to_device(link, downlink, turn, send_json, send_bytes, xai_lock) -> None:
    while True:
        raw = await link.recv()
        if isinstance(raw, bytes):
            async with xai_lock:
                play = turn.playing_id
            if play is None:
                log.info("xai audio %s bytes dropped", len(raw))
                continue
            pcm = downlink.process(raw)
            if not pcm:
                continue
            # The stick drops binary until a speaking status for this turn
            # has been seen. The status has to be the previous frame.
            if not turn.announced_speaking:
                turn.announced_speaking = True
                turn.response_active = True
                log.info("downlink start %s bytes turn %s", len(pcm), play)
                await send_json({"type": "status", "state": "speaking", "turn": play})
            await send_bytes(pcm)
            continue
        event = json.loads(raw)
        kind = str(event.get("type") or "")
        log.info("xai %s turn %s", _brief(event), turn.id)
        if kind == "conversation.created":
            continue
        # Set this before taking the lock. The reply task adopts a response
        # the server already opened, and it reads the flag while commit
        # still holds the lock.
        rid = _response_id(event)
        created_ignored = kind == "response.created" and bool(rid and rid in turn.ignored_responses)
        if kind == "response.created" and not created_ignored:
            turn.response_active = True
            turn.announced_speaking = False
            if rid:
                turn.response_id = rid
        publish: list[dict] = []
        clear_playing = False
        async with xai_lock:
            done_applies = _done_applies(turn, event) if kind == "response.done" else False
            if kind == "input_audio_buffer.committed":
                message = _bind_committed(turn, event)
                if message is not None:
                    publish.append(message)
            elif user_transcript(event) is not None:
                message = _take_user_transcript(turn, event)
                if message is not None:
                    publish.append(message)
            elif kind == "response.created" and not created_ignored and turn.reply_pending:
                turn.playing_id = turn.id
                turn.reply_pending = False
                turn.answered_id = turn.id
            elif kind == "response.done" and done_applies:
                turn.response_active = False
                turn.response_id = None
                clear_playing = True
            elif kind == "response.done":
                log.info(
                    "response.done ignored %s active %s",
                    rid or "-",
                    turn.response_id or "-",
                )
            play = turn.playing_id
            label = turn.id if play is None else play
        for message in publish:
            await send_json(message)
        mapped = [
            message
            for message in map_server_event(event)
            if message.get("type") != "transcript.user"
        ]
        if kind == "response.done" and not done_applies:
            mapped = [message for message in mapped if message.get("state") != "idle"]
        if created_ignored:
            mapped = [message for message in mapped if message.get("type") != "status"]
        if not _assistant_belongs(turn, event):
            mapped = [
                message for message in mapped if message.get("type") != "transcript.assistant"
            ]
        for message in mapped:
            if message.get("type") == "status" and play is None:
                continue
            await send_json(_stamp(message, label))
        if clear_playing:
            async with xai_lock:
                if turn.playing_id == play:
                    turn.playing_id = None
                    turn.announced_speaking = False


def _expected_disconnect(group: ExceptionGroup) -> bool:
    return all(
        isinstance(exc, (WebSocketDisconnect, ConnectionError, asyncio.CancelledError))
        for exc in group.exceptions
    )
