"""FastAPI entrypoint. Devices connect here. xAI is reached only from here."""

from __future__ import annotations

import logging

from fastapi import FastAPI, Header, HTTPException, WebSocket, WebSocketDisconnect

from .bridge import bridge
from .memory import ConversationMemory
from .missions import GrokBotWebhookDispatcher, Mission, MissionNotConfigured
from .protocol import ProtocolError
from .settings import Settings
from .tokens import bearer_from_header, token_allowed
from .xai import WebsocketsXaiFactory, XaiFactory

log = logging.getLogger("kommunikator")


def create_app(
    settings: Settings | None = None,
    xai_factory: XaiFactory | None = None,
) -> FastAPI:
    cfg = settings or Settings.from_env()
    factory = xai_factory or WebsocketsXaiFactory()
    memory = ConversationMemory()
    missions = GrokBotWebhookDispatcher(cfg.grok_bot_webhook_url)
    app = FastAPI(title="kommunikator-relay", docs_url=None, redoc_url=None)
    app.state.settings = cfg
    app.state.xai_factory = factory
    app.state.memory = memory

    @app.get("/health")
    @app.get("/healthz")
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    @app.websocket("/ws")
    async def session(socket: WebSocket) -> None:
        await socket.accept()
        # Query ?token= is not accepted. Uvicorn logs the request URL.
        token = bearer_from_header(socket.headers.get("authorization"))
        if not token_allowed(token, cfg.device_tokens):
            await socket.send_json(
                {
                    "type": "error",
                    "code": "unauthorized",
                    "message": "ogiltig enhetstoken",
                }
            )
            await socket.close(code=1008)
            return
        try:
            await bridge(socket, token, cfg, factory, memory)
        except ProtocolError as exc:
            await socket.send_json({"type": "error", "code": "protocol", "message": str(exc)})
            await socket.close(code=1003)
        except WebSocketDisconnect:
            return
        except Exception:
            log.exception("session failed")
            try:
                await socket.close(code=1011)
            except RuntimeError:
                return

    @app.post("/v1/missions", status_code=501)
    async def create_mission(
        mission: Mission,
        authorization: str | None = Header(default=None),
    ) -> dict[str, str]:
        token = bearer_from_header(authorization)
        if not token_allowed(token, cfg.device_tokens):
            raise HTTPException(status_code=401, detail="ogiltig enhetstoken")
        try:
            await missions.dispatch(mission)
        except MissionNotConfigured as exc:
            raise HTTPException(status_code=501, detail=str(exc)) from exc
        return {"status": "accepted"}

    return app


app = create_app()
