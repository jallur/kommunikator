"""xAI realtime WebSocket. Endpoint and auth come from the published docs."""

from __future__ import annotations

import json
from typing import Protocol
from urllib.parse import urlencode

import websockets

from .settings import Settings


def realtime_url(settings: Settings, conversation_id: str | None = None) -> str:
    params: list[tuple[str, str]] = [
        ("model", settings.xai_model),
        ("reasoning.effort", settings.reasoning_effort),
    ]
    if conversation_id:
        params.append(("conversation_id", conversation_id))
    return settings.xai_realtime_url + "?" + urlencode(params)


class XaiLink(Protocol):
    async def send(self, message: str | bytes) -> None: ...

    async def recv(self) -> str | bytes: ...

    async def close(self) -> None: ...


class XaiFactory(Protocol):
    async def connect(self, url: str, api_key: str) -> XaiLink: ...


class WebsocketsLink:
    def __init__(self, socket: websockets.ClientConnection) -> None:
        self._socket = socket

    async def send(self, message: str | bytes) -> None:
        await self._socket.send(message)

    async def recv(self) -> str | bytes:
        message = await self._socket.recv()
        if isinstance(message, str):
            return message
        return bytes(message)

    async def close(self) -> None:
        await self._socket.close()


class WebsocketsXaiFactory:
    async def connect(self, url: str, api_key: str) -> XaiLink:
        if not api_key:
            raise RuntimeError("XAI_API_KEY is empty")
        socket = await websockets.connect(
            url,
            additional_headers={"Authorization": f"Bearer {api_key}"},
        )
        return WebsocketsLink(socket)


def dumps(payload: dict) -> str:
    return json.dumps(payload, separators=(",", ":"))
